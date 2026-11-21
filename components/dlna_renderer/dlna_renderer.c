/*
 * DLNA/UPnP MediaRenderer 的协议部分（只覆盖 QQ 音乐 QPlay 实测用到的动作）。
 *
 * 实测（iOS QQ 音乐，QPlay v1，见 tmp/dlna_probe/probe.log）手机只发：
 *   SetAVTransportURI / Play / Pause / Stop / Seek(REL_TIME)
 *   GetTransportInfo / GetPositionInfo / GetMediaInfo / GetVolume
 *   GetProtocolInfo / SUBSCRIBE / UNSUBSCRIBE
 * 切歌 = 新的 SetAVTransportURI + Play；音量极少下发。
 *
 * 三件事必须做对，否则手机会"看起来连上了但播不了"：
 *   1. GetPositionInfo 返回真实位置，否则拖进度会算错目标（旧探针恒返回 0:00）
 *   2. Play 要区分"新曲开播"和"暂停后继续"（手机用同一个 Play）
 *   3. Seek 用 REL_TIME，需要在 app 侧换算成字节偏移并重新 Range 拉流
 *
 * 内存：所有缓冲在 start 时一次性分配（优先 PSRAM），之后不再 malloc，
 * 避免与 LVGL/解码器抢内部 RAM 造成碎片。
 */
#include "dlna_renderer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlna_ssdp.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "dlna"

/* 描述/SCPD/SOAP 响应共用的生成缓冲。
   注意：AVTransport 的 SCPD 实测 ~14 KB（13 个动作 + 20 多个状态变量），
   12 KB 会把 XML 截断成非法文档 —— 手机拿到半截 SCPD 后会把设备从列表里丢掉。 */
#define XML_CAP          (24 * 1024)
#define SOAP_IN_CAP      (6 * 1024)     /* 请求体上限：带 DIDL 的 SetAVTransportURI ~1.5 KB */
#define URI_CAP          640     /* QQ 音乐带 vkey 的链接实测 300~400 字节，留足余量 */
#define META_CAP         2048

/* -------------------------------------------------------------------------- */
/*  静态状态                                                                   */
/* -------------------------------------------------------------------------- */

static struct {
    dlna_transport_t transport;
    uint32_t         position_ms;
    uint32_t         duration_ms;
    int              volume;
    bool             has_media;
    char             uri[URI_CAP];
    char             metadata[META_CAP];
    dlna_media_info_t media;
} s_state;

static SemaphoreHandle_t s_state_lock = NULL;
static SemaphoreHandle_t s_build_lock = NULL;

static httpd_handle_t s_httpd = NULL;
static dlna_cmd_cb_t s_on_cmd = NULL;
static void *s_user = NULL;
static char s_uuid[48];
static char s_base_url[64];
static char s_friendly[48] = "ESP32-DLNA";
static char s_model[48] = "ESP32-S3 DLNA Renderer";
static uint16_t s_port = 0;

/* 生成缓冲与请求体缓冲：start 时一次分配，优先 PSRAM（HTTP 发送不需要 DMA） */
static char *s_xml = NULL;
static char *s_soap_in = NULL;
/* 只在 httpd 任务里用的临时串（esp_http_server 串行执行 handler） */
static char s_uri_tmp[URI_CAP];
static char s_meta_tmp[META_CAP];

/* -------------------------------------------------------------------------- */
/*  字符串 / 时间工具                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} sb_t;

static void sb_printf(sb_t *b, const char *fmt, ...)
{
    if (b->len + 1 >= b->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        b->len += (size_t)n;
        if (b->len >= b->cap) {
            b->len = b->cap - 1;
        }
    }
}

static void sb_escape(sb_t *b, const char *s)
{
    if (s == NULL) {
        return;
    }
    for (; *s != '\0'; s++) {
        switch (*s) {
        case '&':  sb_printf(b, "&amp;"); break;
        case '<':  sb_printf(b, "&lt;"); break;
        case '>':  sb_printf(b, "&gt;"); break;
        case '"':  sb_printf(b, "&quot;"); break;
        case '\'': sb_printf(b, "&apos;"); break;
        default:   sb_printf(b, "%c", *s); break;
        }
    }
}

/** @brief 把 SOAP 里的 XML 实体还原（&lt;DIDL-Lite...&gt; -> 原始 DIDL） */
static void xml_unescape(char *s)
{
    char *r = s;
    char *w = s;
    while (*r != '\0') {
        if (*r == '&') {
            if (strncmp(r, "&lt;", 4) == 0) {
                *w++ = '<';
                r += 4;
                continue;
            }
            if (strncmp(r, "&gt;", 4) == 0) {
                *w++ = '>';
                r += 4;
                continue;
            }
            if (strncmp(r, "&amp;", 5) == 0) {
                *w++ = '&';
                r += 5;
                continue;
            }
            if (strncmp(r, "&quot;", 6) == 0) {
                *w++ = '"';
                r += 6;
                continue;
            }
            if (strncmp(r, "&apos;", 6) == 0) {
                *w++ = '\'';
                r += 6;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/** @brief 取 <tag>value</tag>（不含命名空间前缀匹配，QPlay 用的是无前缀标签） */
static bool body_tag(const char *body, const char *tag, char *out, size_t out_len)
{
    char open[40];
    char close[40];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *p = strstr(body, open);
    if (p == NULL) {
        return false;
    }
    p += strlen(open);
    const char *q = strstr(p, close);
    if (q == NULL) {
        return false;
    }
    size_t n = (size_t)(q - p);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    xml_unescape(out);
    return true;
}

/** @brief "HH:MM:SS" / "H:MM:SS.mmm" -> 毫秒 */
static uint32_t parse_time_ms(const char *text)
{
    unsigned h = 0;
    unsigned m = 0;
    double s = 0.0;
    if (sscanf(text, "%u:%u:%lf", &h, &m, &s) != 3) {
        return 0;
    }
    return (uint32_t)(((uint64_t)h * 3600 + (uint64_t)m * 60) * 1000 + (uint64_t)(s * 1000.0));
}

static void format_time_ms(char *buf, size_t len, uint32_t ms)
{
    const uint32_t total_s = ms / 1000;
    snprintf(buf, len, "%02u:%02u:%02u", (unsigned)(total_s / 3600),
             (unsigned)((total_s / 60) % 60), (unsigned)(total_s % 60));
}

/* -------------------------------------------------------------------------- */
/*  DIDL-Lite 解析（拿到歌名/歌手/时长，给界面用）                              */
/* -------------------------------------------------------------------------- */

/** @brief 取 <tag>..</tag> 的裸文本并转义还原；找不到保持 out 不变 */
static void didl_tag(const char *didl, const char *tag, char *out, size_t out_len)
{
    char open[40];
    char close[40];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *p = strstr(didl, open);
    if (p == NULL) {
        return;
    }
    p += strlen(open);
    const char *q = strstr(p, close);
    if (q == NULL) {
        return;
    }
    size_t n = (size_t)(q - p);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    xml_unescape(out);
}

/** @brief 从 <res ... duration="00:04:39" protocolInfo="http-get:*:audio/mpeg:..."> 里取属性 */
static void didl_res_attr(const char *didl, const char *attr, char *out, size_t out_len)
{
    const char *res = strstr(didl, "<res");
    if (res == NULL) {
        return;
    }
    char key[32];
    snprintf(key, sizeof(key), "%s=\"", attr);
    const char *p = strstr(res, key);
    if (p == NULL) {
        return;
    }
    p += strlen(key);
    const char *q = strchr(p, '"');
    if (q == NULL) {
        return;
    }
    size_t n = (size_t)(q - p);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    xml_unescape(out);
}

static void parse_didl(const char *didl, dlna_media_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (didl == NULL || didl[0] == '\0') {
        return;
    }
    out->has_media = true;
    didl_tag(didl, "dc:title", out->title, sizeof(out->title));
    didl_tag(didl, "dc:creator", out->artist, sizeof(out->artist));
    didl_tag(didl, "upnp:album", out->album, sizeof(out->album));

    char duration[24] = { 0 };
    didl_res_attr(didl, "duration", duration, sizeof(duration));
    out->duration_ms = parse_time_ms(duration);

    /* 探测专辑封面：UPnP 标准标签 upnp:albumArtURI 可能带属性
       （<upnp:albumArtURI dlna:profileID="JPEG_TN">），所以不能按整标签匹配。
       先把地址打出来确认手机到底给不给，再决定设备端要不要做下载+解码。 */
    const char *art = strstr(didl, "albumArtURI");
    if (art != NULL) {
        const char *gt = strchr(art, '>');
        const char *end = (gt != NULL) ? strstr(gt, "</") : NULL;
        if (gt != NULL && end != NULL && end > gt + 1) {
            size_t n = (size_t)(end - (gt + 1));
            if (n > sizeof(out->album_art) - 1) {
                n = sizeof(out->album_art) - 1;
            }
            memcpy(out->album_art, gt + 1, n);
            out->album_art[n] = '\0';
            xml_unescape(out->album_art);
            ESP_LOGI(TAG, "didl albumArtURI: %s", out->album_art);
        }
    } else {
        ESP_LOGW(TAG, "didl has no albumArtURI (didl %u B)", (unsigned)strlen(didl));
    }

    /* protocolInfo="http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;..." 取第三段 */
    char proto[96] = { 0 };
    didl_res_attr(didl, "protocolInfo", proto, sizeof(proto));
    const char *p1 = strchr(proto, ':');
    const char *p2 = (p1 != NULL) ? strchr(p1 + 1, ':') : NULL;
    const char *p3 = (p2 != NULL) ? strchr(p2 + 1, ':') : NULL;
    if (p2 != NULL && p3 != NULL && p3 > p2 + 1) {
        size_t n = (size_t)(p3 - (p2 + 1));
        if (n >= sizeof(out->mime)) {
            n = sizeof(out->mime) - 1;
        }
        memcpy(out->mime, p2 + 1, n);
        out->mime[n] = '\0';
    }
}

/* -------------------------------------------------------------------------- */
/*  SCPD（服务描述）：表驱动生成，QPlay 实测不取，但部分 Android 端会取           */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *dir;
    const char *var;
} arg_spec_t;

typedef struct {
    const char *name;
    const arg_spec_t *args;
    uint8_t     arg_count;
} action_spec_t;

typedef struct {
    const char *name;
    const char *type;
    const char *def;
    const char *allowed;    /* 逗号分隔；NULL 表示不列 allowedValueList */
} var_spec_t;

#define ARGS(...) (const arg_spec_t[]){ __VA_ARGS__ }

static const action_spec_t AVT_ACTIONS[] = {
    { "SetAVTransportURI", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                                { "CurrentURI", "in", "AVTransportURI" },
                                { "CurrentURIMetaData", "in", "AVTransportURIMetaData" }), 3 },
    { "SetNextAVTransportURI", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                                    { "NextURI", "in", "NextAVTransportURI" },
                                    { "NextURIMetaData", "in", "NextAVTransportURIMetaData" }), 3 },
    { "GetMediaInfo", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                           { "NrTracks", "out", "NumberOfTracks" },
                           { "MediaDuration", "out", "CurrentMediaDuration" },
                           { "CurrentURI", "out", "AVTransportURI" },
                           { "CurrentURIMetaData", "out", "AVTransportURIMetaData" },
                           { "NextURI", "out", "NextAVTransportURI" },
                           { "NextURIMetaData", "out", "NextAVTransportURIMetaData" },
                           { "PlayMedium", "out", "PlaybackStorageMedium" },
                           { "RecordMedium", "out", "RecordStorageMedium" },
                           { "WriteStatus", "out", "RecordMediumWriteStatus" }), 10 },
    { "GetTransportInfo", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                               { "CurrentTransportState", "out", "TransportState" },
                               { "CurrentTransportStatus", "out", "TransportStatus" },
                               { "CurrentSpeed", "out", "TransportPlaySpeed" }), 4 },
    { "GetPositionInfo", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                              { "Track", "out", "CurrentTrack" },
                              { "TrackDuration", "out", "CurrentTrackDuration" },
                              { "TrackMetaData", "out", "CurrentTrackMetaData" },
                              { "TrackURI", "out", "CurrentTrackURI" },
                              { "RelTime", "out", "RelativeTimePosition" },
                              { "AbsTime", "out", "AbsoluteTimePosition" },
                              { "RelCount", "out", "RelativeCounterPosition" },
                              { "AbsCount", "out", "AbsoluteCounterPosition" }), 9 },
    { "GetDeviceCapabilities", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                                    { "PlayMedia", "out", "PossiblePlaybackStorageMedia" },
                                    { "RecMedia", "out", "PossibleRecordStorageMedia" },
                                    { "RecQualityModes", "out", "PossibleRecordQualityModes" }), 4 },
    { "GetTransportSettings", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                                   { "PlayMode", "out", "CurrentPlayMode" },
                                   { "RecQualityMode", "out", "CurrentRecordQualityMode" }), 3 },
    { "GetCurrentTransportActions", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                                         { "Actions", "out", "CurrentTransportActions" }), 2 },
    { "Stop", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" }), 1 },
    { "Play", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                   { "Speed", "in", "TransportPlaySpeed" }), 2 },
    { "Pause", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" }), 1 },
    { "Seek", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                   { "Unit", "in", "A_ARG_TYPE_SeekMode" },
                   { "Target", "in", "A_ARG_TYPE_SeekTarget" }), 3 },
    { "Next", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" }), 1 },
    { "Previous", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" }), 1 },
};

static const var_spec_t AVT_VARS[] = {
    { "TransportState", "string", "STOPPED", "STOPPED,PLAYING,PAUSED_PLAYBACK,TRANSITIONING,NO_MEDIA_PRESENT" },
    { "TransportStatus", "string", "OK", "OK,ERROR_OCCURRED" },
    { "PlaybackStorageMedium", "string", "NETWORK", "NONE,NETWORK" },
    { "RecordStorageMedium", "string", "NOT_IMPLEMENTED", "NOT_IMPLEMENTED" },
    { "PossiblePlaybackStorageMedia", "string", "NETWORK", NULL },
    { "PossibleRecordStorageMedia", "string", "NOT_IMPLEMENTED", NULL },
    { "CurrentPlayMode", "string", "NORMAL", "NORMAL" },
    { "TransportPlaySpeed", "string", "1", "1" },
    { "RecordMediumWriteStatus", "string", "NOT_IMPLEMENTED", "NOT_IMPLEMENTED" },
    { "CurrentRecordQualityMode", "string", "NOT_IMPLEMENTED", "NOT_IMPLEMENTED" },
    { "PossibleRecordQualityModes", "string", "NOT_IMPLEMENTED", NULL },
    { "NumberOfTracks", "ui4", "0", NULL },
    { "CurrentTrack", "ui4", "0", NULL },
    { "CurrentTrackDuration", "string", "00:00:00", NULL },
    { "CurrentMediaDuration", "string", "00:00:00", NULL },
    { "CurrentTrackMetaData", "string", "", NULL },
    { "CurrentTrackURI", "string", "", NULL },
    { "AVTransportURI", "string", "", NULL },
    { "AVTransportURIMetaData", "string", "", NULL },
    { "NextAVTransportURI", "string", "", NULL },
    { "NextAVTransportURIMetaData", "string", "", NULL },
    { "RelativeTimePosition", "string", "00:00:00", NULL },
    { "AbsoluteTimePosition", "string", "00:00:00", NULL },
    { "RelativeCounterPosition", "i4", "2147483647", NULL },
    { "AbsoluteCounterPosition", "i4", "2147483647", NULL },
    { "CurrentTransportActions", "string", "Play,Stop,Pause", NULL },
    { "LastChange", "string", "", NULL },
    { "A_ARG_TYPE_SeekMode", "string", "REL_TIME", "REL_TIME,TRACK_NR" },
    { "A_ARG_TYPE_SeekTarget", "string", "", NULL },
    { "A_ARG_TYPE_InstanceID", "ui4", "0", NULL },
};

static const action_spec_t RCS_ACTIONS[] = {
    { "ListPresets", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                          { "CurrentPresetNameList", "out", "PresetNameList" }), 2 },
    { "SelectPreset", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                           { "PresetName", "in", "A_ARG_TYPE_PresetName" }), 2 },
    { "GetMute", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                      { "Channel", "in", "A_ARG_TYPE_Channel" },
                      { "CurrentMute", "out", "Mute" }), 3 },
    { "SetMute", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                      { "Channel", "in", "A_ARG_TYPE_Channel" },
                      { "DesiredMute", "in", "Mute" }), 3 },
    { "GetVolume", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                        { "Channel", "in", "A_ARG_TYPE_Channel" },
                        { "CurrentVolume", "out", "Volume" }), 3 },
    { "SetVolume", ARGS({ "InstanceID", "in", "A_ARG_TYPE_InstanceID" },
                        { "Channel", "in", "A_ARG_TYPE_Channel" },
                        { "DesiredVolume", "in", "Volume" }), 3 },
};

static const var_spec_t RCS_VARS[] = {
    { "PresetNameList", "string", "FactoryDefaults", NULL },
    { "Mute", "boolean", "0", NULL },
    { "Volume", "ui2", "50", NULL },
    { "A_ARG_TYPE_Channel", "string", "Master", "Master" },
    { "A_ARG_TYPE_InstanceID", "ui4", "0", NULL },
    { "A_ARG_TYPE_PresetName", "string", "FactoryDefaults", "FactoryDefaults" },
};

static const action_spec_t CMS_ACTIONS[] = {
    { "GetProtocolInfo", ARGS({ "Source", "out", "SourceProtocolInfo" },
                              { "Sink", "out", "SinkProtocolInfo" }), 2 },
    { "GetCurrentConnectionIDs", ARGS({ "ConnectionIDs", "out", "CurrentConnectionIDs" }), 1 },
    { "GetCurrentConnectionInfo", ARGS({ "ConnectionID", "in", "A_ARG_TYPE_ConnectionID" },
                                       { "RcsID", "out", "A_ARG_TYPE_RcsID" },
                                       { "AVTransportID", "out", "A_ARG_TYPE_AVTransportID" },
                                       { "ProtocolInfo", "out", "A_ARG_TYPE_ProtocolInfo" },
                                       { "PeerConnectionManager", "out", "A_ARG_TYPE_ConnectionManager" },
                                       { "PeerConnectionID", "out", "A_ARG_TYPE_ConnectionID" },
                                       { "Direction", "out", "A_ARG_TYPE_Direction" },
                                       { "Status", "out", "A_ARG_TYPE_ConnectionStatus" }), 8 },
};

static const var_spec_t CMS_VARS[] = {
    { "SourceProtocolInfo", "string", "", NULL },
    { "SinkProtocolInfo", "string", "", NULL },
    { "CurrentConnectionIDs", "string", "0", NULL },
    { "A_ARG_TYPE_ConnectionStatus", "string", "OK", "OK,ContentFormatMismatch,InsufficientBandwidth,UnreliableChannel,Unknown" },
    { "A_ARG_TYPE_ConnectionManager", "string", "", NULL },
    { "A_ARG_TYPE_Direction", "string", "Input", "Input,Output" },
    { "A_ARG_TYPE_ProtocolInfo", "string", "", NULL },
    { "A_ARG_TYPE_ConnectionID", "i4", "0", NULL },
    { "A_ARG_TYPE_AVTransportID", "i4", "0", NULL },
    { "A_ARG_TYPE_RcsID", "i4", "0", NULL },
};

typedef struct {
    const char *name;       /* AVTransport */
    const char *type;
    const char *control;    /* /control/AVTransport */
    const action_spec_t *actions;
    size_t action_count;
    const var_spec_t *vars;
    size_t var_count;
} service_spec_t;

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static const service_spec_t SERVICES[] = {
    { "AVTransport", "urn:schemas-upnp-org:service:AVTransport:1", "/control/AVTransport",
      AVT_ACTIONS, COUNT_OF(AVT_ACTIONS), AVT_VARS, COUNT_OF(AVT_VARS) },
    { "RenderingControl", "urn:schemas-upnp-org:service:RenderingControl:1", "/control/RenderingControl",
      RCS_ACTIONS, COUNT_OF(RCS_ACTIONS), RCS_VARS, COUNT_OF(RCS_VARS) },
    { "ConnectionManager", "urn:schemas-upnp-org:service:ConnectionManager:1", "/control/ConnectionManager",
      CMS_ACTIONS, COUNT_OF(CMS_ACTIONS), CMS_VARS, COUNT_OF(CMS_VARS) },
};

static const service_spec_t *service_by_name(const char *name)
{
    for (size_t i = 0; i < COUNT_OF(SERVICES); i++) {
        if (strcmp(SERVICES[i].name, name) == 0) {
            return &SERVICES[i];
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  XML 文档生成                                                               */
/* -------------------------------------------------------------------------- */

static int build_scpd(const service_spec_t *svc)
{
    sb_t b = { s_xml, XML_CAP, 0 };
    sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
                  "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>");
    for (size_t i = 0; i < svc->action_count; i++) {
        const action_spec_t *a = &svc->actions[i];
        sb_printf(&b, "<action><name>%s</name><argumentList>", a->name);
        for (uint8_t k = 0; k < a->arg_count; k++) {
            sb_printf(&b, "<argument><name>%s</name><direction>%s</direction>"
                          "<relatedStateVariable>%s</relatedStateVariable></argument>",
                      a->args[k].name, a->args[k].dir, a->args[k].var);
        }
        sb_printf(&b, "</argumentList></action>");
    }
    sb_printf(&b, "</actionList><serviceStateTable>");
    for (size_t i = 0; i < svc->var_count; i++) {
        const var_spec_t *v = &svc->vars[i];
        sb_printf(&b, "<stateVariable sendEvents=\"no\"><name>%s</name><dataType>%s</dataType>"
                      "<defaultValue>%s</defaultValue>", v->name, v->type, v->def);
        if (v->allowed != NULL) {
            sb_printf(&b, "<allowedValueList>");
            const char *p = v->allowed;
            while (*p != '\0') {
                const char *comma = strchr(p, ',');
                const size_t n = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
                sb_printf(&b, "<allowedValue>%.*s</allowedValue>", (int)n, p);
                if (comma == NULL) {
                    break;
                }
                p = comma + 1;
            }
            sb_printf(&b, "</allowedValueList>");
        }
        sb_printf(&b, "</stateVariable>");
    }
    sb_printf(&b, "</serviceStateTable></scpd>");
    return (int)b.len;
}

static int build_description(void)
{
    sb_t b = { s_xml, XML_CAP, 0 };
    sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" "
                  "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
                  "<specVersion><major>1</major><minor>0</minor></specVersion><device>"
                  "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
                  "<friendlyName>%s</friendlyName>"
                  "<manufacturer>Espressif</manufacturer>"
                  "<manufacturerURL>https://www.espressif.com</manufacturerURL>"
                  "<modelDescription>ESP32-S3 DLNA renderer for QPlay</modelDescription>"
                  "<modelName>%s</modelName><modelNumber>0.2.0</modelNumber>"
                  "<UDN>%s</UDN><dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>"
                  "<serviceList>", s_friendly, s_model, s_uuid);
    for (size_t i = 0; i < COUNT_OF(SERVICES); i++) {
        const service_spec_t *s = &SERVICES[i];
        sb_printf(&b, "<service><serviceType>%s</serviceType>"
                      "<serviceId>urn:upnp-org:serviceId:%s</serviceId>"
                      "<SCPDURL>/scpd/%s.xml</SCPDURL>"
                      "<controlURL>%s</controlURL>"
                      "<eventSubURL>/event/%s</eventSubURL></service>",
                  s->type, s->name, s->name, s->control, s->name);
    }
    sb_printf(&b, "</serviceList></device></root>");
    return (int)b.len;
}

/* -------------------------------------------------------------------------- */
/*  SOAP 收发                                                                  */
/* -------------------------------------------------------------------------- */

static void soap_send(httpd_req_t *req, const char *svc_type, const char *action,
                      void (*body_builder)(sb_t *b, const void *ctx), const void *ctx)
{
    sb_t b = { s_xml, XML_CAP, 0 };
    sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                  "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                  "<u:%sResponse xmlns:u=\"%s\">", action, svc_type);
    if (body_builder != NULL) {
        body_builder(&b, ctx);
    }
    sb_printf(&b, "</u:%sResponse></s:Body></s:Envelope>", action);

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    httpd_resp_send(req, s_xml, (ssize_t)b.len);
}

static void soap_send_error(httpd_req_t *req, int code, const char *desc)
{
    sb_t b = { s_xml, XML_CAP, 0 };
    sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                  "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                  "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault>"
                  "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
                  "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
                  "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
                  "</UPnPError></detail></s:Fault></s:Body></s:Envelope>",
                  code, desc);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    httpd_resp_send(req, s_xml, (ssize_t)b.len);
}

/* -------------------------------------------------------------------------- */
/*  状态快照                                                                   */
/* -------------------------------------------------------------------------- */

static const char *transport_name(dlna_transport_t t)
{
    switch (t) {
    case DLNA_TRANSPORT_PLAYING:
        return "PLAYING";
    case DLNA_TRANSPORT_PAUSED:
        return "PAUSED_PLAYBACK";
    case DLNA_TRANSPORT_STOPPED:
        return "STOPPED";
    case DLNA_TRANSPORT_NO_MEDIA:
    default:
        return "NO_MEDIA_PRESENT";
    }
}

typedef struct {
    dlna_transport_t transport;
    uint32_t position_ms;
    uint32_t duration_ms;
    int volume;
    bool has_media;
    char uri[URI_CAP];
    char metadata[META_CAP];
    dlna_media_info_t media;
} state_snapshot_t;

/* 快照带 uri+metadata，约 2.7 KB，放静态区复用：
   HTTP 任务栈只有 6 KB，塞两次就溢出；调用者必须已持有 s_build_lock。 */
static state_snapshot_t s_snap;

static const state_snapshot_t *state_copy(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_snap.transport = s_state.transport;
    s_snap.position_ms = s_state.position_ms;
    s_snap.duration_ms = s_state.duration_ms;
    s_snap.volume = s_state.volume;
    s_snap.has_media = s_state.has_media;
    s_snap.media = s_state.media;
    snprintf(s_snap.uri, sizeof(s_snap.uri), "%s", s_state.uri);
    snprintf(s_snap.metadata, sizeof(s_snap.metadata), "%s", s_state.metadata);
    xSemaphoreGive(s_state_lock);
    return &s_snap;
}

/* -------------------------------------------------------------------------- */
/*  SOAP 响应的 body builder                                                   */
/* -------------------------------------------------------------------------- */

static void body_transport_info(sb_t *b, const void *ctx)
{
    const state_snapshot_t *st = ctx;
    sb_printf(b, "<CurrentTransportState>%s</CurrentTransportState>"
                 "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                 "<CurrentSpeed>1</CurrentSpeed>", transport_name(st->transport));
}

static void body_position_info(sb_t *b, const void *ctx)
{
    const state_snapshot_t *st = ctx;
    char duration[16];
    char rel[16];
    format_time_ms(duration, sizeof(duration), st->duration_ms);
    format_time_ms(rel, sizeof(rel), st->position_ms);
    sb_printf(b, "<Track>1</Track><TrackDuration>%s</TrackDuration><TrackMetaData>", duration);
    sb_escape(b, st->metadata);
    sb_printf(b, "</TrackMetaData><TrackURI>");
    sb_escape(b, st->uri);
    sb_printf(b, "</TrackURI><RelTime>%s</RelTime><AbsTime>%s</AbsTime>"
                 "<RelCount>2147483647</RelCount><AbsCount>2147483647</AbsCount>",
              rel, rel);
}

static void body_media_info(sb_t *b, const void *ctx)
{
    const state_snapshot_t *st = ctx;
    char duration[16];
    format_time_ms(duration, sizeof(duration), st->duration_ms);
    sb_printf(b, "<NrTracks>%d</NrTracks><MediaDuration>%s</MediaDuration><CurrentURI>",
              st->has_media ? 1 : 0, duration);
    sb_escape(b, st->uri);
    sb_printf(b, "</CurrentURI><CurrentURIMetaData>");
    sb_escape(b, st->metadata);
    sb_printf(b, "</CurrentURIMetaData><NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
                 "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                 "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
}

static void body_volume(sb_t *b, const void *ctx)
{
    const state_snapshot_t *st = ctx;
    sb_printf(b, "<CurrentVolume>%d</CurrentVolume>", st->volume);
}

/* -------------------------------------------------------------------------- */
/*  命令下发                                                                   */
/* -------------------------------------------------------------------------- */

static void emit_cmd(dlna_cmd_t cmd, const char *uri, uint32_t value)
{
    if (s_on_cmd != NULL) {
        s_on_cmd(cmd, uri, value, s_user);
    }
}

/* -------------------------------------------------------------------------- */
/*  控制处理                                                                   */
/* -------------------------------------------------------------------------- */

static void handle_avtransport(httpd_req_t *req, const char *action, const char *body,
                               const service_spec_t *svc)
{
    if (strcmp(action, "SetAVTransportURI") == 0) {
        const bool got_uri = body_tag(body, "CurrentURI", s_uri_tmp, sizeof(s_uri_tmp));
        body_tag(body, "CurrentURIMetaData", s_meta_tmp, sizeof(s_meta_tmp));

        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        if (!got_uri || s_uri_tmp[0] == '\0') {
            /* QPlay 切歌前会先发一次空 URI 清场 */
            s_state.has_media = false;
            s_state.transport = DLNA_TRANSPORT_NO_MEDIA;
            s_state.position_ms = 0;
            s_state.duration_ms = 0;
            s_state.uri[0] = '\0';
            s_state.metadata[0] = '\0';
            memset(&s_state.media, 0, sizeof(s_state.media));
            xSemaphoreGive(s_state_lock);
            ESP_LOGI(TAG, "clear media (empty URI)");
            emit_cmd(DLNA_CMD_STOP, NULL, 0);
        } else {
            /* 先在锁外解析，避免拿着状态锁做字符串扫描，也避免解锁后再读 s_state */
            dlna_media_info_t media;
            parse_didl(s_meta_tmp, &media);

            snprintf(s_state.uri, sizeof(s_state.uri), "%s", s_uri_tmp);
            snprintf(s_state.metadata, sizeof(s_state.metadata), "%s", s_meta_tmp);
            s_state.media = media;
            s_state.has_media = true;
            s_state.transport = DLNA_TRANSPORT_STOPPED;
            s_state.position_ms = 0;
            s_state.duration_ms = media.duration_ms;
            xSemaphoreGive(s_state_lock);

            ESP_LOGI(TAG, "set uri: title=%s artist=%s dur=%lu ms mime=%s",
                     media.title, media.artist, (unsigned long)media.duration_ms, media.mime);
            emit_cmd(DLNA_CMD_SET_URI, s_uri_tmp, 0);
        }
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    if (strcmp(action, "Play") == 0) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        const bool has_media = s_state.has_media;
        if (has_media) {
            s_state.transport = DLNA_TRANSPORT_PLAYING;
        }
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG, "Play (has_media=%d)", (int)has_media);
        if (has_media) {
            emit_cmd(DLNA_CMD_PLAY, NULL, 0);
        }
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    if (strcmp(action, "Pause") == 0) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        if (s_state.transport == DLNA_TRANSPORT_PLAYING) {
            s_state.transport = DLNA_TRANSPORT_PAUSED;
        }
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG, "Pause");
        emit_cmd(DLNA_CMD_PAUSE, NULL, 0);
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    if (strcmp(action, "Stop") == 0) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_state.transport = s_state.has_media ? DLNA_TRANSPORT_STOPPED : DLNA_TRANSPORT_NO_MEDIA;
        s_state.position_ms = 0;
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG, "Stop");
        emit_cmd(DLNA_CMD_STOP, NULL, 0);
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    if (strcmp(action, "Seek") == 0) {
        char unit[24] = { 0 };
        char target[24] = { 0 };
        body_tag(body, "Unit", unit, sizeof(unit));
        body_tag(body, "Target", target, sizeof(target));
        if (strcmp(unit, "REL_TIME") != 0) {
            soap_send_error(req, 710, "Seek mode not supported");
            return;
        }
        const uint32_t ms = parse_time_ms(target);
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_state.position_ms = ms;
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG, "Seek REL_TIME %s -> %lu ms", target, (unsigned long)ms);
        emit_cmd(DLNA_CMD_SEEK, NULL, ms);
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    if (strcmp(action, "GetTransportInfo") == 0) {
        soap_send(req, svc->type, action, body_transport_info, state_copy());
        return;
    }

    if (strcmp(action, "GetPositionInfo") == 0) {
        soap_send(req, svc->type, action, body_position_info, state_copy());
        return;
    }

    if (strcmp(action, "GetMediaInfo") == 0) {
        soap_send(req, svc->type, action, body_media_info, state_copy());
        return;
    }

    if (strcmp(action, "GetDeviceCapabilities") == 0) {
        /* 手机不强依赖，回合法字段即可 */
        sb_t b = { s_xml, XML_CAP, 0 };
        sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                      "<u:GetDeviceCapabilitiesResponse xmlns:u=\"%s\">"
                      "<PlayMedia>NETWORK</PlayMedia><RecMedia>NOT_IMPLEMENTED</RecMedia>"
                      "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>"
                      "</u:GetDeviceCapabilitiesResponse></s:Body></s:Envelope>", svc->type);
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_send(req, s_xml, (ssize_t)b.len);
        return;
    }

    if (strcmp(action, "GetTransportSettings") == 0) {
        sb_t b = { s_xml, XML_CAP, 0 };
        sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                      "<u:GetTransportSettingsResponse xmlns:u=\"%s\">"
                      "<PlayMode>NORMAL</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>"
                      "</u:GetTransportSettingsResponse></s:Body></s:Envelope>", svc->type);
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_send(req, s_xml, (ssize_t)b.len);
        return;
    }

    if (strcmp(action, "GetCurrentTransportActions") == 0) {
        sb_t b = { s_xml, XML_CAP, 0 };
        sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                      "<u:GetCurrentTransportActionsResponse xmlns:u=\"%s\">"
                      "<Actions>Play,Stop,Pause,Seek</Actions>"
                      "</u:GetCurrentTransportActionsResponse></s:Body></s:Envelope>", svc->type);
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_send(req, s_xml, (ssize_t)b.len);
        return;
    }

    if (strcmp(action, "SetNextAVTransportURI") == 0 || strcmp(action, "Next") == 0 ||
            strcmp(action, "Previous") == 0) {
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    ESP_LOGW(TAG, "unsupported AVTransport action: %s", action);
    soap_send_error(req, 401, "Invalid Action");
}

static void handle_rendering_control(httpd_req_t *req, const char *action, const char *body,
                                     const service_spec_t *svc)
{
    if (strcmp(action, "GetVolume") == 0) {
        soap_send(req, svc->type, action, body_volume, state_copy());
        return;
    }

    if (strcmp(action, "SetVolume") == 0) {
        char vol[8] = { 0 };
        body_tag(body, "DesiredVolume", vol, sizeof(vol));
        int v = atoi(vol);
        if (v < 0) {
            v = 0;
        }
        if (v > 100) {
            v = 100;
        }
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_state.volume = v;
        xSemaphoreGive(s_state_lock);
        ESP_LOGI(TAG, "SetVolume %d", v);
        emit_cmd(DLNA_CMD_SET_VOLUME, NULL, (uint32_t)v);
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    if (strcmp(action, "GetMute") == 0) {
        sb_t b = { s_xml, XML_CAP, 0 };
        sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                      "<u:GetMuteResponse xmlns:u=\"%s\"><CurrentMute>0</CurrentMute>"
                      "</u:GetMuteResponse></s:Body></s:Envelope>", svc->type);
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_send(req, s_xml, (ssize_t)b.len);
        return;
    }

    if (strcmp(action, "SetMute") == 0) {
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    if (strcmp(action, "ListPresets") == 0) {
        sb_t b = { s_xml, XML_CAP, 0 };
        sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                      "<u:ListPresetsResponse xmlns:u=\"%s\">"
                      "<CurrentPresetNameList>FactoryDefaults</CurrentPresetNameList>"
                      "</u:ListPresetsResponse></s:Body></s:Envelope>", svc->type);
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_send(req, s_xml, (ssize_t)b.len);
        return;
    }

    if (strcmp(action, "SelectPreset") == 0) {
        soap_send(req, svc->type, action, NULL, NULL);
        return;
    }

    ESP_LOGW(TAG, "unsupported RenderingControl action: %s", action);
    soap_send_error(req, 401, "Invalid Action");
}

static void handle_connection_manager(httpd_req_t *req, const char *action,
                                      const service_spec_t *svc)
{
    if (strcmp(action, "GetProtocolInfo") == 0) {
        /* 注意 QPlay 会把 FLAC 标成 audio/x-ogg，所以这里必须把两种都报告出去 */
        sb_t b = { s_xml, XML_CAP, 0 };
        sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                      "<u:GetProtocolInfoResponse xmlns:u=\"%s\"><Source></Source><Sink>", svc->type);
        sb_printf(&b, "http-get:*:audio/mpeg:*,http-get:*:audio/mp3:*,"
                      "http-get:*:audio/flac:*,http-get:*:audio/x-flac:*,"
                      "http-get:*:audio/x-ogg:*,http-get:*:audio/ogg:*,"
                      "http-get:*:audio/mp4:*,http-get:*:audio/aac:*,http-get:*:audio/aacp:*,"
                      "http-get:*:audio/wav:*,http-get:*:audio/x-wav:*,"
                      "http-get:*:application/octet-stream:*");
        sb_printf(&b, "</Sink></u:GetProtocolInfoResponse></s:Body></s:Envelope>");
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_send(req, s_xml, (ssize_t)b.len);
        return;
    }

    if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
        sb_t b = { s_xml, XML_CAP, 0 };
        sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                      "<u:GetCurrentConnectionIDsResponse xmlns:u=\"%s\">"
                      "<ConnectionIDs>0</ConnectionIDs>"
                      "</u:GetCurrentConnectionIDsResponse></s:Body></s:Envelope>", svc->type);
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_send(req, s_xml, (ssize_t)b.len);
        return;
    }

    if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
        sb_t b = { s_xml, XML_CAP, 0 };
        sb_printf(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                      "<s:Envelope s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                      "<u:GetCurrentConnectionInfoResponse xmlns:u=\"%s\">"
                      "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
                      "<ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager>"
                      "<PeerConnectionID>-1</PeerConnectionID><Direction>Input</Direction>"
                      "<Status>OK</Status>"
                      "</u:GetCurrentConnectionInfoResponse></s:Body></s:Envelope>", svc->type);
        httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
        httpd_resp_send(req, s_xml, (ssize_t)b.len);
        return;
    }

    ESP_LOGW(TAG, "unsupported ConnectionManager action: %s", action);
    soap_send_error(req, 401, "Invalid Action");
}

/* -------------------------------------------------------------------------- */
/*  HTTP handler                                                               */
/* -------------------------------------------------------------------------- */

/** @brief 记录谁在拉文档：手机/电视的 UA 里能直接看出是哪家控制点 */
static void log_get(const char *what, httpd_req_t *req)
{
    char ua[64] = { 0 };
    if (httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua)) != ESP_OK) {
        snprintf(ua, sizeof(ua), "-");
    }
    ESP_LOGI(TAG, "GET %s UA=%s", what, ua);
}

static esp_err_t desc_get(httpd_req_t *req)
{
    log_get("description.xml", req);
    xSemaphoreTake(s_build_lock, portMAX_DELAY);
    const int len = build_description();
    if (len >= XML_CAP - 1) {
        ESP_LOGE(TAG, "description truncated at %d bytes", len);
    }
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    const esp_err_t err = httpd_resp_send(req, s_xml, len);
    xSemaphoreGive(s_build_lock);
    return err;
}

static esp_err_t scpd_get(httpd_req_t *req)
{
    const char *slash = strrchr(req->uri, '/');
    if (slash == NULL) {
        return httpd_resp_send_404(req);
    }
    char name[24];
    snprintf(name, sizeof(name), "%s", slash + 1);
    char *dot = strchr(name, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    const service_spec_t *svc = service_by_name(name);
    if (svc == NULL) {
        return httpd_resp_send_404(req);
    }

    log_get(name, req);
    xSemaphoreTake(s_build_lock, portMAX_DELAY);
    const int len = build_scpd(svc);
    if (len >= XML_CAP - 1) {
        /* 半截 SCPD 是非法 XML，控制点会直接放弃这台设备，必须吼出来 */
        ESP_LOGE(TAG, "%s scpd truncated at %d bytes", name, len);
    }
    httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
    const esp_err_t err = httpd_resp_send(req, s_xml, len);
    xSemaphoreGive(s_build_lock);
    return err;
}

static esp_err_t control_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= SOAP_IN_CAP) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }

    size_t total = 0;
    while (total < (size_t)req->content_len) {
        const int r = httpd_req_recv(req, s_soap_in + total, (size_t)req->content_len - total);
        if (r <= 0) {
            return ESP_FAIL;
        }
        total += (size_t)r;
    }
    s_soap_in[total] = '\0';

    char hdr[128] = { 0 };
    if (httpd_req_get_hdr_value_str(req, "SOAPACTION", hdr, sizeof(hdr)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no SOAPACTION");
    }
    char *hash = strrchr(hdr, '#');
    char action[40];
    snprintf(action, sizeof(action), "%s", (hash != NULL) ? hash + 1 : hdr);
    size_t alen = strlen(action);
    while (alen > 0 && (action[alen - 1] == '"' || action[alen - 1] == '\n' ||
                        action[alen - 1] == '\r')) {
        action[--alen] = '\0';
    }

    const char *svc_name = strrchr(req->uri, '/');
    if (svc_name == NULL) {
        return httpd_resp_send_404(req);
    }
    const service_spec_t *svc = service_by_name(svc_name + 1);
    if (svc == NULL) {
        return httpd_resp_send_404(req);
    }

    ESP_LOGI(TAG, "%s#%s", svc->name, action);

    xSemaphoreTake(s_build_lock, portMAX_DELAY);
    if (strcmp(svc->name, "AVTransport") == 0) {
        handle_avtransport(req, action, s_soap_in, svc);
    } else if (strcmp(svc->name, "RenderingControl") == 0) {
        handle_rendering_control(req, action, s_soap_in, svc);
    } else {
        handle_connection_manager(req, action, svc);
    }
    xSemaphoreGive(s_build_lock);
    return ESP_OK;
}

/** @brief 不处理的请求体也要读掉，否则同一条 keep-alive 连接上后续请求会解析错位 */
static void drain_body(httpd_req_t *req)
{
    char tmp[64];
    size_t left = req->content_len;
    while (left > 0) {
        const int r = httpd_req_recv(req, tmp, left < sizeof(tmp) ? left : sizeof(tmp));
        if (r <= 0) {
            break;
        }
        left -= (size_t)r;
    }
}

/**
 * @brief GENA：QPlay 会 SUBSCRIBE（实测 6 次）但不依赖事件内容，返回 SID 即可。
 *
 * SUBSCRIBE 与 UNSUBSCRIBE 都是"未知方法"，httpd 给的 method 都是同一个值，
 * 所以按头区分：带 NT/CALLBACK/TIMEOUT 的是订阅（含续订），只剩 SID 的是退订。
 */
static esp_err_t event_any(httpd_req_t *req)
{
    static uint32_t s_sid_seq = 0;
    char hdr[128] = { 0 };
    char sid[64] = { 0 };   /* uuid: + 36 字节 UDN + "-" + 8 位序号 */

    const bool has_nt = httpd_req_get_hdr_value_str(req, "NT", hdr, sizeof(hdr)) == ESP_OK;
    const bool has_timeout = httpd_req_get_hdr_value_str(req, "TIMEOUT", hdr, sizeof(hdr)) == ESP_OK;
    const bool has_sid = httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid)) == ESP_OK;

    if (has_nt || has_timeout) {
        /* 续订要沿用原 SID，初次订阅才发新 SID */
        if (!has_sid) {
            snprintf(sid, sizeof(sid), "uuid:%s-%08lx", s_uuid + 5, (unsigned long)(++s_sid_seq));
        }
        httpd_resp_set_hdr(req, "SID", sid);
        httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
        ESP_LOGI(TAG, "gena subscribe %s -> %s", req->uri, sid);
    } else {
        /* 退订：UPnP 不要求回 SID，回 200 即可 */
        ESP_LOGI(TAG, "gena unsubscribe %s (sid=%s)", req->uri, sid);
    }

    drain_body(req);
    return httpd_resp_send(req, NULL, 0);
}

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */

static void make_uuid(char *out, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* 8-4-4-4-12，用 MAC 填前两段，后三段固定，避免手机端把不同设备认成同一台 */
    snprintf(out, len,
             "uuid:%02x%02x%02x%02x-%02x%02x-4a3b-8c11-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/** @brief 启动失败时回收已申请的资源，避免"一次失败后永远起不来" */
static void release_resources(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_xml != NULL) {
        heap_caps_free(s_xml);
        s_xml = NULL;
    }
    if (s_soap_in != NULL) {
        heap_caps_free(s_soap_in);
        s_soap_in = NULL;
    }
    if (s_state_lock != NULL) {
        vSemaphoreDelete(s_state_lock);
        s_state_lock = NULL;
    }
    if (s_build_lock != NULL) {
        vSemaphoreDelete(s_build_lock);
        s_build_lock = NULL;
    }
}

esp_err_t dlna_renderer_start(const dlna_renderer_cfg_t *cfg)
{
    if (cfg == NULL || cfg->on_cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_httpd != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state_lock = xSemaphoreCreateMutex();
    s_build_lock = xSemaphoreCreateMutex();
    if (s_state_lock == NULL || s_build_lock == NULL) {
        release_resources();
        return ESP_ERR_NO_MEM;
    }

    s_xml = heap_caps_malloc(XML_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_soap_in = heap_caps_malloc(SOAP_IN_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_xml == NULL || s_soap_in == NULL) {
        ESP_LOGW(TAG, "PSRAM unavailable for XML buffers, falling back to internal");
        if (s_xml == NULL) {
            s_xml = heap_caps_malloc(XML_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_soap_in == NULL) {
            s_soap_in = heap_caps_malloc(SOAP_IN_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_xml == NULL || s_soap_in == NULL) {
            ESP_LOGE(TAG, "no memory for XML buffers");
            release_resources();
            return ESP_ERR_NO_MEM;
        }
    }

    s_on_cmd = cfg->on_cmd;
    s_user = cfg->user;
    snprintf(s_friendly, sizeof(s_friendly), "%s", cfg->friendly_name != NULL ? cfg->friendly_name : "ESP32-DLNA");
    snprintf(s_model, sizeof(s_model), "%s", cfg->model_name != NULL ? cfg->model_name : "ESP32-S3 DLNA Renderer");
    make_uuid(s_uuid, sizeof(s_uuid));

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = { 0 };
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        ESP_LOGE(TAG, "no station IP, connect WiFi before starting renderer");
        release_resources();
        return ESP_ERR_INVALID_STATE;
    }

    s_state.transport = DLNA_TRANSPORT_NO_MEDIA;
    s_state.volume = 50;

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = cfg->port != 0 ? cfg->port : 8090;
    http_cfg.stack_size = 6144;
    http_cfg.max_open_sockets = 4;
    http_cfg.max_uri_handlers = 12;     /* 默认 8，这里注册了 10 个，不够会静默注册失败 */
    http_cfg.lru_purge_enable = true;
    http_cfg.recv_wait_timeout = 5;
    http_cfg.send_wait_timeout = 5;
    if (httpd_start(&s_httpd, &http_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        release_resources();
        return ESP_FAIL;
    }
    s_port = http_cfg.server_port;

    static const httpd_uri_t routes[] = {
        { .uri = "/description.xml", .method = HTTP_GET, .handler = desc_get },
        { .uri = "/scpd/AVTransport.xml", .method = HTTP_GET, .handler = scpd_get },
        { .uri = "/scpd/RenderingControl.xml", .method = HTTP_GET, .handler = scpd_get },
        { .uri = "/scpd/ConnectionManager.xml", .method = HTTP_GET, .handler = scpd_get },
        { .uri = "/control/AVTransport", .method = HTTP_POST, .handler = control_post },
        { .uri = "/control/RenderingControl", .method = HTTP_POST, .handler = control_post },
        { .uri = "/control/ConnectionManager", .method = HTTP_POST, .handler = control_post },
        { .uri = "/event/AVTransport", .method = HTTP_ANY, .handler = event_any },
        { .uri = "/event/RenderingControl", .method = HTTP_ANY, .handler = event_any },
        { .uri = "/event/ConnectionManager", .method = HTTP_ANY, .handler = event_any },
    };
    for (size_t i = 0; i < COUNT_OF(routes); i++) {
        if (httpd_register_uri_handler(s_httpd, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed", routes[i].uri);
            release_resources();
            return ESP_FAIL;
        }
    }

    /* ip.ip 是 esp_ip4_addr_t（成员 addr），可以直接喂 IPSTR/IP2STR */
    snprintf(s_base_url, sizeof(s_base_url), "http://" IPSTR ":%u/description.xml",
             IP2STR(&ip.ip), (unsigned)s_port);

    esp_err_t err = dlna_ssdp_start(ip.ip.addr, s_port, s_uuid, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ssdp start failed: %s", esp_err_to_name(err));
        release_resources();
        return err;
    }

    ESP_LOGI(TAG, "renderer ready: %s  (friendly=%s uuid=%s)",
             s_base_url, s_friendly, s_uuid);
    return ESP_OK;
}

void dlna_renderer_set_transport(dlna_transport_t state, uint32_t position_ms, uint32_t duration_ms)
{
    if (s_state_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state.transport = state;
    s_state.position_ms = position_ms;
    if (duration_ms > 0) {
        s_state.duration_ms = duration_ms;
    }
    xSemaphoreGive(s_state_lock);
}

void dlna_renderer_get_media(dlna_media_info_t *out)
{
    if (out == NULL || s_state_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    *out = s_state.media;
    out->has_media = s_state.has_media;
    xSemaphoreGive(s_state_lock);
}

const char *dlna_renderer_uuid(void)
{
    return s_uuid;
}

uint16_t dlna_renderer_port(void)
{
    return s_port;
}
