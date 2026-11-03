#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Web 配置控制台与 REST API 服务
 */
esp_err_t web_server_start(void);

/**
 * @brief 停止 Web 服务
 */
void web_server_stop(void);

#ifdef __cplusplus
}
#endif
