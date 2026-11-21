#include "xz_audio.h"

#include <stdlib.h>
#include <string.h>

#include "app_settings.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_audio_dec.h"
#include "esp_audio_enc.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "xz_config.h"

static const char *TAG = "xz.audio";

/* 60ms @16k@24kbps 约 180 字节，留足余量 */
#define OPUS_PACKET_MAX     1024
#define DECODE_PCM_MAX      4096

typedef struct {
    uint8_t *data;
    size_t   len;
} play_item_t;

static esp_codec_dev_handle_t s_spk = NULL;
static esp_codec_dev_handle_t s_mic = NULL;

static void *s_enc = NULL;
static void *s_dec = NULL;

static QueueHandle_t s_play_queue = NULL;
static TaskHandle_t s_capture_task = NULL;
static TaskHandle_t s_play_task = NULL;

static volatile bool s_capturing = false;
static volatile bool s_running = false;
static volatile bool s_playing = false;

static xz_audio_capture_cb_t s_capture_cb = NULL;
static void *s_capture_ctx = NULL;

/* -------------------------------------------------------------------------- */

static i2s_std_config_t make_i2s_config(void)
{
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(XZ_AUDIO_SAMPLE_RATE),
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

static esp_err_t opus_codec_init(void)
{
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = XZ_AUDIO_SAMPLE_RATE;
    enc_cfg.channel = 1;
    enc_cfg.bits_per_sample = 16;
    enc_cfg.bitrate = XZ_OPUS_BITRATE;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity = XZ_OPUS_COMPLEXITY;
    enc_cfg.enable_vbr = false;

    esp_audio_err_t err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &s_enc);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus enc open failed: %d", err);
        return ESP_FAIL;
    }

    esp_opus_dec_cfg_t dec_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec_cfg.sample_rate = XZ_AUDIO_SAMPLE_RATE;
    dec_cfg.channel = 1;
    dec_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    dec_cfg.self_delimited = false;

    err = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &s_dec);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "opus dec open failed: %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

static void capture_task(void *arg)
{
    /* 采集缓冲同样必须在内部 RAM：I2S RX DMA 不能直接写 PSRAM */
    uint8_t *pcm = heap_caps_malloc(XZ_PCM_FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t opus[OPUS_PACKET_MAX];
    uint32_t frames = 0;
    uint32_t last_report = esp_log_timestamp();
    int peak = 0;

    if (pcm == NULL) {
        ESP_LOGE(TAG, "capture buffer alloc failed");
        s_capture_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ESP_LOGI(TAG, "capture task started (%d bytes/frame)", XZ_PCM_FRAME_BYTES);

    while (s_running) {
        const int ret = esp_codec_dev_read(s_mic, pcm, XZ_PCM_FRAME_BYTES);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_capturing || s_capture_cb == NULL) {
            continue;   /* 半双工静默期：数据丢弃 */
        }

        /* 诊断：统计 2s 内的峰值，用来判断麦克风是否真的收到声音 */
        const int16_t *samples = (const int16_t *)pcm;
        for (size_t i = 0; i < XZ_PCM_FRAME_BYTES / sizeof(int16_t); i++) {
            int v = samples[i];
            if (v < 0) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
        }

        esp_audio_enc_in_frame_t in_frame = {
            .buffer = pcm,
            .len = XZ_PCM_FRAME_BYTES,
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = opus,
            .len = sizeof(opus),
        };
        if (esp_opus_enc_process(s_enc, &in_frame, &out_frame) != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "opus encode failed");
            continue;
        }
        if (out_frame.encoded_bytes > 0) {
            s_capture_cb(opus, out_frame.encoded_bytes, s_capture_ctx);
        }

        frames++;
        const uint32_t now = esp_log_timestamp();
        if (now - last_report >= 2000) {
            ESP_LOGI(TAG, "mic tx %u frames / %ums, peak %d",
                     (unsigned)frames, (unsigned)(now - last_report), peak);
            frames = 0;
            peak = 0;
            last_report = now;
        }
    }

    ESP_LOGI(TAG, "capture task exit");
    free(pcm);
    s_capture_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void play_task(void *arg)
{
    /* 解码后的 PCM 存放必须在内部 RAM：I2S DMA 不能直接访问 PSRAM */
    uint8_t *pcm = heap_caps_malloc(DECODE_PCM_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        ESP_LOGE(TAG, "play buffer alloc failed");
        s_play_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    play_item_t item;
    uint32_t played = 0;

    while (s_running) {
        if (xQueueReceive(s_play_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (s_playing) {
                s_playing = false;
            }
            continue;
        }

        s_playing = true;

        esp_audio_dec_in_raw_t raw = {
            .buffer = item.data,
            .len = item.len,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = pcm,
            .len = DECODE_PCM_MAX,
        };
        esp_audio_dec_info_t info = {0};

        while (raw.len > 0) {
            esp_audio_err_t err = esp_opus_dec_decode(s_dec, &raw, &frame, &info);
            if (err != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
                if (err != ESP_AUDIO_ERR_OK) {
                    ESP_LOGD(TAG, "opus decode err: %d", err);
                }
                break;
            }
            if (esp_codec_dev_write(s_spk, frame.buffer, frame.decoded_size) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "speaker write failed");
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            frame.len = DECODE_PCM_MAX;
        }

        free(item.data);
        s_playing = false;

        /* 诊断：解码/写喇叭这一段有没有吃内部 RAM */
        if ((++played % 10) == 0) {
            ESP_LOGI(TAG, "[heap] play %u int=%u dma=%u", (unsigned)played,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        }
    }

    free(pcm);
    s_play_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* -------------------------------------------------------------------------- */

esp_err_t xz_audio_init(xz_audio_capture_cb_t cb, void *ctx)
{
    s_capture_cb = cb;
    s_capture_ctx = ctx;

    i2s_std_config_t i2s_cfg = make_i2s_config();
    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    if (s_spk == NULL || s_mic == NULL) {
        ESP_LOGE(TAG, "codec init failed (spk=%p mic=%p)", s_spk, s_mic);
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = XZ_AUDIO_BITS_PER_SAMPLE,
        .channel = XZ_AUDIO_CHANNELS,
        .channel_mask = 0,
        .sample_rate = XZ_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        return ESP_FAIL;
    }
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphone open failed");
        return ESP_FAIL;
    }

    /* 音量统一由 selector 设置页写进 NVS（app_settings），Kconfig 值只作默认 */
    const int volume = app_settings_get_speaker_volume();
    esp_codec_dev_set_out_vol(s_spk, volume);
    esp_codec_dev_set_in_gain(s_mic, (float)CONFIG_XZ_MIC_GAIN_DB);
    ESP_LOGI(TAG, "speaker volume %d%%", volume);

    if (opus_codec_init() != ESP_OK) {
        return ESP_FAIL;
    }

    s_play_queue = xQueueCreate(XZ_PLAY_QUEUE_DEPTH, sizeof(play_item_t));
    if (s_play_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_running = true;
    /*
     * 栈需求：libopus 编解码 + 回调里的 MQTT/UDP 加密发送（走 TLS/AES）。
     * - 采集：16KB 实测会 stack overflow，给 40KB
     * - 播放：12KB 实测会 stack overflow（解码 + esp_codec_dev_write），给 32KB
     * 栈放 PSRAM：74KB 内部 RAM 一旦被占走，LVGL 刷屏时
     * spicommon 的 DMA 中转缓冲（每帧 46KB）和 mbedtls 会分配失败。
     * 注意 PCM 缓冲不能跟着栈走 PSRAM，必须单独 heap_caps_malloc 到内部 RAM。
     */
    if (xTaskCreateWithCaps(play_task, "xz_play", 32768, NULL, 5, &s_play_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(capture_task, "xz_cap", 40960, NULL, 6, &s_capture_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "audio ready: %d Hz / %d ch / %d bit, opus %d ms",
             XZ_AUDIO_SAMPLE_RATE, XZ_AUDIO_CHANNELS, XZ_AUDIO_BITS_PER_SAMPLE,
             XZ_OPUS_FRAME_MS);
    return ESP_OK;
}

esp_err_t xz_audio_set_capture(bool enable)
{
    s_capturing = enable;
    return ESP_OK;
}

esp_err_t xz_audio_play_opus(const uint8_t *data, size_t len)
{
    if (s_play_queue == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 包缓冲优先放 PSRAM：解码是 CPU 逐字节读，不需要 DMA 能力。
     * 用 malloc 的话（<16KB 会落进内部 RAM）每包都要在内部堆里申请/释放，
     * 正好和 esp-aes 每包申请的临时 DMA 缓冲抢同一片内存，
     * 多轮播报后 AES 那边就会 ESP_ERR_NO_MEM，音频包全部解不出来。
     */
    play_item_t item = {
        .data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .len = len,
    };
    if (item.data == NULL) {
        item.data = malloc(len);      /* PSRAM 拿不到时退回内部 RAM */
    }
    if (item.data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(item.data, data, len);

    if (xQueueSend(s_play_queue, &item, 0) != pdTRUE) {
        free(item.data);
        ESP_LOGW(TAG, "play queue full, drop %u bytes", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void xz_audio_flush(void)
{
    if (s_play_queue == NULL) {
        return;
    }
    play_item_t item;
    while (xQueueReceive(s_play_queue, &item, 0) == pdTRUE) {
        free(item.data);
    }
}

bool xz_audio_is_busy(void)
{
    return s_playing || (s_play_queue != NULL && uxQueueMessagesWaiting(s_play_queue) > 0);
}
