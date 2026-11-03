# ESP32-S3 / STM32 硬件与 Agent 协同运行档案 (SSOT)

> 本文档为开发 Agent 的本地运行事实来源，仅保留硬件端口、工具链、核心约束与高频指令；架构及用户向细节详见 [README.md](file:///d:/Projects/esp-rndis/README.md)。

---

## 一、 硬件拓扑与端口

| 物理端口 | 芯片/协议 | 引脚/角色 | 用途与说明 |
| :--- | :--- | :--- | :--- |
| **COM7** | CH343 (UART0) | GPIO43 / GPIO44 | **调试串口**：固件烧录、日志监控 (`idf.py monitor`)，网卡运行期间常驻可用。 |
| **原生 USB** | ESP32-S3 OTG | GPIO19 (D-) / GPIO20 (D+) | **数据网卡**：TinyUSB CDC-NCM，全平台免驱（`192.168.4.1` 网关，DHCP 自动分配）。 |
| **ST-Link** | ST-Link V2 (SWD) | PA13(SWDIO) / PA14(SWCLK) | **STM32 调试器**：STM32H563 烧录与调试（已置于 Stop 2 SVOS5 超低功耗休眠，功耗 ~100µA）。 |

- **Wi-Fi MAC**：`48:ca:43:9c:2c:a8` | **USB PC 端 MAC**：`02:02:11:22:33:01` | **网关端 MAC**：`02:02:11:22:33:02`
- **本地环境**：Windows 11（代理常驻端口 7897，所有本地网关请求必须携带 `--noproxy "*"`）。

---

## 二、 核心代码模块映射

- [`main/main.c`](file:///d:/Projects/esp-rndis/main/main.c)：系统启动主入口与级联初始化。
- [`main/usb_net.c`](file:///d:/Projects/esp-rndis/main/usb_net.c)：TinyUSB CDC-NCM 驱动、NTB 缓冲与 25ms 弹性重试窗口（0 丢包保障）。
- [`main/wifi_mgr.c`](file:///d:/Projects/esp-rndis/main/wifi_mgr.c)：Wi-Fi STA 事件状态机、NVS 凭据持久化、15s 后台重连定时器。
- [`main/web_server.c`](file:///d:/Projects/esp-rndis/main/web_server.c)：Web 控制台页面响应与 `/api/*` REST 接口。
- [`controller_firmware/.../main.c`](file:///d:/Projects/esp-rndis/controller_firmware/stm32h563_controller_firmware/Core/Src/main.c)：STM32H563 超低功耗休眠固件（Stop 2 + SWD 常驻保持 + KEYA 唤醒）。

---

## 三、 核心工程红线与物理边界

1. **射频与供电平衡**：ESP32 最大发射功率严格锁定在 **10 dBm (`esp_wifi_set_max_tx_power(40)`)**，杜绝大电流导致 5V VBUS 压降诱发 USB 掉线。
2. **零符号与内存安全**：Wi-Fi 状态查询必须经 `s_status_lock` 互斥保护；URL 解码严格校验目标缓冲区边界；禁止后台全频段漫游扫描。
3. **SWD 防锁片保障**：STM32H563 进入低功耗必须保留 `HAL_DBGMCU_EnableDBGStopMode()`，且必须保留启动安全延时视窗，严禁零延迟锁死 SWD。

---

## 四、 核心操作指令

```powershell
# 1. 激活 ESP-IDF 环境
. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1

# 2. ESP32 编译与烧录
idf.py build
idf.py -p COM7 flash monitor

# 3. 网关与网络探针验证（注意绕过本地代理）
curl.exe -s --noproxy "*" http://192.168.4.1/api/status
curl.exe -s --noproxy "*" http://192.168.4.1/api/test_net

# 4. STM32H563 编译 (Keil CLI)
$uv4 = "$env:LOCALAPPDATA\Keil_v5\UV4\UV4.exe"
$proj = "D:\Projects\esp-rndis\controller_firmware\stm32h563_controller_firmware\MDK-ARM\stm32h563_controller_firmware.uvprojx"
& $uv4 -b $proj -o build_log.txt

# 5. STM32H563 烧录 (STM32CubeProgrammer CLI)
$prog = "C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304\tools\bin\STM32_Programmer_CLI.exe"
$hex = "D:\Projects\esp-rndis\controller_firmware\stm32h563_controller_firmware\MDK-ARM\controller_firmware\controller_firmware.hex"
& $prog -c port=SWD freq=4000 -d $hex -v -rst
```
