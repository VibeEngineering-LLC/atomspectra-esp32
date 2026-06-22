idf_component_register(
    SRCS "main.c" "usb_host_cdc.c" "wifi_manager.c" "web_server.c" "spectrum.c"
    INCLUDE_DIRS "."
    REQUIRES usb_host_cdc_acm esp_wifi esp_http_server nvs_flash esp_event littlefs shproto json
    EMBED_FILES "${PROJECT_DIR}/web/index.html"
)
