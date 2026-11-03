idf_component_register(SRCS "main.c" "usb_net.c" "wifi_mgr.c" "web_server.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_event nvs_flash esp_netif lwip esp_wifi esp_http_server esp_timer)
