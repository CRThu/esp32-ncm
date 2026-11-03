#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "usb_net.h"
#include "wifi_mgr.h"

static const char *TAG = "wifi_mgr";

static esp_netif_t *s_sta_netif = NULL;
static wifi_mgr_status_t s_status = {
    .is_connected = false,
    .is_connecting = false,
    .auto_reconnect = true,
    .ssid = "",
    .ip = "",
    .rssi = 0,
};
static wifi_state_cb_t s_state_cb = NULL;

static char s_scan_cache[2048] = "[]";
static SemaphoreHandle_t s_scan_lock = NULL;
static SemaphoreHandle_t s_status_lock = NULL;
static TaskHandle_t s_scan_task = NULL;
static volatile bool s_is_scanning = false;
static int s_retry_count = 0;
#define MAX_RETRY_COUNT 3

static esp_timer_handle_t s_reconnect_timer = NULL;
static bool s_auto_reconnect = true;
static char s_target_ssid[33] = {0};

static void reconnect_timer_cb(void *arg)
{
    if (!s_auto_reconnect) {
        return;
    }
    if (s_status.is_connected || s_status.is_connecting || s_is_scanning) {
        return;
    }
    if (s_target_ssid[0] == '\0') {
        return;
    }
    ESP_LOGI(TAG, "15s auto-reconnect timer fired: probing target AP '%s'...", s_target_ssid);
    s_status.is_connecting = true;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_status.is_connecting = false;
        ESP_LOGW(TAG, "esp_wifi_connect in reconnect timer returned: %s", esp_err_to_name(err));
    }
}

static void start_reconnect_timer(void)
{
    if (!s_auto_reconnect || !s_reconnect_timer) return;
    esp_timer_stop(s_reconnect_timer);
    esp_err_t err = esp_timer_start_periodic(s_reconnect_timer, 15000000); // 15s 周期
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Auto-reconnect periodic timer started (15-second interval)");
    }
}

static void stop_reconnect_timer(void)
{
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
}

static int compare_ap_rssi(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *ap_b = (const wifi_ap_record_t *)b;
    return ap_b->rssi - ap_a->rssi;
}

static void wifi_scan_task_entry(void *pv)
{
    while (1) {
        // 仅在被显式通知（用户点击/网页请求）时执行扫描，严禁后台无节制突发扫描以避免瞬态功耗冲击
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // 关键时序防御：让出 300ms 确保触发此扫描的 HTTP 响应报文及 TCP ACK 已经在网络层完整收发完毕
        vTaskDelay(pdMS_TO_TICKS(300));

        if (s_status.is_connecting) {
            ESP_LOGW(TAG, "Wi-Fi is currently connecting, skipping scan trigger");
            continue;
        }
        ESP_LOGI(TAG, "Background scan worker starting Wi-Fi channel scan...");
        s_is_scanning = true;

        wifi_scan_config_t scan_cfg = {
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time = {
                .active = { .min = 30, .max = 60 }, // 缩短单信道停留，大幅削减射频发射时间与功耗
            },
        };
        esp_err_t err = esp_wifi_scan_start(&scan_cfg, true); // 阻塞式扫描，独立任务上下文
        s_is_scanning = false;

        if (err == ESP_OK) {
            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);
            ESP_LOGI(TAG, "Scan completed! Found %u APs", ap_count);

            if (ap_count > 0) {
                if (ap_count > 20) ap_count = 20;
                wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
                if (ap_records) {
                    if (esp_wifi_scan_get_ap_records(&ap_count, ap_records) == ESP_OK) {
                        qsort(ap_records, ap_count, sizeof(wifi_ap_record_t), compare_ap_rssi);

                        char *temp_json = malloc(2048);
                        if (temp_json) {
                            int written = snprintf(temp_json, 2048, "[");
                            int valid = 0;

                            for (int i = 0; i < ap_count && written < 2048 - 100; i++) {
                                if (ap_records[i].ssid[0] == '\0') continue;

                                bool dup = false;
                                for (int j = 0; j < i; j++) {
                                    if (strcmp((char *)ap_records[i].ssid, (char *)ap_records[j].ssid) == 0) {
                                        dup = true;
                                        break;
                                    }
                                }
                                if (dup) continue;

                                char safe_ssid[65] = {0};
                                int s_idx = 0;
                                for (int k = 0; ap_records[i].ssid[k] != '\0' && s_idx < 60; k++) {
                                    char c = ap_records[i].ssid[k];
                                    if ((unsigned char)c < 32) continue; // 过滤控制字符避免 JSON 解析异常
                                    if (c == '"' || c == '\\') safe_ssid[s_idx++] = '\\';
                                    safe_ssid[s_idx++] = c;
                                }

                                if (valid > 0) written += snprintf(temp_json + written, 2048 - written, ",");
                                written += snprintf(temp_json + written, 2048 - written,
                                                    "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                                                    safe_ssid, ap_records[i].rssi, ap_records[i].authmode);
                                valid++;
                            }

                            snprintf(temp_json + written, 2048 - written, "]");

                            if (s_scan_lock && xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                                snprintf(s_scan_cache, sizeof(s_scan_cache), "%s", temp_json);
                                xSemaphoreGive(s_scan_lock);
                            }
                            ESP_LOGI(TAG, "Cached %d unique APs into s_scan_cache", valid);
                            free(temp_json);

                            // 手动扫描唤醒机制：若处于掉线重连中，且在扫描结果中发现了目标热点，立即抢先就地触发重连！
                            if (s_auto_reconnect && !s_status.is_connected && !s_status.is_connecting && s_target_ssid[0] != '\0') {
                                bool found = false;
                                for (int k = 0; k < ap_count; k++) {
                                    if (strcmp((char *)ap_records[k].ssid, s_target_ssid) == 0) {
                                        found = true;
                                        break;
                                    }
                                }
                                if (found) {
                                    ESP_LOGI(TAG, "Target AP '%s' detected during scan! Triggering immediate reconnect!", s_target_ssid);
                                    s_status.is_connecting = true;
                                    esp_wifi_connect();
                                }
                            }
                        }
                    }
                    free(ap_records);
                } else {
                    esp_wifi_clear_ap_list();
                }
            } else {
                esp_wifi_clear_ap_list();
                if (s_scan_lock && xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                    snprintf(s_scan_cache, sizeof(s_scan_cache), "[]");
                    xSemaphoreGive(s_scan_lock);
                }
            }
        } else {
            ESP_LOGW(TAG, "esp_wifi_scan_start returned err: %s", esp_err_to_name(err));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            ESP_LOGI(TAG, "Wi-Fi STA started, checking saved credentials...");
            wifi_config_t saved_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &saved_cfg) == ESP_OK && saved_cfg.sta.ssid[0] != '\0') {
                ESP_LOGI(TAG, "Auto-connecting to saved AP: '%s'", saved_cfg.sta.ssid);
                s_status.is_connecting = true;
                snprintf(s_status.ssid, sizeof(s_status.ssid), "%.32s", (char *)saved_cfg.sta.ssid);
                snprintf(s_target_ssid, sizeof(s_target_ssid), "%.32s", (char *)saved_cfg.sta.ssid);
                s_auto_reconnect = true;
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "No saved Wi-Fi found, idle waiting for user connection.");
            }
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "WIFI_EVENT_SCAN_DONE event notification received");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Wi-Fi STA connected to AP, waiting for IP...");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Wi-Fi STA disconnected, reason: %d", disc->reason);
            s_status.is_connected = false;
            s_status.is_connecting = false;
            s_status.ip[0] = '\0';
            s_status.rssi = 0;
            esp_netif_t *usb_netif = usb_net_get_netif();
            if (usb_netif) {
                esp_netif_napt_disable(usb_netif);
            }
            if (s_state_cb) {
                s_state_cb(false);
            }
            // 若启用了自动重连且有已知目标热点，启动/维持 15 秒定时探测
            if (s_auto_reconnect && s_target_ssid[0] != '\0') {
                start_reconnect_timer();
            }
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_status.ip, sizeof(s_status.ip));
        s_status.is_connected = true;
        s_status.is_connecting = false;
        s_retry_count = 0;
        stop_reconnect_timer();

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_status.rssi = ap_info.rssi;
            snprintf(s_status.ssid, sizeof(s_status.ssid), "%.32s", (char *)ap_info.ssid);
        }

        ESP_LOGI(TAG, "Wi-Fi STA got IP: %s (SSID: %s, RSSI: %d dBm)",
                 s_status.ip, s_status.ssid, s_status.rssi);

        // 设置 STA 为默认网关接口
        esp_netif_set_default_netif(s_sta_netif);

        // 启用 NAPT：在下行 LAN 接口 (USB 网卡) 上开启 NAPT 网络地址与端口转换
        esp_netif_t *usb_netif = usb_net_get_netif();
        if (usb_netif) {
            esp_err_t err = esp_netif_napt_enable(usb_netif);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "NAPT routing enabled successfully on USB netif!");
            } else {
                ESP_LOGW(TAG, "Failed to enable NAPT on USB netif: %s", esp_err_to_name(err));
            }
        }

        if (s_state_cb) {
            s_state_cb(true);
        }
    }
}

esp_err_t wifi_mgr_init(wifi_state_cb_t cb)
{
    s_state_cb = cb;
    s_scan_lock = xSemaphoreCreateMutex();
    s_status_lock = xSemaphoreCreateMutex();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 发射功率严格限制在 10dBm(40)，削减 60% 射频瞬态电流，根治 USB 供电跌落与过流不稳定
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(40));

    // 保持 WIFI_PS_NONE（禁用休眠）：射频常开，消除 100ms 休眠等待，保障 5Mbps 高速吞吐
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // 启动后台独立扫描任务，栈深 4096 字节
    BaseType_t r = xTaskCreate(wifi_scan_task_entry, "wifi_scan", 4096, NULL, 3, &s_scan_task);
    if (r != pdPASS || !s_scan_task) {
        ESP_LOGE(TAG, "Failed to create wifi_scan task");
    }

    // 创建后台 15 秒低频定时重连定时器
    const esp_timer_create_args_t timer_args = {
        .callback = &reconnect_timer_cb,
        .name = "reconn_tmr",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    ESP_LOGI(TAG, "Wi-Fi subsystem initialized in RAM mode, ready for commands.");
    return ESP_OK;
}

esp_err_t wifi_mgr_scan(char *json_out, size_t max_len)
{
    if (!s_sta_netif) {
        if (json_out && max_len > 2) {
            snprintf(json_out, max_len, "[]");
        }
        return ESP_ERR_INVALID_STATE;
    }

    // 若当前未在扫描，通知后台任务触发扫描（任务内部会避让 300ms）
    if (!s_is_scanning && s_scan_task) {
        xTaskNotifyGive(s_scan_task);
    }

    // 快速读取并返回已有缓存
    if (s_scan_lock && xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(json_out, max_len, "%s", s_scan_cache);
        xSemaphoreGive(s_scan_lock);
    } else {
        snprintf(json_out, max_len, "[]");
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to AP '%s'...", ssid);
    s_auto_reconnect = true;
    snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", ssid);
    stop_reconnect_timer();

    s_status.is_connecting = true;
    s_status.is_connected = false;
    s_retry_count = 0;
    snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", ssid);

    esp_wifi_disconnect();

    wifi_config_t wifi_cfg = { 0 };
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    if (password && strlen(password) > 0) {
        snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", password);
    }
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // 启用 PMF 兼容器件能力，提升手机热点与现代 WPA2/WPA3 路由兼容性
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    return esp_wifi_connect();
}

esp_err_t wifi_mgr_disconnect(void)
{
    if (!s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Explicitly disconnecting Wi-Fi...");
    s_auto_reconnect = false;
    stop_reconnect_timer();
    s_status.is_connected = false;
    s_status.is_connecting = false;
    s_retry_count = MAX_RETRY_COUNT; // 禁止自动重连
    return esp_wifi_disconnect();
}

void wifi_mgr_get_status(wifi_mgr_status_t *status)
{
    if (status) {
        if (s_status_lock) xSemaphoreTake(s_status_lock, portMAX_DELAY);
        if (s_status.is_connected) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                s_status.rssi = ap_info.rssi;
            }
        }
        s_status.auto_reconnect = s_auto_reconnect;
        *status = s_status;
        if (s_status_lock) xSemaphoreGive(s_status_lock);
    }
}

esp_netif_t *wifi_mgr_get_sta_netif(void)
{
    return s_sta_netif;
}
