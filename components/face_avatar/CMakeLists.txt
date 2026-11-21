# 设备统一的拟人形象（表情包），素材在 storage 分区，由 selector 与所有 app 共用
idf_component_register(
    SRCS "face_avatar.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES spiffs esp_partition heap
)
