#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 TinyUSB CDC-NCM 虚拟网卡设备及 LwIP 网络接口
 * 
 * 配置网卡静态 IP 为 192.168.4.1，并开启 DHCP Server 为主机分配 IP
 */
esp_err_t usb_net_init(void);

/**
 * @brief 获取 USB 网卡的 esp_netif 实例
 */
esp_netif_t *usb_net_get_netif(void);

/**
 * @brief 获取 USB 发送吞吐统计
 */
void usb_net_get_stats(uint32_t *tx_pkts, uint32_t *tx_retries, uint32_t *tx_fails);

#ifdef __cplusplus
}
#endif
