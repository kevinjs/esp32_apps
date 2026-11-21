/* 组件内部接口：SSDP 发现（不对外） */
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t dlna_ssdp_start(uint32_t ip_be, uint16_t http_port, const char *uuid_str,
                          uint32_t boot_id);
void dlna_ssdp_stop(void);
