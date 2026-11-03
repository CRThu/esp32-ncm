#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "usb_net.h"
#include "wifi_mgr.h"
#include "web_server.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;
static int s_scan_hits = 0;
static int s_scan_ret_code = 0;
static int s_send_ret = 0;
static int s_buf_len = 0;

static void urldecode(char *dst, const char *src, size_t dst_max)
{
    if (!dst || dst_max == 0) return;
    char a, b;
    size_t out_len = 0;
    while (*src && out_len < dst_max - 1) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[out_len++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[out_len++] = ' ';
            src++;
        } else {
            dst[out_len++] = *src++;
        }
    }
    dst[out_len] = '\0';
}

static const char INDEX_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>ESP32-S3 随身虚拟网卡控制台</title>\n"
"<style>\n"
":root { --primary: #0066cc; --primary-hover: #0052a3; --bg: #f8fafc; --card: #ffffff; --text: #1e293b; --border: #e2e8f0; }\n"
"* { box-sizing: border-box; }\n"
"body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'PingFang SC', Helvetica, Arial, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 14px 20px; overflow-x: hidden; }\n"
".container { max-width: 1040px; margin: 0 auto; }\n"
".top-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; padding: 0 2px; }\n"
".top-header h1 { margin: 0; font-size: 19px; font-weight: 700; color: #0f172a; display: flex; align-items: center; gap: 8px; }\n"
".top-tag { font-size: 11px; background: #e0f2fe; color: #0284c7; padding: 2px 7px; border-radius: 6px; font-weight: 600; }\n"
".main-grid { display: grid; grid-template-columns: 410px 1fr; gap: 16px; align-items: stretch; }\n"
"@media (max-width: 860px) { .main-grid { grid-template-columns: 1fr; } }\n"
".card { background: var(--card); border-radius: 12px; box-shadow: 0 1px 3px rgba(0,0,0,0.05), 0 4px 10px -2px rgba(0,0,0,0.03); padding: 16px; border: 1px solid var(--border); display: flex; flex-direction: column; }\n"
".card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }\n"
".card-title { margin: 0; font-size: 15px; font-weight: 600; color: #0f172a; }\n"
".badge { display: inline-flex; align-items: center; padding: 3px 9px; border-radius: 20px; font-size: 12px; font-weight: 600; }\n"
".badge-ok { background: #ecfdf5; color: #059669; border: 1px solid #a7f3d0; }\n"
".badge-err { background: #fef2f2; color: #dc2626; border: 1px solid #fecaca; }\n"
".badge-busy { background: #eff6ff; color: #2563eb; border: 1px solid #bfdbfe; }\n"
".badge-warn { background: #fffbeb; color: #d97706; border: 1px solid #fde68a; }\n"
".badge-count { background: #f1f5f9; color: #475569; border: 1px solid #cbd5e1; }\n"
".info-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; background: #f8fafc; padding: 10px 12px; border-radius: 8px; border: 1px solid var(--border); }\n"
".info-item { display: flex; flex-direction: column; gap: 2px; }\n"
".info-label { font-size: 11px; color: #64748b; }\n"
".info-val { font-size: 13px; font-weight: 600; color: #0f172a; font-family: monospace; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }\n"
".btn { background: var(--primary); color: #fff; border: none; border-radius: 7px; padding: 8px 14px; font-size: 13px; font-weight: 600; cursor: pointer; transition: all 0.2s; width: 100%; }\n"
".btn:hover { background: var(--primary-hover); }\n"
".btn:disabled { background: #94a3b8; cursor: not-allowed; }\n"
".btn-secondary { background: #f1f5f9; color: #334155; width: auto; padding: 6px 12px; font-size: 12px; border: 1px solid #cbd5e1; }\n"
".btn-secondary:hover { background: #e2e8f0; }\n"
".btn-danger { background: #fff1f2; color: #e11d48; border: 1px solid #fecdd3; width: auto; padding: 6px 12px; font-size: 12px; }\n"
".btn-danger:hover { background: #ffe4e6; }\n"
"label { display: block; font-size: 12px; font-weight: 500; color: #475569; margin-bottom: 4px; }\n"
"input[type=\"text\"], input[type=\"password\"] { width: 100%; padding: 8px 10px; border: 1px solid var(--border); border-radius: 6px; font-size: 13px; outline: none; transition: border 0.15s; background: #fff; }\n"
"input:focus { border-color: var(--primary); box-shadow: 0 0 0 2px rgba(0,102,204,0.12); }\n"
".wifi-list { flex: 1; min-height: 280px; max-height: 380px; overflow-y: auto; padding-right: 4px; }\n"
".wifi-list::-webkit-scrollbar { width: 5px; }\n"
".wifi-list::-webkit-scrollbar-thumb { background: #cbd5e1; border-radius: 4px; }\n"
".wifi-item { display: flex; justify-content: space-between; align-items: center; padding: 10px 12px; border: 1px solid var(--border); border-radius: 8px; margin-bottom: 6px; cursor: pointer; transition: all 0.15s; background: #ffffff; }\n"
".wifi-item:hover { border-color: var(--primary); background: #f8fafc; }\n"
".wifi-item.selected { border-color: var(--primary); background: #eff6ff; }\n"
".wifi-name { font-weight: 600; font-size: 13px; color: #0f172a; margin-bottom: 2px; }\n"
".wifi-meta { font-size: 11px; color: #64748b; }\n"
".sig-pill { font-size: 11px; font-weight: 600; padding: 2px 7px; border-radius: 10px; }\n"
".sig-strong { background: #ecfdf5; color: #059669; }\n"
".sig-medium { background: #eff6ff; color: #2563eb; }\n"
".sig-weak { background: #f1f5f9; color: #64748b; }\n"
".status-msg { margin-top: 6px; font-size: 12px; text-align: center; font-weight: 500; min-height: 16px; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n"
"  <div class=\"top-header\">\n"
"    <h1><span>⚡ ESP32-S3 随身虚拟网卡</span><span class=\"top-tag\">CDC-NCM / RNDIS</span></h1>\n"
"    <div style=\"font-size:12px; color:#64748b;\">宿主连接: <b style=\"color:#0f172a;\">192.168.4.2</b></div>\n"
"  </div>\n"
"  <div class=\"main-grid\">\n"
"    <div class=\"card\">\n"
"      <div class=\"card-header\">\n"
"        <h3 class=\"card-title\">网卡状态</h3>\n"
"        <span id=\"conn-badge\" class=\"badge badge-err\">未连接</span>\n"
"      </div>\n"
"      <div class=\"info-grid\">\n"
"        <div class=\"info-item\"><span class=\"info-label\">网关 IP:</span><span class=\"info-val\">192.168.4.1</span></div>\n"
"        <div class=\"info-item\"><span class=\"info-label\">上行 IP 地址:</span><span id=\"cur-ip\" class=\"info-val\">--</span></div>\n"
"        <div class=\"info-item\"><span class=\"info-label\">当前 Wi-Fi:</span><span id=\"cur-ssid\" class=\"info-val\">--</span></div>\n"
"        <div class=\"info-item\"><span class=\"info-label\">信号强度:</span><span id=\"cur-rssi\" class=\"info-val\">--</span></div>\n"
"      </div>\n"
"      <div style=\"display:flex; gap:8px; margin-top:10px;\">\n"
"        <button id=\"btn-test\" class=\"btn btn-secondary\" style=\"flex:1;\" onclick=\"testInternet()\">🌐 测网</button>\n"
"        <button id=\"btn-disc\" class=\"btn btn-danger\" style=\"display:none; flex:1;\" onclick=\"disconnectWifi()\">断开连接</button>\n"
"      </div>\n"
"      <div id=\"test-result\" style=\"margin-top:6px; font-size:12px; text-align:center; min-height:16px;\"></div>\n"
"      <hr style=\"border:none; border-top:1px dashed var(--border); margin:10px 0;\">\n"
"      <div style=\"margin-bottom:8px;\">\n"
"        <label for=\"inp-ssid\">目标 Wi-Fi SSID (点击右侧列表自动填入)</label>\n"
"        <input type=\"text\" id=\"inp-ssid\" placeholder=\"请输入或在右侧选择 Wi-Fi 名称\">\n"
"      </div>\n"
"      <div style=\"margin-bottom:8px;\">\n"
"        <label for=\"inp-pass\">Wi-Fi 密码</label>\n"
"        <input type=\"password\" id=\"inp-pass\" placeholder=\"请输入热点密码 (开放热点留空)\">\n"
"      </div>\n"
"      <button id=\"btn-conn\" class=\"btn\" style=\"padding:9px; font-size:14px; margin-top:2px;\" onclick=\"connectWifi()\">上网</button>\n"
"      <div id=\"status-msg\" class=\"status-msg\"></div>\n"
"    </div>\n"
"    <div class=\"card\">\n"
"      <div class=\"card-header\">\n"
"        <div style=\"display:flex; align-items:center; gap:8px;\">\n"
"          <h3 class=\"card-title\">周边 Wi-Fi 热点</h3>\n"
"          <span id=\"ap-count\" class=\"badge badge-count\">0 个热点</span>\n"
"        </div>\n"
"        <button id=\"btn-scan\" class=\"btn btn-secondary\" onclick=\"scanWifi()\">🔄 刷新扫描</button>\n"
"      </div>\n"
"      <div id=\"wifi-container\" class=\"wifi-list\">\n"
"        <div style=\"padding:30px; text-align:center; color:#94a3b8; font-size:13px;\">正在扫描周边热点...</div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<script>\n"
"let isConnecting = false;\n"
"let selectedSsid = '';\n"
"function updateStatus() {\n"
"  fetch('/api/status').then(r => r.json()).then(data => {\n"
"    const badge = document.getElementById('conn-badge');\n"
"    const btnDisc = document.getElementById('btn-disc');\n"
"    if (data.connected) {\n"
"      badge.className = 'badge badge-ok';\n"
"      badge.innerText = '已连接互联网';\n"
"      document.getElementById('cur-ssid').innerText = data.ssid;\n"
"      document.getElementById('cur-ip').innerText = data.ip;\n"
"      document.getElementById('cur-rssi').innerText = data.rssi + ' dBm';\n"
"      btnDisc.style.display = 'inline-block';\n"
"      if (isConnecting) {\n"
"        isConnecting = false;\n"
"        document.getElementById('status-msg').innerHTML = '<span style=\"color:#059669;\">✅ 连接成功！PC 已可通过手机热点上网</span>';\n"
"      }\n"
"    } else if (data.connecting) {\n"
"      badge.className = 'badge badge-busy';\n"
"      badge.innerText = '正在连接...';\n"
"      document.getElementById('cur-ssid').innerText = data.ssid;\n"
"      document.getElementById('cur-ip').innerText = '获取中...';\n"
"      document.getElementById('cur-rssi').innerText = '--';\n"
"      btnDisc.style.display = 'none';\n"
"    } else if (data.auto_reconnect && data.ssid) {\n"
"      badge.className = 'badge badge-warn';\n"
"      badge.innerText = '等待热点靠近 (15s重试)';\n"
"      document.getElementById('cur-ssid').innerText = data.ssid + ' (重连中)';\n"
"      document.getElementById('cur-ip').innerText = '--';\n"
"      document.getElementById('cur-rssi').innerText = '--';\n"
"      btnDisc.style.display = 'inline-block';\n"
"      if (isConnecting) {\n"
"        isConnecting = false;\n"
"      }\n"
"    } else {\n"
"      badge.className = 'badge badge-err';\n"
"      badge.innerText = '未连接';\n"
"      document.getElementById('cur-ssid').innerText = '--';\n"
"      document.getElementById('cur-ip').innerText = '--';\n"
"      document.getElementById('cur-rssi').innerText = '--';\n"
"      btnDisc.style.display = 'none';\n"
"      if (isConnecting) {\n"
"        isConnecting = false;\n"
"        document.getElementById('status-msg').innerHTML = '<span style=\"color:#dc2626;\">❌ 连接失败或已断开，请检查热点与密码</span>';\n"
"      }\n"
"    }\n"
"  }).catch(() => {});\n"
"}\n"
"function renderAps(aps) {\n"
"  const container = document.getElementById('wifi-container');\n"
"  const countBadge = document.getElementById('ap-count');\n"
"  if (!aps || aps.length === 0) {\n"
"    container.innerHTML = '<div style=\"padding:30px; text-align:center; color:#94a3b8;\">正在搜索周边热点，请稍候...</div>';\n"
"    countBadge.innerText = '0 个热点';\n"
"    return false;\n"
"  }\n"
"  countBadge.innerText = aps.length + ' 个热点';\n"
"  let html = '';\n"
"  aps.forEach(ap => {\n"
"    const lock = ap.auth === 0 ? '🔓 开放' : '🔒 加密';\n"
"    const safeSsid = ap.ssid.replace(/\\'/g, \"\\\\'\");\n"
"    const isSel = (ap.ssid === selectedSsid) ? ' selected' : '';\n"
"    let sigClass = 'sig-weak';\n"
"    let sigText = '较弱';\n"
"    if (ap.rssi >= -60) {\n"
"      sigClass = 'sig-strong';\n"
"      sigText = '极强';\n"
"    } else if (ap.rssi >= -75) {\n"
"      sigClass = 'sig-medium';\n"
"      sigText = '良好';\n"
"    }\n"
"    html += `<div class=\"wifi-item${isSel}\" onclick=\"selectSsid('${safeSsid}')\">` +\n"
"            `  <div>` +\n"
"            `    <div class=\"wifi-name\">📶 ${ap.ssid}</div>` +\n"
"            `    <div class=\"wifi-meta\"><span>${lock}</span></div>` +\n"
"            `  </div>` +\n"
"            `  <div>` +\n"
"            `    <span class=\"sig-pill ${sigClass}\">${ap.rssi} dBm (${sigText})</span>` +\n"
"            `  </div>` +\n"
"            `</div>`;\n"
"  });\n"
"  container.innerHTML = html;\n"
"  return true;\n"
"}\n"
"let scanPollCount = 0;\n"
"function scanWifi() {\n"
"  const btn = document.getElementById('btn-scan');\n"
"  btn.disabled = true;\n"
"  btn.innerText = '扫描中...';\n"
"  scanPollCount = 0;\n"
"  function doPoll() {\n"
"    fetch('/api/scan').then(r => r.json()).then(aps => {\n"
"      if (aps && aps.length > 0) {\n"
"        renderAps(aps);\n"
"        btn.disabled = false;\n"
"        btn.innerText = '🔄 刷新扫描';\n"
"      } else if (scanPollCount < 8) {\n"
"        scanPollCount++;\n"
"        setTimeout(doPoll, 700);\n"
"      } else {\n"
"        renderAps([]);\n"
"        btn.disabled = false;\n"
"        btn.innerText = '🔄 刷新扫描';\n"
"      }\n"
"    }).catch(() => {\n"
"      btn.disabled = false;\n"
"      btn.innerText = '🔄 刷新扫描';\n"
"    });\n"
"  }\n"
"  doPoll();\n"
"}\n"
"function testInternet() {\n"
"  const btn = document.getElementById('btn-test');\n"
"  const res = document.getElementById('test-result');\n"
"  btn.disabled = true;\n"
"  btn.innerText = '测试中...';\n"
"  res.innerHTML = '<span style=\"color:#64748b;\">正在探测公网连通性...</span>';\n"
"  fetch('/api/test_net').then(r => r.json()).then(d => {\n"
"    if (d.ok) {\n"
"      res.innerHTML = `<span style=\"color:#059669; font-weight:600;\">✅ 公网畅通 (延迟 ${d.rtt_ms}ms)</span>`;\n"
"    } else {\n"
"      res.innerHTML = `<span style=\"color:#dc2626; font-weight:600;\">❌ 无法访问公网 (${d.msg || '请检查热点'})</span>`;\n"
"    }\n"
"  }).catch(() => {\n"
"    res.innerHTML = '<span style=\"color:#dc2626;\">探测请求异常</span>';\n"
"  }).finally(() => {\n"
"    btn.disabled = false;\n"
"    btn.innerText = '🌐 测网';\n"
"  });\n"
"}\n"
"function selectSsid(name) {\n"
"  selectedSsid = name;\n"
"  document.getElementById('inp-ssid').value = name;\n"
"  document.getElementById('inp-pass').focus();\n"
"  const items = document.querySelectorAll('.wifi-item');\n"
"  items.forEach(el => {\n"
"    if (el.innerText.includes(name)) el.classList.add('selected');\n"
"    else el.classList.remove('selected');\n"
"  });\n"
"}\n"
"function connectWifi() {\n"
"  const ssid = document.getElementById('inp-ssid').value.trim();\n"
"  const pass = document.getElementById('inp-pass').value;\n"
"  if (!ssid) { alert('请输入或选择 Wi-Fi 名称'); return; }\n"
"  isConnecting = true;\n"
"  document.getElementById('status-msg').innerHTML = '<span style=\"color:#0066cc;\">正在向 ESP32 发起连接请求...</span>';\n"
"  fetch(`/api/connect?ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(pass)}`)\n"
"    .then(r => r.json())\n"
"    .then(() => {\n"
"      document.getElementById('status-msg').innerHTML = '<span style=\"color:#0066cc;\">正在连接 ' + ssid + '，请稍候...</span>';\n"
"    })\n"
"    .catch(() => {\n"
"      document.getElementById('status-msg').innerHTML = '<span style=\"color:#dc2626;\">指令下发异常</span>';\n"
"      isConnecting = false;\n"
"    });\n"
"}\n"
"function disconnectWifi() {\n"
"  if (!confirm('确定断开当前 Wi-Fi 连接并停止自动重连吗？')) return;\n"
"  fetch('/api/disconnect').then(() => updateStatus());\n"
"}\n"
"updateStatus();\n"
"scanWifi();\n"
"setInterval(updateStatus, 3000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    wifi_mgr_status_t status;
    wifi_mgr_get_status(&status);

    esp_reset_reason_t rst = esp_reset_reason();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    uint32_t tx_pkts = 0, tx_retries = 0, tx_fails = 0;
    usb_net_get_stats(&tx_pkts, &tx_retries, &tx_fails);

    char resp[450];
    snprintf(resp, sizeof(resp),
             "{\"connected\":%s,\"connecting\":%s,\"auto_reconnect\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"gateway\":\"192.168.4.1\",\"reset_reason\":%d,\"uptime_s\":%lld,\"tx_pkts\":%lu,\"tx_retries\":%lu,\"tx_fails\":%lu}",
             status.is_connected ? "true" : "false",
             status.is_connecting ? "true" : "false",
             status.auto_reconnect ? "true" : "false",
             status.ssid,
             status.ip,
             status.rssi,
             (int)rst,
             (long long)uptime_s,
             (unsigned long)tx_pkts,
             (unsigned long)tx_retries,
             (unsigned long)tx_fails);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    s_scan_hits++;
    ESP_LOGI(TAG, "GET /api/scan requested (hit #%d)", s_scan_hits);
    char *buf = malloc(2048);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate 2048 bytes for scan response");
        httpd_resp_send_500(req);
        s_scan_ret_code = -1;
        return ESP_FAIL;
    }

    s_scan_ret_code = wifi_mgr_scan(buf, 2048);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    s_buf_len = (int)strlen(buf);
    esp_err_t ret = httpd_resp_send(req, buf, s_buf_len);
    s_send_ret = (int)ret;
    free(buf);
    return ret;
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    char ssid[65] = {0};
    char password[65] = {0};

    char query[512] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char raw_ssid[128] = {0};
        char raw_pass[128] = {0};
        if (httpd_query_key_value(query, "ssid", raw_ssid, sizeof(raw_ssid)) == ESP_OK) {
            urldecode(ssid, raw_ssid, sizeof(ssid));
        }
        if (httpd_query_key_value(query, "password", raw_pass, sizeof(raw_pass)) == ESP_OK) {
            urldecode(password, raw_pass, sizeof(password));
        }
    }

    if (strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"error\":\"SSID cannot be empty\"}", HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "Web connect request: SSID='%s'", ssid);
    wifi_mgr_connect(ssid, password);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"connecting\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t disconnect_handler(httpd_req_t *req)
{
    wifi_mgr_disconnect();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"disconnected\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t test_net_handler(httpd_req_t *req)
{
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);
    if (!st.is_connected) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Wi-Fi未连接\"}", HTTPD_RESP_USE_STRLEN);
    }

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"Socket创建失败\"}", HTTPD_RESP_USE_STRLEN);
    }

    // 设置非阻塞套接字，防止丢包时阻断 httpd 工作线程
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in dest = { 0 };
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);
    dest.sin_addr.s_addr = inet_addr("223.5.5.5"); // 阿里公共 DNS

    int64_t t0 = esp_timer_get_time();
    int res = connect(s, (struct sockaddr *)&dest, sizeof(dest));
    int rtt_ms = -1;
    bool ok = false;

    if (res == 0) {
        ok = true;
        rtt_ms = (int)((esp_timer_get_time() - t0) / 1000);
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 }; // 严格 2 秒超时
        int sel_res = select(s + 1, NULL, &wfds, NULL, &tv);
        if (sel_res > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                ok = true;
                rtt_ms = (int)((esp_timer_get_time() - t0) / 1000);
            }
        }
    }
    close(s);

    char resp[128];
    if (ok) {
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"rtt_ms\":%d}", rtt_ms);
    } else {
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"msg\":\"公网连接超时\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 5;
    config.max_uri_handlers = 16;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP Server on port: %d", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
        return ESP_FAIL;
    }

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler };
    httpd_register_uri_handler(s_server, &uri_status);

    httpd_uri_t uri_scan = { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get_handler };
    httpd_register_uri_handler(s_server, &uri_scan);

    httpd_uri_t uri_connect = { .uri = "/api/connect", .method = HTTP_GET, .handler = connect_handler };
    httpd_register_uri_handler(s_server, &uri_connect);

    httpd_uri_t uri_disconnect = { .uri = "/api/disconnect", .method = HTTP_GET, .handler = disconnect_handler };
    httpd_register_uri_handler(s_server, &uri_disconnect);

    httpd_uri_t uri_test = { .uri = "/api/test_net", .method = HTTP_GET, .handler = test_net_handler };
    httpd_register_uri_handler(s_server, &uri_test);

    const char *captive_uris[] = {
        "/generate_204", "/hotspot-detect.html", "/ncsi.txt", "/connecttest.txt"
    };
    for (size_t i = 0; i < sizeof(captive_uris) / sizeof(captive_uris[0]); i++) {
        httpd_uri_t uri_cp = { .uri = captive_uris[i], .method = HTTP_GET, .handler = captive_portal_handler };
        httpd_register_uri_handler(s_server, &uri_cp);
    }

    ESP_LOGI(TAG, "HTTP Server handlers registered successfully");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
