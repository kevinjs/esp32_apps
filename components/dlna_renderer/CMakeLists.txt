idf_component_register(
    SRCS "dlna_renderer.c" "dlna_ssdp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_http_server esp_netif
    PRIV_REQUIRES lwip esp_timer esp_hw_support heap
)
