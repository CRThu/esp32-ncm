#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip4_addr.h"
#include "usb_net.h"

static const char *TAG = "usb_net";
static esp_netif_t *s_usb_netif = NULL;
static SemaphoreHandle_t s_tx_lock = NULL;

static uint32_t s_tx_pkts = 0;
static uint32_t s_tx_retries = 0;
static uint32_t s_tx_fails = 0;

void usb_net_get_stats(uint32_t *tx_pkts, uint32_t *tx_retries, uint32_t *tx_fails)
{
    if (tx_pkts) *tx_pkts = s_tx_pkts;
    if (tx_retries) *tx_retries = s_tx_retries;
    if (tx_fails) *tx_fails = s_tx_fails;
}

static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    s_tx_pkts++;
    esp_err_t ret = ESP_FAIL;
    for (int retry = 0; retry < 25; retry++) {
        if (s_tx_lock) {
            xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        }
        ret = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(20));
        if (s_tx_lock) {
            xSemaphoreGive(s_tx_lock);
        }
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        s_tx_retries++;
        vTaskDelay(pdMS_TO_TICKS(1)); // 避让 1ms，等待 TinyUSB 任务完成上一个 NTB 发送
    }
    s_tx_fails++;
    return ret;
}

static void l2_free(void *h, void *buffer)
{
    free(buffer);
}

static esp_err_t netif_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (s_usb_netif) {
        void *buf_copy = malloc(len);
        if (!buf_copy) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(buf_copy, buffer, len);
        return esp_netif_receive(s_usb_netif, buf_copy, len, NULL);
    }
    return ESP_OK;
}

esp_err_t usb_net_init(void)
{
    if (!s_tx_lock) {
        s_tx_lock = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "Initializing TinyUSB driver...");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.task.priority = 18; // 提升 TinyUSB 任务优先级与 LwIP TCPIP 线程对齐，防止 USB 响应被抢占
    tusb_cfg.task.xCoreID = 1;   // 绑定在 Core 1，与运行在 Core 0 的 Wi-Fi / LwIP 形成双核真并行
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // 主机侧 USB 虚拟网卡 MAC (02:02:11:22:33:01)
    tinyusb_net_config_t net_config = {
        .mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = netif_recv_callback,
    };
    ESP_ERROR_CHECK(tinyusb_net_init(&net_config));

    // ESP32 网关侧 MAC (02:02:11:22:33:02)
    uint8_t esp_gateway_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};

    // 配置静态 IP: 192.168.4.1 / 255.255.255.0
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .if_key = "USB_RNDIS",
        .if_desc = "USB RNDIS Virtual Network Device",
        .route_prio = 10,
        .mac = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02},
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,
        .transmit = netif_transmit,
        .driver_free_rx_buffer = l2_free,
    };

    struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        }
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &lwip_netif_config,
    };

    s_usb_netif = esp_netif_new(&cfg);
    if (!s_usb_netif) {
        ESP_LOGE(TAG, "Failed to create USB netif");
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_usb_netif, esp_gateway_mac);

    // 配置 DHCP Server 租期
    uint32_t lease_opt = 1;
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME, &lease_opt, sizeof(lease_opt));

    // 下发国内高可用公共 DNS (114.114.114.114 / 223.5.5.5) 供主机直接高速解析互联网
    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_netif_dhcps_option(s_usb_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));

    esp_netif_dns_info_t dns_info;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(114, 114, 114, 114);
    esp_netif_set_dns_info(s_usb_netif, ESP_NETIF_DNS_MAIN, &dns_info);

    dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(223, 5, 5, 5);
    esp_netif_set_dns_info(s_usb_netif, ESP_NETIF_DNS_BACKUP, &dns_info);

    // 启动网络接口
    esp_netif_action_start(s_usb_netif, 0, 0, 0);
    ESP_LOGI(TAG, "USB RNDIS netif running at 192.168.4.1 (DHCP Server active)");
    return ESP_OK;
}

esp_netif_t *usb_net_get_netif(void)
{
    return s_usb_netif;
}
