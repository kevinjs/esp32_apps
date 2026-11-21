/*
 * 双音源播放通路：本地 storage WAV + DLNA 投屏流
 *
 * 数据流：
 *   本地  storage:/music-*.wav
 *              └─ fread（8KB/次） ─┐
 *   投屏  http://... (MP3/FLAC)     │
 *              └─ esp_http_client_read（8KB/次，Range 拖动）
 *                                  └─ esp_audio_simple_dec_process（容器解析 + 解码）
 *                                        └─ PCM（内部 RAM）─► esp_codec_dev_write ─► I2S ─► ES8311
 *
 * 为什么用 simple decoder 而不是自己解容器：
 *   它内部带解析器，接受任意长度输入（读多少喂多少），换 MP3/FLAC 只改 dec_type，
 *   解码循环完全不动。
 *
 * 采样率：本地固定 48 kHz；投屏流可能是 44.1 kHz / 立体声，所以喇叭格式跟着
 *   解码器信息走（变化时 esp_codec_dev_close + open，I2S 时钟会一起重配）。
 *
 * 内存约束：
 *   - 解码后的 PCM 必须放内部 RAM（I2S 的 DMA 读不到 PSRAM）；
 *   - 拉流/读文件用的原始数据放 PSRAM；
 *   - 播放任务栈 8KB（解码器在预编译库里工作，不占用户栈）。
 */
#include "music_player.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_settings.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "music_config.h"

static const char *TAG = "music.player";

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t byte_rate;
    uint32_t data_offset;
    uint32_t data_size;
} wav_info_t;

/* 状态用自旋锁保护：播放任务写，界面每 200ms 读一次快照 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static music_status_t s_status = {
    .state = MUSIC_STATE_INIT,
    .source = MUSIC_SOURCE_LOCAL,
    .sample_rate = MUSIC_SAMPLE_RATE,
    .channels = MUSIC_CHANNELS,
    .bits = MUSIC_BITS_PER_SAMPLE,
};

/* 只在播放任务里访问 */
static esp_codec_dev_handle_t s_spk = NULL;
static esp_audio_simple_dec_handle_t s_dec = NULL;
static FILE *s_file = NULL;
static wav_info_t s_wav = { 0 };
static uint32_t s_pos_bytes = 0;
static TaskHandle_t s_task = NULL;
static music_source_t s_source = MUSIC_SOURCE_LOCAL;
static char s_local_track[64];      /* 本地曲目名，投屏结束后要切回来 */

/* 喇叭当前格式：投屏流和本地 WAV 格式不同时要重开 codec */
static uint32_t s_fmt_rate = MUSIC_SAMPLE_RATE;
static uint8_t s_fmt_ch = MUSIC_CHANNELS;
static uint8_t s_fmt_bits = MUSIC_BITS_PER_SAMPLE;

/* 打不开的格式记下来，避免每帧重试 */
static uint32_t s_bad_rate = 0;
static uint8_t s_bad_ch = 0;
static uint8_t s_bad_bits = 0;

/* 解码器输出位深（FLAC 可能是 24 bit，3 字节打包） */
static uint8_t s_pcm_bits = 16;

/* 解码用的两个缓冲，播放任务启动时分配一次 */
static uint8_t *s_in_buf = NULL;
static uint8_t *s_out_buf = NULL;

/* 耗时剖析：每 5 秒打印一次平均耗时，用来区分"网络慢 / 解码慢 / I2S 写阻塞" */
static uint32_t s_prof_read_us = 0;
static uint32_t s_prof_dec_us = 0;
static uint32_t s_prof_write_us = 0;
static uint32_t s_prof_reads = 0;

/*
 * PCM 抖动缓冲：播放任务只负责"拉流 + 解码 + 塞进 fifo"，另一个任务负责
 * 从 fifo 取数据写 I2S。这样网络抖动（实测一次 8 KB read 要 25~60 ms）和解码
 * 耗时不会叠加在 I2S 的 ~33 ms DMA 队列上，DMA 永远不会被饿死。
 * 512 KB 在 48 kHz 立体声 16 bit 下约 2.7 秒余量（PSRAM 够用，代价为零）。
 */
#define MUSIC_PCM_FIFO_BYTES    (512 * 1024)
#define MUSIC_FEED_CHUNK        (4 * 1024)

/*
 * 开流预填充：QQ 音乐切歌/拖动后，CDN 前 10~20 秒吞吐只有 ~180 KB/s（正常 400 KB/s），
 * 边放边攒会连续欠载（实测 underrun 25~32，持续约 20 秒）。新流先静音攒到约 1.7 秒
 * 再出声，把这段最差的网络期用"晚一点开始"换掉。
 */
#define MUSIC_PREFILL_BYTES     (320 * 1024)
#define MUSIC_PREFILL_MS        2500

static StreamBufferHandle_t s_pcm_fifo = NULL;   /* PCM 环形缓冲（PSRAM） */
static SemaphoreHandle_t s_codec_lock = NULL;    /* 喇叭重配格式时与写任务互斥 */
static uint8_t *s_feed_buf = NULL;               /* 写任务取数用的中转块 */
static volatile uint32_t s_feed_fail = 0;
static volatile uint32_t s_feed_underrun = 0;    /* 写任务取不到数据的次数（>0 就是缓冲空了） */
static volatile bool s_prefill = false;          /* true：喂喇叭任务先别出声，等缓冲攒够 */
static volatile uint32_t s_prefill_start = 0;
static uint32_t s_prof_chunks = 0;
static volatile uint32_t s_prof_feed_calls = 0;   /* 写任务实际写了几次 */
static volatile uint32_t s_prof_feed_bytes = 0;   /* 写任务实际写了多少字节 */

/* -------------------------------------------------------------------------- */
/*  投屏命令队列（手机侧 HTTP 任务只投递，播放任务消费）                        */
/* -------------------------------------------------------------------------- */

typedef enum {
    PCMD_SET_URL = 0,
    PCMD_PLAY,
    PCMD_PAUSE,
    PCMD_STOP,
    PCMD_SEEK,
    PCMD_VOLUME,
} player_cmd_t;

typedef struct {
    player_cmd_t cmd;
    uint32_t     value;         /* SEEK 毫秒 / 音量 */
    uint32_t     duration_ms;
    char         url[MUSIC_URL_MAX];
    char         title[64];
    char         artist[64];
} player_msg_t;

static QueueHandle_t s_cmd_q = NULL;

/* -------------------------------------------------------------------------- */
/*  投屏流状态（只在播放任务里改）                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    char     url[MUSIC_URL_MAX];
    uint32_t duration_ms;       /* DIDL 报的时长；0 表示未知 */
    uint32_t total_len;         /* 整首的字节数（含已跳过的部分） */
    uint32_t base_ms;           /* 当前连接对应的时间基准 */
    uint64_t pcm_bytes;         /* 本次连接解码出的 PCM 字节 */
    uint32_t byte_rate;         /* 解码后每秒 PCM 字节 */
    uint16_t fail_chunks;       /* 连续解不出 PCM 的块数，用来识别"格式根本不对" */
    uint8_t  recoveries;        /* 丢帧同步后重开的次数，防止反复重连 */
    bool     opened;
    bool     eos;
    esp_http_client_handle_t http;
    esp_audio_simple_dec_type_t type;
} net_stream_t;

static net_stream_t s_net = { .type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE };

/*
 * 拉流解耦：read 和 decode 各占一个任务，中间隔一个压缩流 ring。
 *
 * 为什么非拆不可：SQ（48 kHz/24 bit FLAC，约 207 KB/s）下一次 8 KB 的 read 实测
 * 20~30 ms、解码 12 ms，串行起来只有 14% 余量，网络偶尔一次 70 ms 尖峰就把 I2S 打空。
 * 拆开后 read 的 20 ms 和解码的 12 ms 重叠，生产速率翻倍。
 *
 * reader 独占 http handle，播放任务只发"打开/关闭"命令 + 从 ring 取数据。
 */
#define MUSIC_RAW_FIFO_BYTES    (128 * 1024)

typedef struct {
    uint8_t  cmd;           /* 0 = open（带 offset），1 = close */
    uint32_t offset;
} net_req_t;

enum {
    NET_ST_IDLE = 0,
    NET_ST_OPENING,
    NET_ST_OPEN,
    NET_ST_FAILED,          /* 打不开（HTTP 错误 / 超时） */
    NET_ST_EOF,             /* 正常读完 */
    NET_ST_LOST,            /* 中途断开 */
};

static StreamBufferHandle_t s_raw_fifo = NULL;
static QueueHandle_t s_net_req_q = NULL;
static volatile int s_net_state = NET_ST_IDLE;
static volatile uint32_t s_net_len_hint = 0;
static volatile int s_net_http_status = 0;
static uint8_t *s_raw_buf = NULL;       /* 播放任务从 raw fifo 取数据用 */
static uint8_t *s_net_in_buf = NULL;    /* 拉流任务专用读缓冲（不能和本地 fread 共用 s_in_buf） */

/* -------------------------------------------------------------------------- */
/*  状态快照                                                                   */
/* -------------------------------------------------------------------------- */

static void status_set_state(music_state_t state)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.state = state;
    taskEXIT_CRITICAL(&s_lock);
}

static void status_set_error(music_state_t state, const char *fmt, ...)
{
    char buf[sizeof(s_status.error)] = { 0 };

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    taskENTER_CRITICAL(&s_lock);
    s_status.state = state;
    memcpy(s_status.error, buf, sizeof(buf));
    taskEXIT_CRITICAL(&s_lock);
}

static void status_set_pcm(uint32_t sample_rate, uint8_t channels, uint8_t bits)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.sample_rate = sample_rate;
    s_status.channels = channels;
    s_status.bits = bits;
    taskEXIT_CRITICAL(&s_lock);
}

static void status_set_position(uint32_t position_ms)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.position_ms = position_ms;
    taskEXIT_CRITICAL(&s_lock);
}

static void status_set_media(const char *track, const char *artist, music_source_t source,
                             uint32_t duration_ms)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.source = source;
    s_status.duration_ms = duration_ms;
    snprintf(s_status.track, sizeof(s_status.track), "%s", track != NULL ? track : "");
    snprintf(s_status.artist, sizeof(s_status.artist), "%s", artist != NULL ? artist : "");
    taskEXIT_CRITICAL(&s_lock);
}

static void status_set_source(music_source_t source)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.source = source;
    taskEXIT_CRITICAL(&s_lock);
}

void music_player_status(music_status_t *out)
{
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    memcpy(out, &s_status, sizeof(*out));
    taskEXIT_CRITICAL(&s_lock);
}

static music_state_t status_get_state(void)
{
    taskENTER_CRITICAL(&s_lock);
    const music_state_t state = s_status.state;
    taskEXIT_CRITICAL(&s_lock);
    return state;
}

static int status_get_volume(void)
{
    taskENTER_CRITICAL(&s_lock);
    const int volume = s_status.volume;
    taskEXIT_CRITICAL(&s_lock);
    return volume;
}

const char *music_player_state_text(music_state_t state)
{
    switch (state) {
    case MUSIC_STATE_PLAYING:
        return "播放中";
    case MUSIC_STATE_PAUSED:
        return "已暂停";
    case MUSIC_STATE_STOPPED:
        return "已结束";
    case MUSIC_STATE_NO_TRACK:
        return "没有曲目";
    case MUSIC_STATE_ERROR:
        return "播放失败";
    case MUSIC_STATE_INIT:
    default:
        return "初始化中";
    }
}

/* -------------------------------------------------------------------------- */
/*  存储                                                                       */
/* -------------------------------------------------------------------------- */

static esp_err_t storage_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MUSIC_STORAGE_BASE,
        .partition_label = MUSIC_STORAGE_LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;      /* INVALID_STATE = 已经被 cjk_font 挂载过 */
    }
    return err;
}

/** @brief 在 storage 根目录里找第一首 music-*.wav（按文件名排序，取最小的那个） */
static bool find_track(char *out, size_t out_len)
{
    DIR *dir = opendir(MUSIC_STORAGE_BASE);
    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir(%s) failed", MUSIC_STORAGE_BASE);
        return false;
    }

    char best[64] = { 0 };
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        const size_t len = strlen(name);
        if (len <= strlen(MUSIC_TRACK_PREFIX) + strlen(MUSIC_TRACK_SUFFIX)) {
            continue;
        }
        if (strncmp(name, MUSIC_TRACK_PREFIX, strlen(MUSIC_TRACK_PREFIX)) != 0) {
            continue;
        }
        if (len < strlen(MUSIC_TRACK_SUFFIX) ||
                strcmp(name + len - strlen(MUSIC_TRACK_SUFFIX), MUSIC_TRACK_SUFFIX) != 0) {
            continue;
        }
        if (best[0] == '\0' || strcmp(name, best) < 0) {
            snprintf(best, sizeof(best), "%s", name);
        }
    }
    closedir(dir);

    if (best[0] == '\0') {
        return false;
    }
    snprintf(out, out_len, "%s", best);
    return true;
}

/* -------------------------------------------------------------------------- */
/*  WAV 头解析（只为了显示时长/格式；解码仍由 simple decoder 完成）             */
/* -------------------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static esp_err_t wav_parse(FILE *fp, wav_info_t *out)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    bool have_fmt = false;
    bool have_data = false;

    for (int i = 0; i < 32 && !(have_fmt && have_data); i++) {
        uint8_t ch[8];
        if (fread(ch, 1, sizeof(ch), fp) != sizeof(ch)) {
            break;
        }
        const uint32_t size = le32(ch + 4);

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16] = { 0 };
            const size_t want = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, want, fp) != want) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (want >= 16) {
                out->channels = le16(fmt + 2);
                out->sample_rate = le32(fmt + 4);
                out->byte_rate = le32(fmt + 8);
                out->bits = le16(fmt + 14);
            }
            if (fseek(fp, (long)size - (long)want, SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
            have_fmt = true;
            continue;
        }

        if (memcmp(ch, "data", 4) == 0) {
            const long off = ftell(fp);
            out->data_offset = off > 0 ? (uint32_t)off : 0;
            out->data_size = size;
            have_data = true;
            continue;   /* 不跳过数据区，交给上层 rewind 后重新读 */
        }

        /* 其它块（LIST/fact/…）按 RIFF 规范跳过，奇数长度补 1 字节 */
        if (fseek(fp, (long)size + (long)(size & 1), SEEK_CUR) != 0) {
            return ESP_FAIL;
        }
    }

    if (!have_fmt || !have_data || out->byte_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 流式 WAV 的 data size 可能是 0xFFFFFFFF，按文件实际长度收口 */
    const long cur = ftell(fp);
    if (fseek(fp, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    const long file_size = ftell(fp);
    if (fseek(fp, cur, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (file_size <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t remain = (uint32_t)file_size > out->data_offset
                            ? (uint32_t)file_size - out->data_offset : 0;
    if (out->data_size == 0 || out->data_size > remain) {
        out->data_size = remain;
    }
    if (out->data_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  音频输出                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief 清空 PCM 缓冲：暂停/拖动/切歌时别把旧数据按新位置播出去 */
static void pcm_fifo_flush(void)
{
    if (s_pcm_fifo == NULL) {
        return;
    }
    if (s_codec_lock != NULL) {
        xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    }
    xStreamBufferReset(s_pcm_fifo);
    if (s_codec_lock != NULL) {
        xSemaphoreGive(s_codec_lock);
    }
}

/** @brief 把解码出的 PCM 交给写任务；false = 缓冲满超时（这块只能丢） */
static bool pcm_fifo_push(const void *data, size_t len)
{
    if (s_pcm_fifo == NULL) {
        return false;
    }
    return xStreamBufferSend(s_pcm_fifo, data, len, pdMS_TO_TICKS(2000)) == len;
}

/**
 * @brief 专门喂 I2S 的任务：从 PCM 缓冲取数据写喇叭。
 *
 * 写喇叭是阻塞的（DMA 满就等），放在独立任务里就不会挡住拉流和解码；
 * 网络抖动、解码耗时只消耗缓冲里的余量，不会把 I2S 的 DMA 队列饿死。
 */
static void audio_feed_task(void *arg)
{
    (void)arg;
    while (true) {
        /* 新流刚开始：先不出声，让缓冲攒到水位（或超时）再放，避开 CDN 预热期 */
        if (s_prefill) {
            const size_t avail = xStreamBufferBytesAvailable(s_pcm_fifo);
            if (avail < MUSIC_PREFILL_BYTES &&
                    esp_log_timestamp() - s_prefill_start < MUSIC_PREFILL_MS) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            s_prefill = false;
            ESP_LOGI(TAG, "prefill %u B in %u ms, start playing",
                     (unsigned)avail,
                     (unsigned)(esp_log_timestamp() - s_prefill_start));
        }

        const size_t got = xStreamBufferReceive(s_pcm_fifo, s_feed_buf,
                                                MUSIC_FEED_CHUNK, pdMS_TO_TICKS(50));
        if (got == 0) {
            /* 播放中取不到数据 = 缓冲被抽干（会听到"咔"），暂停时不计数
               50 ms 粒度：DMA 队列只有 ~33 ms，空这么久一定已经断音了 */
            if (status_get_state() == MUSIC_STATE_PLAYING) {
                s_feed_underrun++;
            }
            continue;
        }
        if (s_codec_lock != NULL) {
            xSemaphoreTake(s_codec_lock, portMAX_DELAY);
        }
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = esp_codec_dev_write(s_spk, s_feed_buf, got);
        s_prof_write_us += (uint32_t)(esp_timer_get_time() - t0);
        s_prof_feed_calls++;
        s_prof_feed_bytes += (uint32_t)got;
        if (s_codec_lock != NULL) {
            xSemaphoreGive(s_codec_lock);
        }
        if (err != ESP_CODEC_DEV_OK) {
            if (s_feed_fail++ < 3) {
                ESP_LOGE(TAG, "speaker write failed: %d", err);
            }
            vTaskDelay(pdMS_TO_TICKS(10));   /* 出错别忙转 */
        }
    }
}

static i2s_std_config_t make_i2s_config(void)
{
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    return cfg;
}

/** @brief 按给定格式打开喇叭（必须先 close，esp_codec_dev_open 对已打开的设备是空操作） */
static esp_err_t audio_open_format(uint32_t sample_rate, uint8_t channels, uint8_t bits)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel = channels,
        .channel_mask = 0,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed (%u Hz / %u ch / %u bit)",
                 (unsigned)sample_rate, (unsigned)channels, (unsigned)bits);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, status_get_volume());
    s_fmt_rate = sample_rate;
    s_fmt_ch = channels;
    s_fmt_bits = bits;
    status_set_pcm(sample_rate, channels, bits);
    ESP_LOGI(TAG, "speaker format: %u Hz / %u ch / %u bit",
             (unsigned)sample_rate, (unsigned)channels, (unsigned)bits);
    return ESP_OK;
}

/** @brief 运行期切换采样率/声道（投屏流常见 44.1 kHz 立体声，本地是 48 kHz 单声道） */
static esp_err_t audio_apply_format(uint32_t sample_rate, uint8_t channels, uint8_t bits)
{
    if (s_spk == NULL || sample_rate == 0 || channels == 0 || bits == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate == s_fmt_rate && channels == s_fmt_ch && bits == s_fmt_bits) {
        return ESP_OK;
    }
    /* 同一种格式已经试过且失败：别每帧都去 close/open（实测会把速度拖到 0.5 倍实时，
       听起来就是连续卡顿） */
    if (sample_rate == s_bad_rate && channels == s_bad_ch && bits == s_bad_bits) {
        return ESP_FAIL;
    }

    const uint32_t old_rate = s_fmt_rate;
    const uint8_t old_ch = s_fmt_ch;
    const uint8_t old_bits = s_fmt_bits;

    /* 切格式期间要独占喇叭：写任务不能还在往旧格式的 I2S 里塞数据，
       缓冲里的旧格式数据也不能留到新格式下去播 */
    if (s_codec_lock != NULL) {
        xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    }
    if (s_pcm_fifo != NULL) {
        xStreamBufferReset(s_pcm_fifo);
    }

    esp_codec_dev_close(s_spk);
    if (audio_open_format(sample_rate, channels, bits) == ESP_OK) {
        s_bad_rate = 0;
        s_bad_ch = 0;
        s_bad_bits = 0;
        if (s_codec_lock != NULL) {
            xSemaphoreGive(s_codec_lock);
        }
        return ESP_OK;
    }
    s_bad_rate = sample_rate;
    s_bad_ch = channels;
    s_bad_bits = bits;

    /* 新格式开不起来就把旧格式恢复，至少别把本地播放也弄哑 */
    esp_codec_dev_close(s_spk);
    audio_open_format(old_rate, old_ch, old_bits);
    if (s_codec_lock != NULL) {
        xSemaphoreGive(s_codec_lock);
    }
    return ESP_FAIL;
}

static esp_err_t audio_open(void)
{
    i2s_std_config_t i2s_cfg = make_i2s_config();
    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL) {
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_FAIL;
    }

    /* 音量跟随 selector 设置页（同一个 NVS，各 app 共享），先写进状态再开设备 */
    const int volume = app_settings_get_speaker_volume();
    taskENTER_CRITICAL(&s_lock);
    s_status.volume = volume;
    taskEXIT_CRITICAL(&s_lock);

    if (audio_open_format(MUSIC_SAMPLE_RATE, MUSIC_CHANNELS, MUSIC_BITS_PER_SAMPLE) != ESP_OK) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "speaker ready: %d Hz / %d ch / %d bit, volume %d%%",
             MUSIC_SAMPLE_RATE, MUSIC_CHANNELS, MUSIC_BITS_PER_SAMPLE, volume);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  解码器                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 打开（或重开）解码器。
 *
 * 为什么换曲/回跳要关掉再开，而不是 esp_audio_simple_dec_reset()：
 * 实测 WAV 容器解析器被 reset 后不会再重新解析 RIFF/WAVE 头，喂进去的数据
 * 全被当成"还在找帧"吃掉（日志里表现为 pos 一直是 0、peak 0、文件瞬间读完、
 * 然后疯狂 track end/loop）。关掉重开等价于第一次播放，稳定且只在换曲时发生。
 */
static esp_err_t decoder_open(esp_audio_simple_dec_type_t type)
{
    if (s_dec != NULL) {
        esp_audio_simple_dec_close(s_dec);
        s_dec = NULL;
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    const esp_audio_err_t err = esp_audio_simple_dec_open(&cfg, &s_dec);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed (%s): %d", esp_audio_simple_dec_get_name(type), err);
        s_dec = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/** @brief 回到文件头 + 重开解码器（循环播放 / 出错重试都走这里） */
static bool track_rewind(void)
{
    if (s_file == NULL) {
        return false;
    }
    if (fseek(s_file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek(0) failed");
        return false;
    }
    /* 解码器要先看到 RIFF/WAVE 头，所以文件头和解码器状态必须一起重置 */
    if (decoder_open(ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) != ESP_OK) {
        return false;
    }
    /* 从投屏切回本地时喇叭可能还停在 44.1 kHz 立体声 */
    audio_apply_format(s_wav.sample_rate, (uint8_t)s_wav.channels, (uint8_t)s_wav.bits);
    s_pcm_bits = 16;        /* 本地只放 16 bit WAV，别把上一路投屏的 24 bit 记着 */
    s_pos_bytes = 0;
    status_set_position(0);
    return true;
}

/* -------------------------------------------------------------------------- */
/*  投屏流                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief 用前几个字节猜容器/编码（QPlay 推的是裸 MP3 或 FLAC，没有可靠的文件名） */
static esp_audio_simple_dec_type_t sniff_type(const uint8_t *p, size_t len)
{
    if (len >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WAVE", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (len >= 4 && memcmp(p, "fLaC", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (len >= 4 && memcmp(p, "OggS", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    }
    if (len >= 3 && memcmp(p, "ID3", 3) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    /* MP3 帧同步：11 个 1 + 版本/层信息 */
    if (len >= 2 && p[0] == 0xFF && (p[1] & 0xE0) == 0xE0) {
        if ((p[1] & 0x06) == 0x00) {        /* layer bits = 0 → AAC(ADTS) */
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        }
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;  /* 兜底：QPlay 绝大多数是 MP3 */
}

/** @brief 请求拉流任务关掉连接（保留 url/时长，便于暂停后按位置重开） */
static void net_close_http(void)
{
    s_net.opened = false;
    if (s_net_req_q != NULL) {
        const net_req_t req = { .cmd = 1, .offset = 0 };
        xQueueSend(s_net_req_q, &req, 0);
    }
}

static void net_clear(void)
{
    net_close_http();
    s_net.url[0] = '\0';
    s_net.duration_ms = 0;
    s_net.total_len = 0;
    s_net.base_ms = 0;
    s_net.pcm_bytes = 0;
    s_net.byte_rate = 0;
    s_net.eos = false;
    s_net.fail_chunks = 0;
    s_net.recoveries = 0;
    s_net.type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    s_pcm_bits = 16;        /* 新流默认 16 bit，解出第一块 PCM 后由 apply_dec_format 修正 */
}

/* -------------------------------------------------------------------------- */
/*  拉流任务（唯一的 http 读方）                                                */
/* -------------------------------------------------------------------------- */

/** @brief 按指定的字节偏移打开 url；成功返回 true */
static bool reader_open(uint32_t offset)
{
    esp_http_client_config_t cfg = {
        .url = s_net.url,
        .timeout_ms = MUSIC_NET_TIMEOUT_MS,
        .buffer_size = 2048,      /* 内部 RAM 要和 FLAC 解码器、屏幕 DMA 共用，省一点 */
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
    };
    s_net.http = esp_http_client_init(&cfg);
    if (s_net.http == NULL) {
        ESP_LOGE(TAG, "http client init failed");
        s_net_state = NET_ST_FAILED;
        return false;
    }
    if (offset > 0) {
        char range[32];
        snprintf(range, sizeof(range), "bytes=%u-", (unsigned)offset);
        esp_http_client_set_header(s_net.http, "Range", range);
    }
    esp_http_client_set_header(s_net.http, "User-Agent", "ESP32-DLNA/1.0");

    if (esp_http_client_open(s_net.http, 0) != ESP_OK) {
        ESP_LOGE(TAG, "http open failed");
        goto fail;
    }

    const int64_t clen = esp_http_client_fetch_headers(s_net.http);
    const int status = esp_http_client_get_status_code(s_net.http);
    s_net_http_status = status;
    if (status != 200 && status != 206) {
        ESP_LOGE(TAG, "http status %d", status);
        goto fail;
    }
    s_net_len_hint = (clen > 0)
                     ? ((status == 206) ? offset + (uint32_t)clen : (uint32_t)clen)
                     : 0;
    s_net_state = NET_ST_OPEN;
    return true;

fail:
    if (s_net.http != NULL) {
        esp_http_client_close(s_net.http);
        esp_http_client_cleanup(s_net.http);
        s_net.http = NULL;
    }
    s_net_state = NET_ST_FAILED;
    return false;
}

/** @brief 关掉 http 连接（不动 s_net_state，EOF/LOST 标记要留给播放任务看） */
static void reader_close(void)
{
    if (s_net.http != NULL) {
        esp_http_client_close(s_net.http);
        esp_http_client_cleanup(s_net.http);
        s_net.http = NULL;
    }
}

/** @brief 处理一条来自播放任务的请求（打开/关闭） */
static void reader_handle_req(const net_req_t *req)
{
    reader_close();
    if (req->cmd == 0) {
        xStreamBufferReset(s_raw_fifo);
        reader_open(req->offset);
    } else if (s_net_state != NET_ST_OPENING) {
        /* 播放侧正在等 OPEN 时别把状态改成 IDLE，否则它会以为打开失败 */
        s_net_state = NET_ST_IDLE;
    }
}

/**
 * @brief 拉流任务：把压缩数据读进 s_raw_fifo。
 *
 * 播放任务只从 ring 取数据解码，于是 read 和解码真正并行。
 *
 * 注意：命令队列必须**非阻塞**轮询。这里曾经用 50 ms 超时，结果把拉流节奏
 * 硬限制在 20 次/秒 ≈ 160 KB/s，SQ（212 KB/s）直接掉到 0.77 倍速、缓冲长期空。
 */
static void net_reader_task(void *arg)
{
    (void)arg;

    while (true) {
        net_req_t req;

        /* 有连接时零等待：先看一眼命令，紧接着就去读网络，不能空等 */
        if (xQueueReceive(s_net_req_q, &req, 0) == pdTRUE) {
            reader_handle_req(&req);
            continue;
        }

        if (s_net_state != NET_ST_OPEN || s_net.http == NULL) {
            /* 空闲：阻塞等命令，别忙转 */
            if (xQueueReceive(s_net_req_q, &req, pdMS_TO_TICKS(50)) == pdTRUE) {
                reader_handle_req(&req);
            }
            continue;
        }

        /* ring 里先腾出地方，读出来的数据必须有处可放 */
        const size_t space = xStreamBufferSpacesAvailable(s_raw_fifo);
        if (space < 2048) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const size_t want = (space < MUSIC_NET_CHUNK) ? space : MUSIC_NET_CHUNK;

        const int64_t t_read = esp_timer_get_time();
        const int got = esp_http_client_read(s_net.http, (char *)s_net_in_buf, (int)want);
        s_prof_read_us += (uint32_t)(esp_timer_get_time() - t_read);
        s_prof_reads++;

        if (got > 0) {
            xStreamBufferSend(s_raw_fifo, s_net_in_buf, (size_t)got, pdMS_TO_TICKS(200));
            continue;
        }

        /* 读回 0/负数：正常读完还是被对端关掉，要分开记 */
        s_net_state = (got == 0 && esp_http_client_is_complete_data_received(s_net.http))
                      ? NET_ST_EOF : NET_ST_LOST;
        reader_close();
    }
}

/** @brief 解码一块数据并写喇叭；返回解码出的 PCM 字节数 */
static uint32_t play_chunk(const uint8_t *in, size_t in_len);

/** @brief 解码信息变化时同步喇叭格式（投屏流的采样率/声道在第一次解出 PCM 后才知道） */
static void apply_dec_format(void)
{
    if (s_dec == NULL) {
        return;
    }
    esp_audio_simple_dec_info_t info = { 0 };
    const esp_audio_err_t ierr = esp_audio_simple_dec_get_info(s_dec, &info);
    if (ierr != ESP_AUDIO_ERR_OK) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "decoder info unavailable: %d (输出格式沿用上一路)", ierr);
        }
        return;
    }
    if (info.sample_rate == 0 || info.channel == 0 || info.bits_per_sample == 0) {
        return;
    }
    /* 解码器可能吐 24 bit（FLAC）：喇叭统一按 16 bit 开，写之前自己转，
       免得 I2S 槽宽和打包方式对不上听成噪声 */
    const uint8_t out_bits = (info.bits_per_sample > 16) ? 16 : (uint8_t)info.bits_per_sample;
    if (audio_apply_format(info.sample_rate, info.channel, out_bits) == ESP_OK) {
        s_pcm_bits = (uint8_t)info.bits_per_sample;
        s_net.byte_rate = info.sample_rate * info.channel * (out_bits / 8);
        if (s_pcm_bits != out_bits) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                ESP_LOGI(TAG, "decoder outputs %u-bit PCM, convert to %u-bit for speaker",
                         (unsigned)s_pcm_bits, (unsigned)out_bits);
            }
        }
    }
}

/** @brief 投屏流的位置 = 本次连接起点 + 已解码 PCM 时长 */
static void net_update_position(void)
{
    if (s_net.byte_rate == 0) {
        return;
    }
    const uint32_t played_ms = (uint32_t)(s_net.pcm_bytes * 1000 / s_net.byte_rate);
    uint32_t pos = s_net.base_ms + played_ms;
    if (s_net.duration_ms > 0 && pos > s_net.duration_ms) {
        pos = s_net.duration_ms;
    }
    status_set_position(pos);
}

static bool net_open(uint32_t start_ms)
{
    /* 这里不能先发 CLOSE：那样 reader 处理 CLOSE 时会把状态置回 IDLE，
       而本函数随后又置 OPENING 并等它变成 OPEN，两条命令交错就永远等不到
       （表现是投屏直接报"拉流失败"）。reader 收到 OPEN 会自己先关旧连接。 */
    s_net.opened = false;
    if (s_raw_fifo != NULL) {
        xStreamBufferReset(s_raw_fifo);
    }

    if (s_net.url[0] == '\0' || s_raw_fifo == NULL || s_raw_buf == NULL || s_net_req_q == NULL) {
        return false;
    }
    if (strncmp(s_net.url, "http://", 7) != 0) {
        status_set_error(MUSIC_STATE_ERROR, "只支持 http 投屏地址");
        return false;
    }

    /* 拖动进度：按"时长的百分比"估字节偏移，CDN 支持 Range（实测 206） */
    uint32_t offset = 0;
    if (start_ms > 0 && (s_net.duration_ms == 0 || s_net.total_len == 0)) {
        /* 还没有 length/时长信息（第一次开流就要求跳转）时没法算偏移，老实从头放 */
        ESP_LOGW(TAG, "no length info yet, cannot seek to %u ms, start from 0",
                 (unsigned)start_ms);
        start_ms = 0;
    }
    if (start_ms > 0) {
        offset = (uint32_t)((uint64_t)start_ms * s_net.total_len / s_net.duration_ms);
        if (s_net.type == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) {
            /* WAV 必须从 RIFF 头开始喂给解析器，按字节跳进去只能从头再来 */
            ESP_LOGW(TAG, "wav cannot seek mid-stream, restart from 0");
            offset = 0;
            start_ms = 0;
        }
    }

    /* 交给拉流任务连；等它把状态从 OPENING 改掉（连不上最多等 10 秒） */
    s_net_state = NET_ST_OPENING;
    const net_req_t req = { .cmd = 0, .offset = offset };
    if (xQueueSend(s_net_req_q, &req, 0) != pdTRUE) {
        ESP_LOGE(TAG, "net request queue full");
        s_net_state = NET_ST_FAILED;
        return false;
    }
    const uint32_t t_wait = esp_log_timestamp();
    while (s_net_state == NET_ST_OPENING) {
        if (esp_log_timestamp() - t_wait > MUSIC_NET_OPEN_TIMEOUT_MS) {
            ESP_LOGE(TAG, "http open timeout");
            s_net_state = NET_ST_FAILED;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_net_state != NET_ST_OPEN) {
        ESP_LOGE(TAG, "stream open failed (state %d)", s_net_state);
        status_set_error(MUSIC_STATE_ERROR, "拉流失败");
        return false;
    }

    if (s_net_len_hint > 0) {
        s_net.total_len = s_net_len_hint;
    }
    if (s_net_http_status == 200 && offset > 0) {
        /* 服务器忽略了 Range：只能从头播，把位置基准也改回 0，别让手机上显示错位 */
        ESP_LOGW(TAG, "server ignored Range, restart from 0");
        start_ms = 0;
    }

    /* 从 ring 取第一块判容器：TCP 首包可能被切成几段，凑够 16 字节再猜 */
    size_t got = 0;
    const int64_t t_sniff = esp_log_timestamp();
    while (got < 16) {
        const size_t n = xStreamBufferReceive(s_raw_fifo, s_raw_buf + got,
                                              MUSIC_NET_CHUNK - got, pdMS_TO_TICKS(100));
        if (n > 0) {
            got += n;
            continue;
        }
        if (s_net_state != NET_ST_OPEN) {
            break;
        }
        if (esp_log_timestamp() - t_sniff > 3000) {
            break;
        }
    }
    if (got == 0) {
        ESP_LOGE(TAG, "no data from stream");
        status_set_error(MUSIC_STATE_ERROR, "拉流失败");
        net_close_http();
        return false;
    }

    /* 已经是中段数据时不能重新嗅探：PCM/帧头里没有 magic，会被误判成别的编码 */
    const esp_audio_simple_dec_type_t type =
        (offset > 0 && s_net.type != ESP_AUDIO_SIMPLE_DEC_TYPE_NONE)
        ? s_net.type
        : sniff_type(s_raw_buf, got);
    if (decoder_open(type) != ESP_OK) {
        status_set_error(MUSIC_STATE_ERROR, "解码器不支持该格式");
        net_close_http();
        return false;
    }
    s_net.type = type;

    s_net.opened = true;
    s_net.eos = false;
    s_net.pcm_bytes = 0;
    s_net.base_ms = start_ms;
    s_net.byte_rate = 0;
    s_net.fail_chunks = 0;
    status_set_position(start_ms);

    ESP_LOGI(TAG, "stream open: %s, offset %u B, total %u B, %u ms",
             esp_audio_simple_dec_get_name(type), (unsigned)offset,
             (unsigned)s_net.total_len, (unsigned)s_net.duration_ms);

    /* 新流（含切歌/拖动/断线重连）：先攒缓冲再出声 */
    s_prefill_start = esp_log_timestamp();
    s_prefill = (s_pcm_fifo != NULL);

    const uint32_t decoded = play_chunk(s_raw_buf, got);
    s_net.pcm_bytes += decoded;
    net_update_position();
    /* 首块就能看出格式判断对不对：没有 PCM 也没有码率，说明解码器根本没识别出帧 */
    ESP_LOGI(TAG, "first chunk: %u B -> %u B PCM, %u B/s",
             (unsigned)got, (unsigned)decoded, (unsigned)s_net.byte_rate);
    return true;
}

/**
 * @brief 连接断了（暂停太久 / CDN 主动关闭）时按当前位置重开。
 *
 * QQ 音乐的 vkey 链接有时效，暂停很久再继续会拿到 0 长度读；这种情况重开一次
 * 通常能接着放，直接判"播完"会让手机进度条瞬间跳到结尾。
 */
static bool net_reopen_current(void)
{
    const uint32_t resume_ms = s_net.base_ms +
                               (s_net.byte_rate ? (uint32_t)(s_net.pcm_bytes * 1000 / s_net.byte_rate) : 0);
    s_net.base_ms = resume_ms;
    s_net.pcm_bytes = 0;
    ESP_LOGW(TAG, "connection lost, reopen at %u ms", (unsigned)resume_ms);
    return net_open(resume_ms);   /* 不用先 CLOSE，net_open 里发 OPEN 即可 */
}

/** @brief 拉一块、解一块；返回 false 表示流结束或出错 */
static bool net_step(void)
{
    if (!s_net.opened) {
        return net_open(s_net.base_ms);
    }

    /* 只从 ring 取数据解码；读网络是 net_reader_task 的事 */
    const size_t got = xStreamBufferReceive(s_raw_fifo, s_raw_buf, MUSIC_NET_CHUNK,
                                            pdMS_TO_TICKS(100));
    if (got == 0) {
        /* ring 空了：看拉流任务那边是断线了还是正常结束 */
        if (s_net_state == NET_ST_LOST) {
            return net_reopen_current();
        }
        if (s_net_state == NET_ST_EOF || s_net_state == NET_ST_FAILED) {
            /* 整首一个 PCM 字节都没解出来：是格式认错了，别当成"正常播完" */
            if (s_net.pcm_bytes == 0) {
                ESP_LOGE(TAG, "stream ended without any PCM (%s)",
                         esp_audio_simple_dec_get_name(s_net.type));
                status_set_error(MUSIC_STATE_ERROR, "投屏音源解不出来");
            }
            return false;
        }
        return true;    /* 还在读，等下一轮 */
    }

    const uint32_t decoded = play_chunk(s_raw_buf, got);
    if (decoded == 0) {
        /* 中段丢一两个块正常（拖动后要等帧同步），连续解不出来就是出问题了 */
        if (++s_net.fail_chunks >= 8) {
            s_net.fail_chunks = 0;
            if (s_net.byte_rate > 0 && s_net.recoveries < 3) {
                /* 曾经放出过声音：多半是解析器丢帧同步（实测拼接素材会这样），
                   按当前进度重新 Range 拉一段，通常能接上 */
                s_net.recoveries++;
                ESP_LOGW(TAG, "decoder lost sync, resync #%u", (unsigned)s_net.recoveries);
                return net_reopen_current();
            }
            ESP_LOGE(TAG, "no PCM, give up (%u B/s, %u resync)",
                     (unsigned)s_net.byte_rate, (unsigned)s_net.recoveries);
            status_set_error(MUSIC_STATE_ERROR, "投屏音源解不出来");
            net_close_http();
            return false;
        }
        return true;
    }
    s_net.fail_chunks = 0;
    s_net.pcm_bytes += decoded;
    net_update_position();
    return true;
}

static void net_seek(uint32_t position_ms)
{
    if (s_net.duration_ms > 0 && position_ms > s_net.duration_ms) {
        position_ms = s_net.duration_ms;
    }
    s_net.base_ms = position_ms;
    s_net.pcm_bytes = 0;
    s_net.eos = false;
    net_close_http();       /* 下一步按新偏移重新 Range */
    status_set_position(position_ms);
    ESP_LOGI(TAG, "stream seek -> %u ms", (unsigned)position_ms);
}

/* -------------------------------------------------------------------------- */
/*  播放任务                                                                   */
/* -------------------------------------------------------------------------- */

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "[heap] %-6s internal=%u dma=%u dma_max=%u psram=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/** @brief 24 bit（3 字节小端打包）原地转 16 bit：丢掉最低字节，返回转换后字节数 */
static size_t pcm24_to_s16(uint8_t *buf, size_t len)
{
    const size_t samples = len / 3;
    for (size_t i = 0; i < samples; i++) {
        const uint8_t *s = buf + i * 3;
        buf[i * 2] = s[1];
        buf[i * 2 + 1] = s[2];
    }
    return samples * 2;
}

/**
 * @brief 解码并播放一个数据块。
 *
 * @return 本次解码出的 PCM 字节数（解码失败返回 0，调用方按"坏块跳过"处理）
 */
static uint32_t play_chunk(const uint8_t *in, size_t in_len)
{
    if (s_dec == NULL || s_spk == NULL || s_out_buf == NULL) {
        return 0;
    }

    esp_audio_simple_dec_raw_t raw = {
        .buffer = (uint8_t *)in,
        .len = (uint32_t)in_len,
        .eos = false,
    };
    uint32_t total = 0;
    int16_t peak = 0;

    while (raw.len > 0) {
        esp_audio_simple_dec_out_t frame = {
            .buffer = s_out_buf,
            .len = MUSIC_OUT_CHUNK,
        };

        const int64_t t_dec = esp_timer_get_time();
        const esp_audio_err_t err = esp_audio_simple_dec_process(s_dec, &raw, &frame);
        s_prof_dec_us += (uint32_t)(esp_timer_get_time() - t_dec);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "output buffer too small (need %u)", (unsigned)frame.needed_size);
            break;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            /* 拖动后会有几块找不到帧同步，别把日志刷爆 */
            static uint32_t fail_cnt = 0;
            if (fail_cnt < 3 || (fail_cnt & 0x3F) == 0) {
                ESP_LOGW(TAG, "decode failed: %d (n=%u)", err, (unsigned)fail_cnt);
            }
            fail_cnt++;
            break;
        }

        if (frame.decoded_size > 0) {
            /* 投屏流的格式第一次解出 PCM 才知道：跟着改喇叭 */
            if (s_source == MUSIC_SOURCE_DLNA) {
                apply_dec_format();
            }

            /* 24 bit 数据喇叭按 16 bit 收：原地收窄（输出比输入短，原地安全） */
            size_t play_len = frame.decoded_size;
            if (s_pcm_bits > 16) {
                play_len = pcm24_to_s16((uint8_t *)frame.buffer, play_len);
            }

            if (play_len > 0) {
                if (s_pcm_fifo != NULL) {
                    /* 交给写任务：缓冲满时这里会阻塞（正好形成背压），只有超时才丢块 */
                    if (!pcm_fifo_push(frame.buffer, play_len)) {
                        static uint32_t drop_cnt = 0;
                        if ((drop_cnt++ & 0x1F) == 0) {
                            ESP_LOGW(TAG, "pcm fifo full, dropped %u B (n=%u)",
                                     (unsigned)play_len, (unsigned)drop_cnt);
                        }
                        break;
                    }
                    s_prof_chunks++;
                } else {
                    const int64_t t_wr = esp_timer_get_time();
                    const esp_err_t werr = esp_codec_dev_write(s_spk, frame.buffer, play_len);
                    s_prof_write_us += (uint32_t)(esp_timer_get_time() - t_wr);
                    s_prof_chunks++;
                    if (werr != ESP_CODEC_DEV_OK) {
                        ESP_LOGE(TAG, "speaker write failed");
                        break;
                    }
                }

                /* 峰值诊断：静音 ~0，正常音乐 3000~20000；32768 说明缓冲/增益有问题 */
                const int16_t *pcm = (const int16_t *)frame.buffer;
                const size_t samples = play_len / sizeof(int16_t);
                for (size_t i = 0; i < samples; i++) {
                    int v = pcm[i];
                    if (v < 0) {
                        v = -v;
                    }
                    if (v > peak) {
                        peak = (int16_t)v;
                    }
                }
                total += play_len;
            }
        }

        if (raw.consumed == 0 && frame.decoded_size == 0) {
            break;      /* 解析器要更多数据，回去再读一块 */
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }

    static uint32_t last_log = 0;
    static int16_t peak_hold = 0;
    if (peak > peak_hold) {
        peak_hold = peak;
    }
    const uint32_t now = esp_log_timestamp();
    if (now - last_log >= 5000) {
        music_status_t st;
        music_player_status(&st);
        /* 一次读/一块解码/一次写喇叭各要多久：超过"这块音频本身的时长"就会卡 */
        const uint32_t rd_ms = s_prof_reads ? s_prof_read_us / s_prof_reads / 1000 : 0;
        const uint32_t dc_ms = s_prof_chunks ? s_prof_dec_us / s_prof_chunks / 1000 : 0;
        const uint32_t wr_ms = s_prof_chunks ? s_prof_write_us / s_prof_chunks / 1000 : 0;
        /* 两个 ring 的水位：pcm 空 = 供不上（网络/解码），raw 满 = 播放侧消费慢 */
        const unsigned pcm_kb = s_pcm_fifo
                                ? (unsigned)(xStreamBufferBytesAvailable(s_pcm_fifo) / 1024) : 0;
        const unsigned raw_kb = s_raw_fifo
                                ? (unsigned)(xStreamBufferBytesAvailable(s_raw_fifo) / 1024) : 0;
        ESP_LOGI(TAG, "%s pos %u/%u ms, peak %d, int=%u dma=%u, fmt %u/%u/%u pcm%u, "
                 "read %u dec %u write %u ms (%u rd/%u wr), underrun %u, "
                 "pcm %u/%u KB raw %u/%u KB feed %u KB/%u",
                 st.source == MUSIC_SOURCE_DLNA ? "dlna" : "local",
                 (unsigned)st.position_ms, (unsigned)st.duration_ms, peak_hold,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)s_fmt_rate, (unsigned)s_fmt_ch, (unsigned)s_fmt_bits,
                 (unsigned)s_pcm_bits, (unsigned)rd_ms, (unsigned)dc_ms, (unsigned)wr_ms,
                 (unsigned)s_prof_reads, (unsigned)s_prof_chunks,
                 (unsigned)s_feed_underrun,
                 pcm_kb, (unsigned)(MUSIC_PCM_FIFO_BYTES / 1024),
                 raw_kb, (unsigned)(MUSIC_RAW_FIFO_BYTES / 1024),
                 (unsigned)(s_prof_feed_bytes / 1024), (unsigned)s_prof_feed_calls);
        s_prof_read_us = 0;
        s_prof_dec_us = 0;
        s_prof_write_us = 0;
        s_prof_reads = 0;
        s_prof_chunks = 0;
        s_feed_underrun = 0;
        s_prof_feed_calls = 0;
        s_prof_feed_bytes = 0;
        last_log = now;
        peak_hold = 0;
    }
    return total;
}

/** @brief 处理一条来自界面/手机的命令（都在播放任务上下文里执行） */
static void handle_msg(const player_msg_t *msg)
{
    switch (msg->cmd) {
    case PCMD_SET_URL:
        net_clear();
        pcm_fifo_flush();
        snprintf(s_net.url, sizeof(s_net.url), "%s", msg->url);
        s_net.duration_ms = msg->duration_ms;
        s_source = MUSIC_SOURCE_DLNA;
        s_pos_bytes = 0;
        status_set_media(msg->title, msg->artist, MUSIC_SOURCE_DLNA, msg->duration_ms);
        status_set_position(0);
        status_set_state(MUSIC_STATE_PAUSED);   /* 手机紧接着会发 Play */
        ESP_LOGI(TAG, "dlna uri set: %s - %s (%u ms)",
                 msg->artist, msg->title, (unsigned)msg->duration_ms);
        break;

    case PCMD_PLAY:
        if (s_source == MUSIC_SOURCE_DLNA) {
            if (s_net.url[0] == '\0') {
                ESP_LOGW(TAG, "play ignored: no dlna uri");
                break;
            }
            if (s_net.eos) {            /* 播完再点播放：从头再来 */
                s_net.eos = false;
                s_net.base_ms = 0;
                s_net.pcm_bytes = 0;
                status_set_position(0);
            }
        } else if (s_file == NULL) {
            ESP_LOGW(TAG, "play ignored: no local track");
            break;
        }
        status_set_state(MUSIC_STATE_PLAYING);
        ESP_LOGI(TAG, "play (%s)", s_source == MUSIC_SOURCE_DLNA ? "dlna" : "local");
        break;

    case PCMD_PAUSE:
        if (status_get_state() == MUSIC_STATE_PLAYING) {
            status_set_state(MUSIC_STATE_PAUSED);
            pcm_fifo_flush();       /* 缓冲里剩的几百毫秒也丢掉，暂停要立刻静音 */
            ESP_LOGI(TAG, "pause");
        }
        break;

    case PCMD_STOP:
        net_clear();
        pcm_fifo_flush();
        s_source = MUSIC_SOURCE_LOCAL;
        status_set_state(MUSIC_STATE_PAUSED);
        if (s_file != NULL && track_rewind()) {
            const uint32_t duration_ms = (uint32_t)((uint64_t)s_wav.data_size * 1000 / s_wav.byte_rate);
            status_set_media(s_local_track, NULL, MUSIC_SOURCE_LOCAL, duration_ms);
        } else {
            status_set_media(s_local_track, NULL, MUSIC_SOURCE_LOCAL, 0);
            status_set_source(MUSIC_SOURCE_LOCAL);
        }
        ESP_LOGI(TAG, "stop, back to local track");
        break;

    case PCMD_SEEK:
        if (s_source == MUSIC_SOURCE_DLNA) {
            net_seek(msg->value);
            pcm_fifo_flush();
        }
        break;

    case PCMD_VOLUME: {
        int volume = (int)msg->value;
        if (volume < 0) {
            volume = 0;
        }
        if (volume > 100) {
            volume = 100;
        }
        taskENTER_CRITICAL(&s_lock);
        s_status.volume = volume;
        taskEXIT_CRITICAL(&s_lock);
        if (s_spk != NULL) {
            esp_codec_dev_set_out_vol(s_spk, volume);
        }
        ESP_LOGI(TAG, "volume %d", volume);
        break;
    }

    default:
        break;
    }
}

static void play_task(void *arg)
{
    (void)arg;

    s_in_buf = heap_caps_malloc(MUSIC_NET_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_in_buf == NULL) {
        s_in_buf = heap_caps_malloc(MUSIC_NET_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    /* 32 KB 内部 RAM 在这个工程里可能没有连续块（Wi-Fi 起来后最大块只剩 ~31 KB），
       优先放 PSRAM；I2S 写入时会拷进内部 DMA 描述符，源缓冲在哪都行 */
    s_out_buf = heap_caps_malloc(MUSIC_OUT_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_out_buf == NULL) {
        s_out_buf = heap_caps_malloc(MUSIC_OUT_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "out buffer fell back to internal RAM");
    }
    if (s_in_buf == NULL || s_out_buf == NULL) {
        ESP_LOGE(TAG, "decode buffers alloc failed (in=%p out=%p)", s_in_buf, s_out_buf);
        status_set_error(MUSIC_STATE_ERROR, "内存不足");
        goto exit;
    }

    /* 解码与 I2S 写入解耦：播放任务只管拉流+解码，写喇叭交给独立任务 */
    s_codec_lock = xSemaphoreCreateMutex();
    s_feed_buf = heap_caps_malloc(MUSIC_FEED_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_feed_buf == NULL) {
        s_feed_buf = heap_caps_malloc(MUSIC_FEED_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_codec_lock != NULL && s_feed_buf != NULL) {
        s_pcm_fifo = xStreamBufferCreateWithCaps(MUSIC_PCM_FIFO_BYTES, 1,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_pcm_fifo != NULL) {
        if (xTaskCreate(audio_feed_task, "music.i2s", 4096, NULL, 7, NULL) != pdPASS) {
            ESP_LOGE(TAG, "feed task create failed, fall back to direct write");
            vStreamBufferDeleteWithCaps(s_pcm_fifo);
            s_pcm_fifo = NULL;
        }
    } else {
        ESP_LOGW(TAG, "pcm fifo not available (%p), fall back to direct write", s_feed_buf);
    }

    /* 拉流任务：压缩流 ring + 命令队列，一个专用任务负责所有 http 读 */
    s_raw_buf = heap_caps_malloc(MUSIC_NET_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_net_in_buf = heap_caps_malloc(MUSIC_NET_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_net_req_q = xQueueCreate(4, sizeof(net_req_t));
    s_raw_fifo = xStreamBufferCreateWithCaps(MUSIC_RAW_FIFO_BYTES, 1,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_raw_buf != NULL && s_net_in_buf != NULL && s_net_req_q != NULL && s_raw_fifo != NULL) {
        /* http client 的 open/解析要吃栈，别用 4 KB */
        if (xTaskCreate(net_reader_task, "music.net", 8192, NULL, 6, NULL) != pdPASS) {
            ESP_LOGE(TAG, "reader task create failed");
        }
    } else {
        ESP_LOGE(TAG, "raw fifo alloc failed (buf=%p in=%p q=%p fifo=%p)",
                 s_raw_buf, s_net_in_buf, s_net_req_q, s_raw_fifo);
    }

    ESP_LOGI(TAG, "play task started (read %d B, out %d B, pcm fifo %d B, raw fifo %d B)",
             MUSIC_NET_CHUNK, MUSIC_OUT_CHUNK, s_pcm_fifo ? MUSIC_PCM_FIFO_BYTES : 0,
             s_raw_fifo ? MUSIC_RAW_FIFO_BYTES : 0);

    uint32_t last_heap_log = 0;
    while (true) {
        player_msg_t msg;
        while (s_cmd_q != NULL && xQueueReceive(s_cmd_q, &msg, 0) == pdTRUE) {
            handle_msg(&msg);
        }

        if (status_get_state() != MUSIC_STATE_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(20));
            const uint32_t now = esp_log_timestamp();
            if (now - last_heap_log >= 5000) {
                log_heap("run");
                last_heap_log = now;
            }
            continue;
        }

        if (s_source == MUSIC_SOURCE_DLNA) {
            if (!net_step() && !s_net.eos) {
                s_net.eos = true;
                /* 解码/拉流已经置过 ERROR 就不要覆盖成"正常结束" */
                if (status_get_state() == MUSIC_STATE_PLAYING) {
                    ESP_LOGI(TAG, "stream finished");
                    net_close_http();   /* 释放连接；手机再点播放时重新开流 */
                    status_set_state(MUSIC_STATE_STOPPED);
                    if (s_net.duration_ms > 0) {
                        status_set_position(s_net.duration_ms);
                    }
                }
            }
            continue;
        }

        /* ---- 本地 WAV：原样保留循环播放 ---- */
        if (s_file == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const size_t got = fread(s_in_buf, 1, MUSIC_NET_CHUNK, s_file);
        if (got == 0) {
            if (feof(s_file)) {
                ESP_LOGI(TAG, "track end, loop");
                if (!track_rewind()) {
                    status_set_error(MUSIC_STATE_ERROR, "回到文件头失败");
                    continue;
                }
                continue;
            }
            ESP_LOGE(TAG, "read failed");
            status_set_error(MUSIC_STATE_ERROR, "读取文件失败");
            continue;
        }

        const uint32_t decoded = play_chunk(s_in_buf, got);
        if (decoded > 0) {
            s_pos_bytes += decoded;
            status_set_position((uint32_t)((uint64_t)s_pos_bytes * 1000 / s_wav.byte_rate));
        } else {
            status_set_error(MUSIC_STATE_ERROR, "解码失败，长按 BOOT 回选择器");
        }

        const uint32_t now = esp_log_timestamp();
        if (now - last_heap_log >= 5000) {
            log_heap("run");
            last_heap_log = now;
        }
    }

exit:
    if (s_in_buf != NULL) {
        heap_caps_free(s_in_buf);
        s_in_buf = NULL;
    }
    if (s_out_buf != NULL) {
        heap_caps_free(s_out_buf);
        s_out_buf = NULL;
    }
    if (s_net_in_buf != NULL) {
        heap_caps_free(s_net_in_buf);
        s_net_in_buf = NULL;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */

static esp_err_t post_msg(const player_msg_t *msg)
{
    if (s_cmd_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_cmd_q, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, drop %d", (int)msg->cmd);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t music_player_init(void)
{
    esp_err_t err = storage_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed: %s", esp_err_to_name(err));
        status_set_error(MUSIC_STATE_ERROR, "storage 挂载失败");
        return err;
    }

    if (s_cmd_q == NULL) {
        s_cmd_q = xQueueCreate(MUSIC_CMD_QUEUE_LEN, sizeof(player_msg_t));
        if (s_cmd_q == NULL) {
            status_set_error(MUSIC_STATE_ERROR, "命令队列创建失败");
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * 解码器不是自动注册的：基础解码器（PCM/MP3/FLAC 等）和容器解析器（WAV）要分别注册，
     * 漏掉这一步 esp_audio_simple_check_audio_type(WAV) 会返回 NOT_SUPPORT。
     * 注册哪些编解码器由 Kconfig（CONFIG_AUDIO_*_SUPPORT）决定。
     */
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    if (audio_open() != ESP_OK) {
        status_set_error(MUSIC_STATE_ERROR, "ES8311 初始化失败");
        goto fail;
    }

    char track[64] = { 0 };
    if (!find_track(track, sizeof(track))) {
        ESP_LOGW(TAG, "%s 里没有 %s*%s", MUSIC_STORAGE_BASE,
                 MUSIC_TRACK_PREFIX, MUSIC_TRACK_SUFFIX);
        status_set_error(MUSIC_STATE_NO_TRACK, "storage 里没有 music-*.wav");
        goto no_local;
    }
    snprintf(s_local_track, sizeof(s_local_track), "%s", track);

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", MUSIC_STORAGE_BASE, track);
    s_file = fopen(path, "rb");
    if (s_file == NULL) {
        ESP_LOGE(TAG, "open %s failed", path);
        status_set_error(MUSIC_STATE_ERROR, "打不开 %s", track);
        goto no_local;
    }

    if (wav_parse(s_file, &s_wav) != ESP_OK) {
        ESP_LOGE(TAG, "%s is not a valid WAV", track);
        status_set_error(MUSIC_STATE_ERROR, "不是有效的 WAV");
        goto fail;
    }
    const uint32_t duration_ms = (uint32_t)((uint64_t)s_wav.data_size * 1000 / s_wav.byte_rate);
    ESP_LOGI(TAG, "wav: %u Hz / %u ch / %u bit, %u ms, data=%u B @%u",
             (unsigned)s_wav.sample_rate, (unsigned)s_wav.channels, (unsigned)s_wav.bits,
             (unsigned)duration_ms, (unsigned)s_wav.data_size, (unsigned)s_wav.data_offset);

    if (s_wav.sample_rate != MUSIC_SAMPLE_RATE || s_wav.channels != MUSIC_CHANNELS ||
            s_wav.bits != MUSIC_BITS_PER_SAMPLE) {
        ESP_LOGE(TAG, "unsupported format, need %d Hz / %d ch / %d bit",
                 MUSIC_SAMPLE_RATE, MUSIC_CHANNELS, MUSIC_BITS_PER_SAMPLE);
        status_set_error(MUSIC_STATE_ERROR, "仅支持 %d Hz/%d bit/单声道", MUSIC_SAMPLE_RATE,
                         MUSIC_BITS_PER_SAMPLE);
        goto fail;
    }

    if (esp_audio_simple_check_audio_type(ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "WAV simple decoder not registered");
        status_set_error(MUSIC_STATE_ERROR, "固件没编进 WAV 解码器");
        goto fail;
    }

    if (decoder_open(ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) != ESP_OK) {
        status_set_error(MUSIC_STATE_ERROR, "解码器打开失败");
        goto fail;
    }

    /* 从文件头开始：解码器需要先看到 RIFF/WAVE 头 */
    if (!track_rewind()) {
        status_set_error(MUSIC_STATE_ERROR, "回到文件头失败");
        goto fail;
    }

    status_set_media(track, NULL, MUSIC_SOURCE_LOCAL, duration_ms);

    if (xTaskCreate(play_task, "music.play", 8192, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "play task create failed");
        status_set_error(MUSIC_STATE_ERROR, "播放任务创建失败");
        goto fail;
    }

    /* 进 app 不自动出声：music 现在主要当投屏音箱用，等手机推流才开始播。
       本地曲目已经解析好停在开头，手机上没投屏时按界面播放键也能放。 */
    status_set_state(MUSIC_STATE_PAUSED);
    log_heap("start");
    ESP_LOGI(TAG, "%s ready (%u ms), waiting for cast", track, (unsigned)duration_ms);
    return ESP_OK;

no_local:
    /* 没有本地曲目也能当投屏音箱用：喇叭已就绪，等手机推流即可 */
    if (xTaskCreate(play_task, "music.play", 8192, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "play task create failed");
        goto fail;
    }
    return ESP_ERR_NOT_FOUND;

fail:
    if (s_dec != NULL) {
        esp_audio_simple_dec_close(s_dec);
        s_dec = NULL;
    }
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    return ESP_FAIL;
}

esp_err_t music_player_toggle(void)
{
    taskENTER_CRITICAL(&s_lock);
    const music_state_t state = s_status.state;
    taskEXIT_CRITICAL(&s_lock);

    if (state == MUSIC_STATE_PLAYING) {
        status_set_state(MUSIC_STATE_PAUSED);
        ESP_LOGI(TAG, "paused");
        return ESP_OK;
    }
    if (state == MUSIC_STATE_PAUSED) {
        status_set_state(MUSIC_STATE_PLAYING);
        ESP_LOGI(TAG, "resumed");
        return ESP_OK;
    }
    if (state == MUSIC_STATE_STOPPED) {
        /* 投屏流放完了：再点一次从头播 */
        return music_player_resume();
    }
    ESP_LOGW(TAG, "nothing to toggle (state=%s)", music_player_state_text(state));
    return ESP_ERR_INVALID_STATE;
}

/* -------------------------------------------------------------------------- */
/*  投屏控制（任意任务可调）                                                   */
/* -------------------------------------------------------------------------- */

esp_err_t music_player_play_url(const char *url, const char *title, const char *artist,
                                uint32_t duration_ms)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    player_msg_t msg = { 0 };
    msg.cmd = PCMD_SET_URL;
    msg.duration_ms = duration_ms;
    snprintf(msg.url, sizeof(msg.url), "%s", url);
    snprintf(msg.title, sizeof(msg.title), "%s", title != NULL ? title : "");
    snprintf(msg.artist, sizeof(msg.artist), "%s", artist != NULL ? artist : "");
    return post_msg(&msg);
}

esp_err_t music_player_resume(void)
{
    const player_msg_t msg = { .cmd = PCMD_PLAY };
    return post_msg(&msg);
}

esp_err_t music_player_pause(void)
{
    const player_msg_t msg = { .cmd = PCMD_PAUSE };
    return post_msg(&msg);
}

esp_err_t music_player_stop(void)
{
    const player_msg_t msg = { .cmd = PCMD_STOP };
    return post_msg(&msg);
}

esp_err_t music_player_seek(uint32_t position_ms)
{
    player_msg_t msg = { 0 };
    msg.cmd = PCMD_SEEK;
    msg.value = position_ms;
    return post_msg(&msg);
}

esp_err_t music_player_set_volume(int volume)
{
    player_msg_t msg = { 0 };
    msg.cmd = PCMD_VOLUME;
    msg.value = (uint32_t)volume;
    return post_msg(&msg);
}
