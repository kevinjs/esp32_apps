/*
 * SSDP 发现：让手机的 UPnP 控制点（这里是 QQ 音乐 QPlay）能找到本机。
 *
 * 只做 DMR 需要的最小集合：
 *   - 加入 239.255.255.250:1900 组播，应答 M-SEARCH（unicast 回给控制点）
 *   - 每 60 s 发一次 ssdp:alive（手机 App 重启后靠它重新发现自己）
 *
 * 注意：手机热点/部分路由器会隔离组播，这时 M-SEARCH 收不到，表现为
 * "QQ 音乐里看不到设备"。家里正常路由器没这个问题。
 */
#include "dlna_ssdp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define SSDP_ADDR       "239.255.255.250"
#define SSDP_PORT       1900
#define NOTIFY_PERIOD_US (60LL * 1000 * 1000)

static const char *TAG = "dlna.ssdp";

static int s_sock = -1;
static volatile bool s_run = false;
static char s_location[96];
static char s_uuid[48];
static char s_server[72] = "ESP32S3/1.0 UPnP/1.0 DLNADOC/1.50";
static uint32_t s_boot_id = 1;
static uint32_t s_ip_be = 0;

/* -------------------------------------------------------------------------- */

static bool ci_prefix(const char *text, const char *prefix)
{
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/** @brief 在请求头里找某个字段的值（大小写不敏感，跳过前导空格） */
static const char *header_value(const char *req, const char *name)
{
    const char *line = req;
    while (line != NULL && *line != '\0') {
        const char *eol = strstr(line, "\r\n");
        const size_t line_len = (eol != NULL) ? (size_t)(eol - line) : strlen(line);
        if (line_len > strlen(name) && ci_prefix(line, name) && line[strlen(name)] == ':') {
            const char *value = line + strlen(name) + 1;
            while (*value == ' ') {
                value++;
            }
            return value;   /* 调用方按行尾截断，这里不复制 */
        }
        line = (eol != NULL) ? eol + 2 : NULL;
    }
    return NULL;
}

/* 这里只用 lwip 的 struct in_addr（成员是 s_addr），不能用 esp_netif 的 IPSTR/IP2STR，
   否则得额外依赖 esp_netif；顺手也省掉一次头文件包含。 */
static const char *ip4_to_str(uint32_t addr_be, char *buf, size_t buf_len)
{
    const uint8_t *b = (const uint8_t *)&addr_be;
    snprintf(buf, buf_len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

static void copy_line_value(const char *value, char *out, size_t out_len)
{
    size_t i = 0;
    while (value[i] != '\0' && value[i] != '\r' && value[i] != '\n' && i + 1 < out_len) {
        out[i] = value[i];
        i++;
    }
    out[i] = '\0';
}

static void ssdp_notify(const char *nt, const char *usn)
{
    char pkt[512];
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SSDP_PORT);
    dst.sin_addr.s_addr = inet_addr(SSDP_ADDR);

    const int n = snprintf(pkt, sizeof(pkt),
                           "NOTIFY * HTTP/1.1\r\n"
                           "HOST: %s:%d\r\n"
                           "CACHE-CONTROL: max-age=1800\r\n"
                           "LOCATION: %s\r\n"
                           "SERVER: %s\r\n"
                           "NT: %s\r\n"
                           "NTS: ssdp:alive\r\n"
                           "USN: %s\r\n"
                           "BOOTID.UPNP.ORG: %lu\r\n"
                           "CONFIGID.UPNP.ORG: 1337\r\n\r\n",
                           SSDP_ADDR, SSDP_PORT, s_location, s_server, nt, usn,
                           (unsigned long)s_boot_id);
    if (n > 0) {
        sendto(s_sock, pkt, (size_t)n, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
}

static void notify_all(void)
{
    char usn[96];
    snprintf(usn, sizeof(usn), "%s::upnp:rootdevice", s_uuid);
    ssdp_notify("upnp:rootdevice", usn);
    ssdp_notify(s_uuid, s_uuid);
    snprintf(usn, sizeof(usn), "%s::urn:schemas-upnp-org:device:MediaRenderer:1", s_uuid);
    ssdp_notify("urn:schemas-upnp-org:device:MediaRenderer:1", usn);
    snprintf(usn, sizeof(usn), "%s::urn:schemas-upnp-org:service:AVTransport:1", s_uuid);
    ssdp_notify("urn:schemas-upnp-org:service:AVTransport:1", usn);
    ESP_LOGD(TAG, "alive sent");
}

static void ssdp_reply(const char *st, const struct sockaddr_in *dst)
{
    char usn[96];
    if (strncmp(st, s_uuid, strlen(s_uuid)) == 0) {
        snprintf(usn, sizeof(usn), "%s", s_uuid);
    } else {
        snprintf(usn, sizeof(usn), "%s::%s", s_uuid, st);
    }

    char date[40];
    /* 控制点不校验 DATE，省一次 time() 调用 */
    snprintf(date, sizeof(date), "Thu, 01 Jan 2026 00:00:00 GMT");

    char pkt[640];
    const int n = snprintf(pkt, sizeof(pkt),
                           "HTTP/1.1 200 OK\r\n"
                           "CACHE-CONTROL: max-age=1800\r\n"
                           "DATE: %s\r\n"
                           "EXT:\r\n"
                           "LOCATION: %s\r\n"
                           "SERVER: %s\r\n"
                           "ST: %s\r\n"
                           "USN: %s\r\n"
                           "BOOTID.UPNP.ORG: %lu\r\n"
                           "CONFIGID.UPNP.ORG: 1337\r\n\r\n",
                           date, s_location, s_server, st, usn, (unsigned long)s_boot_id);
    if (n > 0) {
        sendto(s_sock, pkt, (size_t)n, 0, (const struct sockaddr *)dst, sizeof(*dst));
    }
}

/** @brief ST 命中就回包；ssdp:all 需要按 4 种目标各回一次 */
static void handle_msearch(const char *req, const struct sockaddr_in *src)
{
    const char *st_value = header_value(req, "ST");
    if (st_value == NULL) {
        return;
    }
    char st[96];
    char ip[16];
    copy_line_value(st_value, st, sizeof(st));
    ESP_LOGI(TAG, "M-SEARCH from %s ST=%s",
             ip4_to_str(src->sin_addr.s_addr, ip, sizeof(ip)), st);

    if (strcmp(st, "ssdp:all") == 0) {
        ssdp_reply("upnp:rootdevice", src);
        ssdp_reply(s_uuid, src);
        ssdp_reply("urn:schemas-upnp-org:device:MediaRenderer:1", src);
        ssdp_reply("urn:schemas-upnp-org:service:AVTransport:1", src);
        return;
    }

    if (strcmp(st, "upnp:rootdevice") == 0 ||
            strcmp(st, "urn:schemas-upnp-org:device:MediaRenderer:1") == 0 ||
            strcmp(st, "urn:schemas-upnp-org:service:AVTransport:1") == 0 ||
            strcmp(st, "urn:schemas-upnp-org:service:RenderingControl:1") == 0 ||
            strcmp(st, "urn:schemas-upnp-org:service:ConnectionManager:1") == 0 ||
            strcmp(st, s_uuid) == 0) {
        ssdp_reply(st, src);
        return;
    }
    ESP_LOGD(TAG, "ignore ST=%s", st);
}

/* -------------------------------------------------------------------------- */

static void ssdp_task(void *arg)
{
    (void)arg;
    char buf[1024];
    int64_t next_notify = 0;

    notify_all();
    next_notify = esp_timer_get_time() + NOTIFY_PERIOD_US;

    while (s_run) {
        if (esp_timer_get_time() >= next_notify) {
            notify_all();
            next_notify = esp_timer_get_time() + NOTIFY_PERIOD_US;
        }

        struct sockaddr_in src = { 0 };
        socklen_t slen = sizeof(src);
        const int len = recvfrom(s_sock, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr *)&src, &slen);
        if (len <= 0) {
            continue;   /* SO_RCVTIMEO 到点，回到循环顶部检查 alive 与退出标志 */
        }
        buf[len] = '\0';
        if (strncmp(buf, "M-SEARCH", 8) == 0) {
            handle_msearch(buf, &src);
        }
    }

    ESP_LOGI(TAG, "task exit");
    vTaskDelete(NULL);
}

esp_err_t dlna_ssdp_start(uint32_t ip_be, uint16_t http_port, const char *uuid_str,
                          uint32_t boot_id)
{
    if (s_run) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ip_be = ip_be;
    s_boot_id = boot_id;
    snprintf(s_uuid, sizeof(s_uuid), "%s", uuid_str);
    snprintf(s_server, sizeof(s_server), "ESP32S3/1.0 UPnP/1.0 DLNADOC/1.50");

    char ip[16];
    snprintf(s_location, sizeof(s_location), "http://%s:%u/description.xml",
             ip4_to_str(ip_be, ip, sizeof(ip)), (unsigned)http_port);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = { 0 };
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(SSDP_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "bind %d failed: errno=%d", SSDP_PORT, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    struct ip_mreq mreq = { 0 };
    mreq.imr_multiaddr.s_addr = inet_addr(SSDP_ADDR);
    mreq.imr_interface.s_addr = ip_be;
    if (setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        ESP_LOGW(TAG, "join multicast failed: errno=%d (继续尝试，可能收不到 M-SEARCH)", errno);
    } else {
        ESP_LOGI(TAG, "joined %s on %s", SSDP_ADDR, ip4_to_str(ip_be, ip, sizeof(ip)));
    }

    uint8_t ttl = 2;
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct in_addr iface = { .s_addr = ip_be };
    setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));

    s_run = true;
    if (xTaskCreate(ssdp_task, "dlna.ssdp", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_run = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started: %s", s_location);
    return ESP_OK;
}

void dlna_ssdp_stop(void)
{
    s_run = false;
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}
