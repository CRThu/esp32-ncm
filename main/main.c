#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_net.h"
#include "wifi_mgr.h"
#include "web_server.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_reset_reason_t rst_reason = esp_reset_reason();
    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "  ESP32-S3 BOOTED | Reset Reason: %d (%s)", rst_reason,
             (rst_reason == ESP_RST_POWERON) ? "POWER_ON" :
             (rst_reason == ESP_RST_SW) ? "SOFTWARE" :
             (rst_reason == ESP_RST_PANIC) ? "PANIC" :
             (rst_reason == ESP_RST_INT_WDT) ? "INT_WDT" :
             (rst_reason == ESP_RST_TASK_WDT) ? "TASK_WDT" :
             (rst_reason == ESP_RST_BROWNOUT) ? "BROWNOUT" : "OTHER");
    ESP_LOGW(TAG, "==================================================");

    // 1. 初始化 NVS 存储
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化底层网络协议栈与事件循环
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. 初始化 USB 虚拟以太网卡 (192.168.4.1) 并开启 DHCP Server
    ESP_LOGI(TAG, "1/3 Initializing USB Network Device (CDC-NCM)...");
    ESP_ERROR_CHECK(usb_net_init());

    // 4. 初始化 Wi-Fi 管理器 (STA 模式，纯 RAM 配置，非阻塞)
    ESP_LOGI(TAG, "2/3 Initializing Wi-Fi Manager...");
    ESP_ERROR_CHECK(wifi_mgr_init(NULL));

    // 5. 启动 Web 配置控制台 HTTP Server
    ESP_LOGI(TAG, "3/3 Starting HTTP Web Server...");
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "All subsystems running! Web Console: http://192.168.4.1/");

    int count = 0;
    while (1) {
        wifi_mgr_status_t st;
        wifi_mgr_get_status(&st);
        ESP_LOGI(TAG, "[Health %d] Heap: %lu | Wi-Fi: %s (IP: %s, RSSI: %d)",
                 count++, (unsigned long)esp_get_free_heap_size(),
                 st.is_connected ? "CONNECTED" : (st.is_connecting ? "CONNECTING" : "IDLE"),
                 st.is_connected ? st.ip : "N/A",
                 st.rssi);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
