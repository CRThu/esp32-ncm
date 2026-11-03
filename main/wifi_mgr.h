#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool is_connected;
    bool is_connecting;
    bool auto_reconnect;
    char ssid[33];
    char ip[16];
    int8_t rssi;
} wifi_mgr_status_t;

typedef void (*wifi_state_cb_t)(bool connected);

/**
 * @brief 初始化 Wi-Fi STA 模块并注册事件处理器
 */
esp_err_t wifi_mgr_init(wifi_state_cb_t cb);

/**
 * @brief 扫描周围可用 Wi-Fi 并返回 JSON 格式结果
 */
esp_err_t wifi_mgr_scan(char *json_out, size_t max_len);

/**
 * @brief 连接指定的 Wi-Fi 热点并保存至 NVS
 */
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);

/**
 * @brief 断开当前 Wi-Fi 连接
 */
esp_err_t wifi_mgr_disconnect(void);

/**
 * @brief 获取当前 Wi-Fi 连接状态与详情
 */
void wifi_mgr_get_status(wifi_mgr_status_t *status);

/**
 * @brief 获取 Wi-Fi STA netif 句柄
 */
esp_netif_t *wifi_mgr_get_sta_netif(void);

#ifdef __cplusplus
}
#endif
