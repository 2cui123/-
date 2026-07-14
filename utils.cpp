
// 本应用无关的通用工具函数，用于支持：
// - WiFi / 以太网
// - NTP
// - 远程日志
// - Base64 编码
// - 设备休眠
//
// s60sc 2021, 2023, 2025
// 部分函数基于 gemi254 贡献的代码

#include "appGlobals.h"

bool dbgVerbose = false;
bool timeSynchronized = false;
bool monitorOpen = true;
bool dataFilesChecked = false;
// 允许远程设备通过浏览器报告任何启动失败
char startupFailure[SF_LEN] = {0};
size_t alertBufferSize = 0;
size_t maxAlertBuffSize = 32 * 1024;
byte* alertBuffer = NULL; // Telegram / SMTP 告警图片缓冲区
RTC_NOINIT_ATTR uint32_t crashLoop;
RTC_NOINIT_ATTR char brownoutStatus;
static void initBrownout(void);
static void printPartitionTable();
int wakePin; // 当 wakeUse 为 true 时使用
int wakeLevel; // 当 wakeUse 为 true 时使用
bool wakeUse = false; // 为 true 时允许应用休眠和唤醒
char* jsonBuff = NULL;
char portFwd[6] = "";
UBaseType_t HEAP_MEM; // 允许部分任务栈在可用时使用 PSRAM

/************************** 网络 (WiFi/以太网) **************************/

#include <esp_task_wdt.h>
 
/** 除非您清楚自己在做什么，否则请勿硬编码以下内容 **/
/** 请使用 Web 界面配置 WiFi 设置 **/

char hostName[MAX_HOST_LEN] = ""; // 默认主机名
char ST_SSID[MAX_HOST_LEN]  = ""; // 默认路由器 SSID
char ST_Pass[MAX_PWD_LEN] = ""; // 默认路由器密码

// 以下留空则使用 DHCP
char ST_ip[MAX_IP_LEN]  = ""; // 静态 IP
char ST_sn[MAX_IP_LEN]  = ""; // 子网掩码，通常为 255.255.255.0
char ST_gw[MAX_IP_LEN]  = ""; // 互联网网关，通常为路由器 IP
char ST_ns1[MAX_IP_LEN] = ""; // DNS 服务器，可为路由器 IP（SNTP 需要）
char ST_ns2[MAX_IP_LEN] = ""; // 备用 DNS 服务器，可为空

// 接入点配置门户 SSID 和密码
char AP_SSID[MAX_HOST_LEN] = "";
char AP_Pass[MAX_PWD_LEN] = "";
char AP_ip[MAX_IP_LEN]  = ""; // 留空则使用 192.168.4.1
char AP_sn[MAX_IP_LEN]  = "";
char AP_gw[MAX_IP_LEN]  = "";

// 以太网 SPI 引脚
int ethCS = -1; // W5500 片选 / LAN8720 MDC
int ethInt = -1; // W5500 中断 / LAN8720 MDIO
int ethRst = -1; // W5500 复位 / LAN8720 POWER
int ethSclk = -1; // W5500 SPI 时钟 / LAN8720 CLOCK
int ethMiso = -1; // W5500 SPI 数据引脚
int ethMosi = -1; // W5500 SPI 数据引脚

// Web 页面基本 HTTP 身份验证
char Auth_Name[MAX_HOST_LEN] = ""; 
char Auth_Pass[MAX_PWD_LEN] = "";

int responseTimeoutSecs = 10; // 等待 FTP 或 SMTP 响应的超时时间
bool allowAP = true;  // 设为 true 时，若无法连接 STA（路由器）则启动 AP
uint32_t wifiTimeoutSecs = 30; // WiFi 状态检查间隔
static bool APstarted = false;
esp_ping_handle_t pingHandle = NULL;
bool usePing = true;

static void startPing();
static void printGpioInfo();
static void boardInfo();

int netMode = 0; // 0=仅 WiFi，1=仅以太网，2=以太网+AP

// LAN8720
#define ETH_PHY_ADDR  0 
#define ETH_CLK_MODE  ETH_CLOCK_GPIO0_IN // 来自晶振的外部时钟

static void setupMdnsHost() {  
  // 设置 MDNS 服务
  char mdnsName[MAX_IP_LEN]; // MDNS 主机名最大长度
  snprintf(mdnsName, MAX_IP_LEN, "%.*s", MAX_IP_LEN - 1, hostName);
  if (MDNS.begin(mdnsName)) {
    // 向 MDNS 添加服务
    useHttps ? MDNS.addService("https", "tcp", HTTPS_PORT) : MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.addService("ws", "udp", 83);
    MDNS.addService("ftp", "tcp", 21);    
    LOG_INF("mDNS service: http%s://%s.local", useHttps ? "s" : "", mdnsName);
  } else LOG_WRN("mDNS host: %s Failed", mdnsName);
  debugMemory("setupMdnsHost");
}

static const char* wifiStatusStr(wl_status_t wlStat) {
  switch (wlStat) {
    case WL_NO_SHIELD: return "wifi not initialised";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "not available, use AP";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "unable to connect";  
    case WL_STOPPED: return "wifi stopped";
    default: return "Invalid WiFi.status";
  }
}

const char* getEncType(int ssidIndex) {
  switch (WiFi.encryptionType(ssidIndex)) {
    case (WIFI_AUTH_OPEN): return "Open";
    case (WIFI_AUTH_WEP): return "WEP";
    case (WIFI_AUTH_WPA_PSK): return "WPA_PSK";
    case (WIFI_AUTH_WPA2_PSK): return "WPA2_PSK";
    case (WIFI_AUTH_WPA_WPA2_PSK): return "WPA_WPA2_PSK";
    case (WIFI_AUTH_WPA2_ENTERPRISE): return "WPA2_ENTERPRISE";
    case (WIFI_AUTH_MAX): return "AUTH_MAX";
    default: return "Not listed";
  }
}

static void onNetEvent(arduino_event_id_t event, arduino_event_info_t info) {
  // 网络事件回调报告
  switch (event) {
    case ARDUINO_EVENT_WIFI_READY: break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE: break;
    case ARDUINO_EVENT_WIFI_STA_START: LOG_INF("Wifi Station started, connecting to: %s", ST_SSID); break;
    case ARDUINO_EVENT_WIFI_STA_STOP: LOG_INF("Wifi Station stopped %s", ST_SSID); break;
    case ARDUINO_EVENT_WIFI_AP_START: {
      if (strlen(AP_SSID) && !strcmp(WiFi.AP.SSID().c_str(), AP_SSID)) {
        LOG_INF("Wifi AP SSID: %s started, use 'http%s://%s' to connect", WiFi.AP.SSID().c_str(), useHttps ? "s" : "", WiFi.AP.localIP().toString().c_str());
        APstarted = true;
      }
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STOP: {
      if (!strcmp(WiFi.AP.SSID().c_str(), AP_SSID)) {
        LOG_INF("Wifi AP stopped: %s", AP_SSID);
        APstarted = false;
      }
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: LOG_INF("Wifi Station IP, use '%s://%s' to connect", useHttps ? "https" : "http", WiFi.STA.localIP().toString().c_str()); break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP: LOG_INF("Wifi Station lost IP"); break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: LOG_INF("WiFi Station connection to %s, using hostname: %s", ST_SSID, hostName); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: LOG_INF("WiFi Station disconnected"); break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: LOG_INF("WiFi AP client connection"); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: LOG_INF("WiFi AP client disconnection"); break;
    case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED: break;
    case ARDUINO_EVENT_WIFI_AP_GOT_IP6: LOG_INF("AP interface V6 IP addr is preferred"); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP6: LOG_INF("Station interface V6 IP addr is preferred"); break;

    case ARDUINO_EVENT_ETH_START: LOG_INF("Ethernet started, speed %uMHz", ETH.linkSpeed()); break;
    case ARDUINO_EVENT_ETH_CONNECTED: LOG_INF("Ethernet connected, MAC: %s", ETH.macAddress().c_str()); break;
    case ARDUINO_EVENT_ETH_STOP: LOG_INF("Ethernet Stopped"); break;
    case ARDUINO_EVENT_ETH_GOT_IP: {
      LOG_INF("Ethernet IP, use '%s://%s' to connect", useHttps ? "https" : "http", ETH.localIP().toString().c_str()); 
      if (netMode == 2) WiFi.AP.enableNAPT(true);
      break;
    }
    case ARDUINO_EVENT_ETH_DISCONNECTED: {
      LOG_INF("Ethernet disconnected");
      if (netMode == 2) WiFi.AP.enableNAPT(false);
      break;
    }
    case ARDUINO_EVENT_ETH_LOST_IP: {
      LOG_INF("Ethernet lost IP");
      if (netMode == 2) WiFi.AP.enableNAPT(false);
      break;
    }
    default: LOG_WRN("Unhandled network event %d", event); break;
  }
}

static void setWifiAP() {
  if (!APstarted) {
    WiFi.AP.begin();
    // 若提供了静态 IP，则设置接入点
    if (strlen(AP_ip) > 1) {
      LOG_INF("Set AP static IP :%s, %s, %s", AP_ip, AP_gw, AP_sn);  
      IPAddress _ip, _gw, _sn, _ns1, _ns2;
      _ip.fromString(AP_ip);
      _gw.fromString(AP_gw);
      _sn.fromString(AP_sn);
      // 设置静态 IP
      WiFi.AP.config(_ip, _gw, _sn);
    } 
    WiFi.AP.create(AP_SSID, AP_Pass);
    debugMemory("setWifiAP");
  }
}

static void setWifiSTA() {
  // 若提供了静态 IP，则设置站点
  if (strlen(ST_ip) > 1) {
    IPAddress _ip, _gw, _sn, _ns1, _ns2;
    if (!_ip.fromString(ST_ip)) LOG_WRN("Failed to parse IP: %s", ST_ip);
    else {
      _ip.fromString(ST_ip);
      _gw.fromString(ST_gw);
      _sn.fromString(ST_sn);
      _ns1.fromString(ST_ns1);
      _ns2.fromString(ST_ns2);
      // 设置静态 IP
      WiFi.STA.config(_ip, _gw, _sn, _ns1); // SNTP 需要 DNS
      LOG_INF("Wifi Station set static IP");
    } 
  } else LOG_INF("Wifi Station IP from DHCP");
  WiFi.STA.enableIPv6(USE_IP6); 
  WiFi.STA.begin();
  WiFi.STA.connect(ST_SSID, ST_Pass);
  debugMemory("setWifiSTA");
}

static void predefEthPins() {
  // 若已定义，则设置板级特定引脚
#if defined(ETH_CS)
  char ethPin[3];
  sprintf(ethPin, "%d", ETH_CS);
  updateStatus("ethCS", ethPin);
  sprintf(ethPin, "%d", ETH_INT);
  updateStatus("ethInt", ethPin);
  sprintf(ethPin, "%d", ETH_RST);
  updateStatus("ethRst", ethPin);
  sprintf(ethPin, "%d", ETH_SCLK);
  updateStatus("ethSclk", ethPin);
  sprintf(ethPin, "%d", ETH_MISO);
  updateStatus("ethMiso", ethPin);
  sprintf(ethPin, "%d", ETH_MOSI);
  updateStatus("ethMosi", ethPin);
#endif
}

static bool startEth() {
  // 通过 SPI 初始化以太网 (W5500)，仅适用于 ESP32-S3 板
  // ESP32-S3-ETH 板为板载，或使用独立外接板
  if (ethCS != -1) {
    ETH.end();
#if CONFIG_IDF_TARGET_ESP32S3
    if (!ETH.begin(ETH_PHY_W5500,
                   ETH_PHY_ADDR_AUTO,
                   ethCS,
                   ethInt,
                   ethRst,
                   SPI2_HOST,
                   ethSclk,
                   ethMiso,
                   ethMosi,
                   ETH_PHY_SPI_FREQ_MHZ)) { 
      LOG_WRN("Ethernet W5500 init failed");
      return false;
    }
#endif
#if CONFIG_IDF_TARGET_ESP32
 #ifdef ISCAM
    LOG_WRN("Insufficient pins for Ethernet on ESP32");
    netMode = 0;
    return false;
 #else
    // RMII 使用预定义引脚 19、21、22、25、26、27
    if (!ETH.begin(ETH_PHY_LAN8720,
                   ETH_PHY_ADDR,
                   ethCS,  // LAN8720 MDC
                   ethInt, // LAN8720 MDIO
                   ethRst, // LAN8720 POWER
                   ETH_CLK_MODE)) { 
      LOG_WRN("Ethernet LAN8720 init failed");
      return false;
    }
 #endif
#endif

    // 若现有字段中已配置，则为以太网应用静态 IP
    if (strlen(ST_ip) > 1) {
      IPAddress _ip, _gw, _sn, _ns1, _ns2;
      if (_ip.fromString(ST_ip)) {
        _gw.fromString(ST_gw);
        _sn.fromString(ST_sn);
        _ns1.fromString(ST_ns1);
        _ns2.fromString(ST_ns2);
        ETH.config(_ip, _gw, _sn, _ns1, _ns2);
        LOG_INF("Ethernet set static IP");
      } else LOG_WRN("Failed to parse Ethernet static IP: %s", ST_ip);
    }
  } else {
    LOG_WRN("Ethernet pins not defined");
    return false;
  }

  // 等待链路建立及 DHCP 或静态 IP 分配
  uint32_t startAttemptTime = millis();
  while (!ETH.linkUp() && millis() - startAttemptTime < 8000) delay(100);
  if (!ETH.linkUp()) LOG_WRN("Ethernet link not up");
  startAttemptTime = millis();
  while (!ETH.localIP() && millis() - startAttemptTime < 8000) delay(100);
  if (!ETH.localIP()) LOG_WRN("Ethernet no IP yet");
  setupMdnsHost();
  if (pingHandle == NULL) startPing();
  return ETH.linkUp();
}

static bool startWifi(bool firstcall = true) {
  // 启动 WiFi 站点（若允许或未定义站点则同时启动 WiFi AP）
  if (firstcall) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.persistent(false); // 防止将 WiFi 凭据写入 Flash
    WiFi.STA.setAutoReconnect(false); // 设置断开后模块是否尝试重新连接接入点
    WiFi.AP.clear();
    WiFi.AP.end(); // 启动时关闭异常 AP
    WiFi.STA.setHostname(hostName);
    delay(100);
  }
  
  wl_status_t wlStat = WL_NO_SSID_AVAIL;
  if (netMode == 0) {
    // 连接 WiFi 站点
    setWifiSTA();
    uint32_t startAttemptTime = millis();
    // 超时失败后停止尝试，稍后通过 ping 重连
    wlStat = WL_NO_SSID_AVAIL;
    if (strlen(ST_SSID)) {
      while (wlStat = WiFi.STA.status(), wlStat != WL_CONNECTED && millis() - startAttemptTime < 5000)  {
        logPrint(".");
        delay(500);
      }
    }
    // 显示所请求 SSID 的统计信息
    int numNetworks = WiFi.scanNetworks();
    for (int i=0; i < numNetworks; i++) {
      if (!strcmp(WiFi.SSID(i).c_str(), ST_SSID))
        LOG_INF("Wifi stats for %s - signal strength: %d dBm; Encryption: %s; channel: %u",  ST_SSID, WiFi.RSSI(i), getEncType(i), WiFi.channel(i));
    }
    if (wlStat != WL_CONNECTED) LOG_WRN("SSID %s not connected %s", ST_SSID, wifiStatusStr(wlStat));
  }
  
  if (wlStat == WL_NO_SSID_AVAIL || allowAP) setWifiAP(); // 无站点 SSID 时允许 AP，例如首次使用
#if CONFIG_IDF_TARGET_ESP32S3
  if (netMode == 0) setupMdnsHost(); // ESP32 上不启用，因占用 6k 堆内存
#endif
  if (pingHandle == NULL) startPing();
  return wlStat == WL_CONNECTED ? true : false;
}

bool startNetwork(bool firstcall) {
  // 按配置启动 WiFi、以太网或以太网+AP
  Network.onEvent(onNetEvent);
  predefEthPins();
  if (netMode > 0) {
    // 以太网或以太网+AP
    if (startEth()) {
      if (netMode == 1) {
        // 静默模式：关闭 WiFi/BLE 射频以实现射频静默
        WiFi.mode(WIFI_OFF);
#ifdef APP_BT_ENABLED
        if (btStarted()) btStop();
#endif
        return true;
      }
    } else {
      LOG_WRN("Ethernet start failed, falling back to WiFi");
      ETH.end();
      WiFi.AP.enableNAPT(false);
      netMode = 0;
    }
  }
  // 仅 WiFi / 以太网失败 / 以太网+AP
  if (netMode == 2) {
    WiFi.AP.enableNAPT(true);
    allowAP = true;
  }
  return startWifi(firstcall);
}

IPAddress netLocalIP() { return (netMode > 0) ? ETH.localIP() : WiFi.STA.localIP(); }
IPAddress netGatewayIP() { return (netMode > 0) ? ETH.gatewayIP() : WiFi.STA.gatewayIP(); }
String netMacAddress() { return (netMode > 0) ? ETH.macAddress().c_str() : WiFi.STA.macAddress().c_str(); }
int netRSSI() { return (netMode == 1) ? 0 : WiFi.STA.RSSI(); }
bool netIsConnected() { return (netMode > 0) ? (ETH.linkUp() && ETH.localIP()) : (WiFi.STA.status() == WL_CONNECTED); }

void resetWatchDog(int wdIndex, uint32_t wdTimeout) {
  // 针对特定任务的自定义看门狗
  // ping 任务 (0) 用作 ESP 冻结时的看门狗
  static bool watchDogStarted[4] = {false, false, false, false};
  if (watchDogStarted[wdIndex]) esp_task_wdt_reset();
  else {
    // 首次调用时设置看门狗
    esp_task_wdt_deinit(); 
    esp_task_wdt_config_t twdt_config = {
      .timeout_ms = wdTimeout,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true, // 看门狗告警时触发崩溃中止（包含 wdt_isr）
    };
    esp_task_wdt_init(&twdt_config);
    esp_task_wdt_add(NULL);
    if (esp_task_wdt_status(NULL) == ESP_OK) {
      watchDogStarted[wdIndex] = true;
      esp_task_wdt_reset();
      LOG_INF("WatchDog started for task: %s", pcTaskGetName(NULL));
    } else LOG_ERR("WatchDog failed to start for task: %s ", pcTaskGetName(NULL));
  }
}

static void statusCheck() {
  // 定期状态检查
  doAppPing();
  if (!timeSynchronized) getLocalNTP();
  if (!dataFilesChecked) dataFilesChecked = checkDataFiles();
#if INCLUDE_MQTT
  if (mqtt_active) startMqttClient();
#endif
}

void resetCrashLoop() {
  crashLoop = 0;
}

static void pingSuccess(esp_ping_handle_t hdl, void *args) {
  //uint32_t elapsed_time;
  //esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
  if (DEBUG_MEM) {
    static uint32_t minStack = UINT32_MAX;
    uint32_t freeStack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (freeStack < minStack) {
      minStack = freeStack;
      if (freeStack < MIN_STACK_FREE) LOG_WRN("Task ping stack space only: %u", freeStack);
      else LOG_INF("Task ping stack space reduced to: %u", freeStack);
    }
  }
  resetWatchDog(0, wifiTimeoutSecs * 1000 * 2);
  if (dataFilesChecked) resetCrashLoop();
  statusCheck();
}

static void pingTimeout(esp_ping_handle_t hdl, void *args) {
  // 使用 ping 检测是因为 ESP 可能仍保持与网关的连接，但该连接可能已不可用，ping 失败可检测到
  // 但部分路由器可能不响应 ping - https://github.com/s60sc/ESP32-CAM_MJPEG2SD/issues/221
  // 因此将 usePing 设为 false 时，若连接仍存在则忽略 ping 失败
  resetWatchDog(0, wifiTimeoutSecs * 1000 * 2);
  if (netMode > 0) {
    if (usePing) {
      LOG_WRN("Failed to ping gateway, restart ethernet ...");
      startNetwork(false);
    } else {
      if (netIsConnected()) statusCheck();
      else {
        LOG_WRN("Disconnected, restart ethernet ...");
        startNetwork(false);
      }
    }
  } else {
    if (strlen(ST_SSID)) {
      wl_status_t wStat = WiFi.STA.status();
      if (wStat != WL_NO_SSID_AVAIL && wStat != WL_NO_SHIELD) {
        if (usePing) {
          LOG_WRN("Failed to ping gateway, restart wifi ...");
          startWifi(false);
        } else {
          if (wStat == WL_CONNECTED) statusCheck();
          else {
            LOG_WRN("Disconnected, restart wifi ...");
            startWifi(false);
          }
        }
      }
    }
  }
}

static void startPing() {
  IPAddress ipAddr = netGatewayIP();
  if (!ipAddr) return; // 网关已知前不启动 ping
  ip_addr_t pingDest; 
  IP_ADDR4(&pingDest, ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
  esp_ping_config_t pingConfig = ESP_PING_DEFAULT_CONFIG();
  pingConfig.target_addr = pingDest;  
  pingConfig.count = ESP_PING_COUNT_INFINITE;
  pingConfig.interval_ms = wifiTimeoutSecs * 1000;
  pingConfig.timeout_ms = 5000;
  pingConfig.task_stack_size = PING_STACK_SIZE;
  pingConfig.task_prio = 1;
  // 设置 ping 任务回调函数
  esp_ping_callbacks_t cbs;
  cbs.on_ping_success = pingSuccess;
  cbs.on_ping_timeout = pingTimeout;
  cbs.on_ping_end = NULL; 
  cbs.cb_args = NULL;
  esp_ping_new_session(&pingConfig, &cbs, &pingHandle);
  esp_ping_start(pingHandle);
  LOG_INF("Started ping monitoring - %s", usePing ? "On" : "Off");
  debugMemory("startPing");
}

void stopPing() {
  if (pingHandle != NULL) {
    esp_ping_stop(pingHandle);
    esp_ping_delete_session(pingHandle);
    pingHandle = NULL;
  }
}

#define EXT_IP_HOST "api.ipify.org"
char extIP[MAX_IP_LEN] = "Not assigned"; // 路由器外网 IP
bool doGetExtIP = true;

void getExtIP() {
  // 获取外网 IP 地址
  if (doGetExtIP) { 
    NetworkClientSecure hclient;
    if (remoteServerConnect(hclient, EXT_IP_HOST, HTTPS_PORT, "", GETEXTIP)) {
      HTTPClient https;
      int httpCode = HTTP_CODE_NOT_FOUND;
      if (https.begin(hclient, EXT_IP_HOST, HTTPS_PORT, "/", true)) {
        char newExtIp[MAX_IP_LEN] = "";
        httpCode = https.GET();
        if (httpCode == HTTP_CODE_OK) {
          strncpy(newExtIp, https.getString().c_str(), sizeof(newExtIp) - 1);  
          if (strcmp(newExtIp, extIP)) {
            // 外网 IP 已变更
            strncpy(extIP, newExtIp, sizeof(extIP) - 1);
            updateStatus("extIP", extIP);
            updateStatus("save", "0");
            externalAlert("External IP changed", extIP);
          } else LOG_INF("External IP: %s", extIP);
        } else LOG_WRN("External IP request failed, error: %s", https.errorToString(httpCode).c_str());    
        if (httpCode != HTTP_CODE_OK) doGetExtIP = false;
        https.end();     
      }
      remoteServerClose(hclient);
    }
  }
}

/************** 通用安全网络客户端函数 ******************/

static uint8_t failCounts[REMFAILCNT] = {0};

void remoteServerClose(NetworkClientSecure& sclient) {
  if (sclient.available()) sclient.clear();
  if (sclient.connected()) sclient.stop();
}

bool remoteServerConnect(NetworkClientSecure& sclient, const char* serverName, uint16_t serverPort, const char* serverCert, uint8_t connIdx) {
  // 若尚未连接或先前已断开，则连接服务器
  if (sclient.connected()) return true;
  else {
    if (failCounts[connIdx] >= MAX_FAIL) {
      if (failCounts[connIdx] == MAX_FAIL) {
        LOG_ERR("Abandon %s connection attempt until next rollover", serverName);
        failCounts[connIdx] = MAX_FAIL + 1;
      }
    } else {
      if (ESP.getFreeHeap() > TLS_HEAP) {
        // 未连接，因此在限定时间内尝试连接
        if (useSecure && strlen(serverCert)) sclient.setCACert(serverCert);
        else sclient.setInsecure(); // 不校验证书
    
        uint32_t startTime = millis();
        while (!sclient.connected()) {
          if (sclient.connect(serverName, serverPort)) break;
          if (millis() - startTime > responseTimeoutSecs * 1000) break;
          delay(2000);
        }
        if (sclient.connected()) {
          failCounts[connIdx] = 0;
          return true;
        }
        else {
          // 在分配时间内连接失败
          // 错误信息为“Memory allocation failed”时表示堆空间不足
          // 错误信息为“Generic error”时可能表示 DNS 失败
          char errBuf[100] = "Unknown server error";
          int errNum = sclient.lastError(errBuf, sizeof(errBuf));
          LOG_WRN("Timed out connecting to server: %s, Err: %d, %s", serverName, errNum, errBuf);
        }
      } else LOG_WRN("Insufficient heap %s for %s TLS session", fmtSize(ESP.getFreeHeap()), serverName);
      failCounts[connIdx]++;
    }
  }
  return false;
}

void remoteServerReset() {
  // 重置失败计数
  for (uint8_t i = 0; i < REMFAILCNT; i++) failCounts[i] = 0;
}

/************************** NTP  **************************/

// 时区字符串须来自：https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv
char timezone[FILE_NAME_LEN] = "GMT0";
char ntpServer[MAX_HOST_LEN] = "pool.ntp.org";
uint8_t alarmHour = 1;

time_t getEpoch() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec;
}

void dateFormat(char* inBuff, size_t inBuffLen, bool isFolder) {
  // 根据日期/时间构造文件名
  time_t currEpoch = getEpoch();
  if (isFolder) strftime(inBuff, inBuffLen, "/%Y%m%d", localtime(&currEpoch));
  else strftime(inBuff, inBuffLen, "/%Y%m%d/%Y%m%d_%H%M%S", localtime(&currEpoch));
}

static void showLocalTime(const char* timeSrc) {
  time_t currEpoch = getEpoch();
  char timeFormat[20];
  strftime(timeFormat, sizeof(timeFormat), "%d/%m/%Y %H:%M:%S", localtime(&currEpoch));
  LOG_INF("Got current time from %s: %s with tz: %s", timeSrc, timeFormat, timezone);
  timeSynchronized = true;
}

bool getLocalNTP() {
  // 从 NTP 服务器获取当前时间并应用到 ESP32
  LOG_INF("Using NTP server: %s", ntpServer);
  configTzTime(timezone, ntpServer);
  if (getEpoch() > 10000) {
    showLocalTime("NTP");    
    return true;
  }
  else {
    LOG_WRN("Not yet synced with NTP");
    return false;
  }
}

void syncToBrowser(uint32_t browserUTC) {
  // 若不同步则与浏览器时钟同步
  if (!timeSynchronized) {
    struct timeval tv;
    tv.tv_sec = browserUTC;
    settimeofday(&tv, NULL);
    setenv("TZ", timezone, 1);
    tzset();
    showLocalTime("browser");
  }
}

void formatElapsedTime(char* timeStr, uint32_t timeVal, bool noDays) {
  // 应用已运行的经过时间
  uint32_t secs = timeVal / 1000; // 毫秒转换为秒
  uint32_t mins = secs / 60; // 秒转换为分钟
  uint32_t hours = mins / 60; // 分钟转换为小时
  uint32_t days = hours / 24; // 小时转换为天
  secs = secs - (mins * 60); // 减去已转换为分钟的秒数，使秒数最多显示 59
  mins = mins - (hours * 60); // 减去已转换为小时的分钟数，使分钟最多显示 59
  hours = hours - (days * 24); // 减去已转换为天的小时数，使小时最多显示 23
  if (noDays) sprintf(timeStr, "%02lu:%02lu:%02lu", hours, mins, secs);
  else sprintf(timeStr, "%lu-%02lu:%02lu:%02lu", days, hours, mins, secs);
}

static time_t setAlarm(uint8_t alarmHour) {
  // 根据当前日期时间计算未来闹钟时间
  // 确保已设置相应时区（默认 GMT0）
  time_t currEpoch = getEpoch();
  struct tm* timeinfo = localtime(&currEpoch);
  // 设置下一个指定小时的闹钟日期和时间
  int nextDay = 0; // 先尝试当天，再尝试次日
  do {
    timeinfo->tm_mday += nextDay;
    timeinfo->tm_hour = alarmHour;
    timeinfo->tm_min = 0;
    timeinfo->tm_sec = 0;
    nextDay = 1;
  } while (mktime(timeinfo) < getEpoch());
  char inBuff[30];
  strftime(inBuff, sizeof(inBuff), "%d/%m/%Y %H:%M:%S", timeinfo);
  LOG_INF("Alarm scheduled at %s", inBuff);
  // 返回未来闹钟时间的纪元秒数
  return mktime(timeinfo);
}

bool checkAlarm() {
  // 从 appPing() 调用，检查指定小时的每日闹钟时间是否到达
  static time_t rolloverEpoch = 0;
  if (timeSynchronized && getEpoch() >= rolloverEpoch) {
    // 闹钟时间已到
    rolloverEpoch = setAlarm(alarmHour); // 设置下一个闹钟时间
    return true;
  }
  return false;
}

/********************** 杂项函数 ************************/

bool changeExtension(char* fileName, const char* newExt) {
  // 用提供的扩展名替换原文件扩展名（缓冲区须足够大）
  size_t inNamePtr = strlen(fileName);
  // 查找扩展名前的 '.'
  while (inNamePtr > 0 && fileName[inNamePtr] != '.') inNamePtr--;
  inNamePtr++;
  size_t extLen = strlen(newExt);
  memcpy(fileName + inNamePtr, newExt, extLen);
  fileName[inNamePtr + extLen] = 0;
  return (inNamePtr > 1) ? true : false;
}

void showProgress(const char* marker) {
  // 以点或提供的标记显示进度
  static uint8_t dotCnt = 0;
  logPrint(marker); // 进度标记
  if (++dotCnt >= DOT_MAX) {
    dotCnt = 0;
    logPrint("\n");
  }
}

bool calcProgress(int progressVal, int totalVal, int percentReport, uint8_t &pcProgress) {
  // 计算百分比进度，仅在 percentReport 边界处回传
  uint8_t percentage = (progressVal * 100) / totalVal;
  if (percentage >= pcProgress + percentReport) {
    pcProgress = percentage;
    return true;
  } else return false;
}

bool urlEncode(const char* inVal, char* encoded, size_t maxSize) {
  int encodedLen = 0;
  char hexTable[] = "0123456789ABCDEF";
  while (*inVal) {
    if (isalnum(*inVal) || strchr("$-_.+!*'(),:@~#", *inVal)) *encoded++ = *inVal;
    else {
      encodedLen += 3; 
      if (encodedLen >= maxSize) return false;  // 缓冲区溢出
      *encoded++ = '%';
      *encoded++ = hexTable[(*inVal) >> 4];
      *encoded++ = hexTable[*inVal & 0xf];
    }
    inVal++;
  }
  *encoded = 0;
  return true;
}

void urlDecode(char* inVal) {
  // 替换 URL 编码字符
  std::string decodeVal(inVal); 
  std::string replaceVal = decodeVal;
  std::smatch match; 
  while (regex_search(decodeVal, match, std::regex("(%)([0-9A-Fa-f]{2})"))) {
    std::string s(1, static_cast<char>(std::strtoul(match.str(2).c_str(),nullptr,16))); // 十六进制转字符
    replaceVal = std::regex_replace(replaceVal, std::regex(match.str(0)), s);
    decodeVal = match.suffix().str();
  }
  strcpy(inVal, replaceVal.c_str());
}

void listBuff (const uint8_t* b, size_t len) {
  // 以十六进制输出缓冲区内容，每行 16 字节
  if (!len || !b) LOG_WRN("Nothing to print");
  else {
    for (size_t i = 0; i < len; i += 16) {
      int linelen = (len - i) < 16 ? (len - i) : 16;
      for (size_t k = 0; k < linelen; k++) logPrint(" %02x", b[i+k]);
      puts(" ");
    }
  }
}

size_t isSubArray(uint8_t* haystack, uint8_t* needle, size_t hSize, size_t nSize) {
  // 在被搜索数组 (haystack) 中查找目标子数组 (needle)
  size_t h = 0, n = 0; // 遍历两个数组的指针
  // 同时遍历两个数组
  while (h < hSize && n < nSize) {
    // 若元素匹配，则两个指针均递增
    if (haystack[h] == needle[n]) {
      h++;
      n++;
      // 若目标子数组已完全遍历
      if (n == nSize) return h; // 目标子数组末尾位置
    } else {
      // 否则递增 h 并重置 n
      h = h - n + 1;
      n = 0;
    }
  }
  return 0; // 未找到
}

void removeChar(char* s, char c) {
  // 从字符串中移除指定字符
  int writer = 0, reader = 0;
  while (s[reader]) {
    if (s[reader] != c) s[writer++] = s[reader];
    reader++;       
  }
  s[writer] = 0;
}

void replaceChar(char* s, char c, char r) {
  // 替换字符串中的指定字符
  int reader = 0;
  while (s[reader]) {
    if (s[reader] == c) s[reader] = r;
    reader++;       
  }
}

char* fmtSize (uint64_t sizeVal) {
  // 按数量级格式化大小
  // 每个格式字符串仅可调用一次
  static char returnStr[20];
  if (sizeVal < 50 * 1024) sprintf(returnStr, "%llu bytes", sizeVal);
  else if (sizeVal < ONEMEG) sprintf(returnStr, "%lluKB", sizeVal / 1024);
  else if (sizeVal < ONEMEG * 1024) sprintf(returnStr, "%0.1fMB", (double)(sizeVal) / ONEMEG);
  else sprintf(returnStr, "%0.1fGB", (double)(sizeVal) / (ONEMEG * 1024));
  return returnStr;
}

char* trim(char* str) {
  // 去除字符串首尾空白
    char* start = str;
    char* end;
    // 去除首部空白
    while (*start && isspace((unsigned char)*start)) start++;
    if (*start == '\0') {
        *str = '\0';
        return str;
    }
    // 去除尾部空白
    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    // 若需要则将字符串移回原缓冲区
    if (start != str) memmove(str, start, end - start + 2);
    return str;
}

char* toCase(char *s, bool toLower) {
  // 将提供的字符串转换为小写或大写
  for (char *p = s; *p; ++p) *p = toLower ? (char)tolower((unsigned char)*p) : (char)toupper((unsigned char)*p);
  return s;
}

/********************** 模拟量函数 ************************/

uint16_t smoothAnalog(int analogPin, int samples) {
  // 获取模拟引脚平均值
  uint32_t level = 0; 
  if (analogPin > 0) {
    for (int j = 0; j < samples; j++) level += analogRead(analogPin); 
    level /= samples;
  }
  return level;
}

void setupADC() {
  analogSetAttenuation(ADC_ATTEN);
  analogReadResolution(ADC_BITS);
}

float smoothSensor(float latestVal, float smoothedVal, float alpha) {
  // 简单指数移动平均滤波器
  // alpha 介于 0.0（最大平滑）与 1.0（无平滑）之间
  return (latestVal * alpha) + smoothedVal * (1.0 - alpha);
}

// 板载芯片温度传感器
#if CONFIG_IDF_TARGET_ESP32
extern "C" {
// 使用内部片上温度传感器（若存在）
uint8_t temprature_sens_read(); // 原文如此（API 名称拼写错误）
}
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3
#include "driver/temperature_sensor.h"
static temperature_sensor_handle_t temp_sensor = NULL;
#endif

static void prepInternalTemp() {
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3
  // 设置内部传感器
  temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
  temperature_sensor_install(&temp_sensor_config, &temp_sensor);
  temperature_sensor_enable(temp_sensor);
#endif
}

float readInternalTemp() {
  float intTemp = NULL_TEMP;
#if CONFIG_IDF_TARGET_ESP32
  // 将片上原始温度（华氏度）转换为摄氏度
  intTemp = (temprature_sens_read() - 32) / 1.8;  // 值为 55 表示不存在
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3
    temperature_sensor_get_celsius(temp_sensor, &intTemp); 
#endif
  return intTemp;
}

/*********************** 远程日志 ***********************/
/*
 * 用户界面中的日志模式选择：
 * false：仅记录到串口 / Web 监视器
 * true ：同时保存日志到 SD 卡。下载生成的日志可：
 *  - 在浏览器中按「显示日志」按钮查看日志
 * - 在日志 Web 页面按「清除日志」链接清除日志文件内容
 */
 
#define MAX_OUT 200
static va_list arglist;
static char fmtBuf[MAX_OUT];
static char outBuf[MAX_OUT];
TaskHandle_t logHandle = NULL;
static SemaphoreHandle_t logSemaphore = NULL;
static SemaphoreHandle_t logMutex = NULL;
static int logWait = 100; // 毫秒
bool useLogColors = false;  // 为 true 时为日志消息着色（例如使用 idf.py 时，Arduino 则否）

#define WRITE_CACHE_CYCLE 5

bool sdLog = false; // 记录到 SD 卡
int logType = 0; // 显示哪类日志内容（0：RAM，1：SD）
static FILE* log_remote_fp = NULL;
static uint32_t counter_write = 0;

// 基于 RAM 的日志，存储在 RTC 慢速内存中（不可初始化）
RTC_NOINIT_ATTR char messageLog[RAM_LOG_LEN];
RTC_NOINIT_ATTR uint16_t mlogEnd;
static RTC_NOINIT_ATTR uint32_t backtrace[60]; // 回溯地址数组
static RTC_NOINIT_ATTR size_t btLen;
static RTC_NOINIT_ATTR char btReason[50];
static RTC_NOINIT_ATTR int btCore;
static RTC_NOINIT_ATTR uint32_t haveTrace;

static void ramLogClear() {
  mlogEnd = 0;
  memset(messageLog, 0, RAM_LOG_LEN);
}
  
static void ramLogStore(size_t msgLen) {
  // 将日志条目保存到 RAM 缓冲区
  if (mlogEnd + msgLen >= RAM_LOG_LEN) {
    // 日志需在循环缓冲区中回绕
    uint16_t firstPart = RAM_LOG_LEN - mlogEnd;
    memcpy(messageLog + mlogEnd, outBuf, firstPart);
    msgLen -= firstPart;
    memcpy(messageLog, outBuf + firstPart, msgLen);
    mlogEnd = 0;
  } else memcpy(messageLog + mlogEnd, outBuf, msgLen);
  mlogEnd += msgLen;
}

void flush_log(bool andClose) {
  if (log_remote_fp != NULL) {
    fsync(fileno(log_remote_fp));  
    fflush(log_remote_fp);
    if (andClose) {
      LOG_INF("Closed SD file for logging");
      fclose(log_remote_fp);
      log_remote_fp = NULL;
    } else delay(1000);
  }  
}

static void remote_log_init_SD() {
#if !CONFIG_IDF_TARGET_ESP32C3
  STORAGE.mkdir(DATA_DIR);
  // 打开远程文件
  log_remote_fp = NULL;
  log_remote_fp = fopen("/sdcard" LOG_FILE_PATH, "a");
  if (log_remote_fp == NULL) {LOG_WRN("Failed to open SD log file %s", LOG_FILE_PATH);}
  else {
    logLine();
    LOG_INF("Opened SD file for logging");
  }
#endif
}

void reset_log() {
  if (logType == 0) ramLogClear();
  if (logType == 1) {
    if (log_remote_fp != NULL) flush_log(true); // 关闭日志文件
    STORAGE.remove(LOG_FILE_PATH);
    remote_log_init_SD();
  }
  LOG_INF("Cleared %s log file", logType == 0 ? "RAM" : "SD"); 
}

void remote_log_init() {
  // 设置所需的日志模式
  if (sdLog) {
    flush_log(false);
    remote_log_init_SD(); // 将日志存储到 SD 卡
  } else flush_log(true);
}

static void logTask(void *arg) {
  // 独立任务以减少其他任务的栈大小
  while(true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vsnprintf(outBuf, MAX_OUT, fmtBuf, arglist);
    va_end(arglist);
    xSemaphoreGive(logSemaphore);
  }
}

void logPrint(const char *format, ...) {
  // 由 logTask 格式化消息，再按需输出
  if (logMutex == NULL) logSetup();
  if (xSemaphoreTake(logMutex, pdMS_TO_TICKS(logWait)) == pdTRUE) {
    strncpy(fmtBuf, format, MAX_OUT);
    va_start(arglist, format); 
    vTaskPrioritySet(logHandle, uxTaskPriorityGet(NULL) + 1);
    xTaskNotifyGive(logHandle);
    outBuf[MAX_OUT - 2] = '\n'; 
    outBuf[MAX_OUT - 1] = 0; // 确保始终有结尾换行
    xSemaphoreTake(logSemaphore, portMAX_DELAY); // 等待 logTask 完成

    // 将消息输出到各接收端
    size_t msgLen = strlen(outBuf);
    if (msgLen > 1) {
#ifdef AUXILIARY
      sendSSE("log", outBuf);
#else
      wsAsyncSendText(outBuf); // 通过 WebSocket 输出到浏览器
#endif
      if (outBuf[msgLen - 2] == '~') outBuf[msgLen - 2] = ' '; // 若存在则移除 '~'
    }
    if (monitorOpen) Serial.print(outBuf); // 若已连接则输出到监视器控制台
    else delay(10); // 为其他任务留出时间
    if (msgLen > 1) {
      ramLogStore(msgLen); // 存储到 RTC RAM
      if (sdLog) {
        if (log_remote_fp != NULL) {
          // 若文件已打开则输出到 SD
          fwrite(outBuf, sizeof(char), msgLen, log_remote_fp); // 日志文件
          // 定期同步到 SD
          if (counter_write++ % WRITE_CACHE_CYCLE == 0) fsync(fileno(log_remote_fp));
        } 
      }
    }
    xSemaphoreGive(logMutex);
  } 
}

void logLine() {
  logPrint(" \n");
}

int vprintfRedirect(const char* format, va_list args) {
  // 将 esp_log() 输出格式化为 logPrint()
  char buffer[256];
  int len = vsnprintf(buffer, sizeof(buffer), format, args);
  logPrint("%s", buffer);
  return len;
}

void logSetup() {
  // 准备日志环境
  if (logMutex == NULL) {
    set_arduino_panic_handler(appPanicHandler, NULL);
#if CONFIG_IDF_TARGET_ESP32S3
   HEAP_MEM = psramFound() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
#else
    // 原版 ESP32 任务栈必须使用内部内存
    HEAP_MEM = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    Serial.begin(115200);
    Serial.setDebugOutput(DBG_ON);
    printf("\n\n");
    if (DEBUG_MEM) printf("init > Free: heap %lu\n", ESP.getFreeHeap()); 
    (DBG_ON) ? esp_log_level_set("*", DBG_LVL) : esp_log_level_set("*", ESP_LOG_NONE); // 抑制 esp 日志消息
    esp_log_set_vprintf(vprintfRedirect); // 将 esp_log 输出重定向到应用日志
    if (crashLoop == MAGIC_NUM) snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Crash loop detected, check log %s", (brownoutStatus == 'B' || brownoutStatus == 'R') ? "(brownout)" : " ");
    crashLoop = MAGIC_NUM;
    logSemaphore = xSemaphoreCreateBinary(); // 标志日志消息已格式化
    logMutex = xSemaphoreCreateMutex(); // 控制对日志格式化器的访问
    xSemaphoreGive(logSemaphore);
    xSemaphoreGive(logMutex);
    xTaskCreateWithCaps(logTask, "logTask", LOG_STACK_SIZE, NULL, LOG_PRI, &logHandle, HEAP_MEM);
    if (mlogEnd >= RAM_LOG_LEN) ramLogClear(); // 初始化
    logPrint("\n\n=============== %s %s ===============\n", APP_NAME, APP_VER);
    LOG_INF("Setup RAM based log, size %u, starting from %u", RAM_LOG_LEN, mlogEnd);
    initBrownout();
    prepInternalTemp();
    boardInfo();
    LOG_INF("Compiled with arduino-esp32 v%s", ESP_ARDUINO_VERSION_STR);
    wakeupResetReason();
     if (jsonBuff == NULL) jsonBuff = psramFound() ? (char*)ps_malloc(JSON_BUFF_LEN) : (char*)malloc(JSON_BUFF_LEN); 
    if (!DBG_ON) esp_log_level_set("*", ESP_LOG_ERROR); // 初始化期间显示 ESP_LOG_ERROR 消息
    debugMemory("logSetup");
  }
}

void formatHex(const char* inData, size_t inLen) {
  // 将数据格式化为十六进制字节输出
  char formatted[(inLen * 3) + 1];
  for (int i=0; i<inLen; i++) sprintf(formatted + (i*3), "%02x ", inData[i]);
  formatted[(inLen * 3)] = 0; // 终止符
  LOG_INF("Hex: %s", formatted);
}

const char* espErrMsg(esp_err_t errCode) {
  // 将 esp 错误码转换为文本
  // https://github.com/espressif/esp-idf/blob/master/components/esp_common/include/esp_err.h
  static char errText[100];
  esp_err_to_name_r(errCode, errText, 100);
  return errText;
}

/****************** Base64 编码 ******************/

#define BASE64 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

const uint8_t* encode64chunk(const uint8_t* inp, int rem) {
  // 接收 3 字节输入缓冲区并返回 4 字节 base64 缓冲区
  rem = 3 - rem; // 最后一块可能少于 3 字节
  uint32_t buff = 0; // 以 24 位移位保存 3 字节
  static uint8_t b64[4];
  // 将输入移入缓冲区
  for (int i = 0; i < 3 - rem; i++) buff |= inp[i] << (8*(2-i)); 
  // 从缓冲区移出 6 位输出并编码
  for (int i = 0; i < 4 - rem; i++) b64[i] = BASE64[buff >> (6*(3-i)) & 0x3F]; 
  // 最后一块少于 3 字节时的填充
  for (int i = 0; i < rem; i++) b64[3-i] = '='; 
  return b64;
}

const char* encode64(const char* inp) {
  // 辅助函数：对最长 90 字符的字符串进行 base64 编码
  static char encoded[121]; // 4/3 扩展空间 + 终止符
  encoded[0] = 0;
  int len = strlen(inp);
  if (len > 90) {
    LOG_WRN("Input string too long: %u chars", len);
    len = 90;
  }
  for (int i = 0; i < len; i += 3) 
    strncat(encoded, (char*)encode64chunk((uint8_t*)inp + i, min(len - i, 3)), 4);
  return encoded;
}


/************** 任务监控 ***************/

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 1, 0)

static const char* getTaskStateString(eTaskState state) {
  // 任务状态字符串
  switch (state) { 
    case eRunning: return "Running"; 
    case eReady: return "Ready"; 
    case eBlocked: return "Blocked"; 
    case eSuspended: return "Suspended"; 
    case eDeleted: return "Deleted"; 
    case eInvalid: return "Invalid"; 
    default: return "Unknown";
  }
}

static void statsTask(void *arg) { 
  // 定期输出实时任务统计
  #define STATS_TASK_PRIO     10
  #define STATS_INTERVAL      30000 // 毫秒
  #define ARRAY_SIZE_OFFSET   40   // 若出现 ESP_ERR_INVALID_SIZE 则增大此值

  bool onceOnly = *(bool*)arg; 
  esp_err_t ret = ESP_OK;  
  TaskStatus_t *statsArray = NULL;
  UBaseType_t statsArraySize;
  static configRUN_TIME_COUNTER_TYPE prevRunCounter = 0;
  configRUN_TIME_COUNTER_TYPE runCounter;
  
  do {
    delay(STATS_INTERVAL);

    do { // 用于 break 的假循环
      // 分配数组以存储当前任务状态
      statsArraySize = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
      statsArray = (TaskStatus_t *)malloc(sizeof(TaskStatus_t) * statsArraySize);
      if (statsArray == NULL) {
        ret = ESP_ERR_NO_MEM;
        break;
      }

      // 获取当前任务状态
      statsArraySize = uxTaskGetSystemState(statsArray, statsArraySize, &runCounter);
      if (statsArraySize == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        break;
      }

      // 以运行时统计时钟周期为单位计算总运行时间
      if (runCounter - prevRunCounter == 0) {
        ret = ESP_ERR_INVALID_STATE;
        break;
      }

      logPrint("\nTask stats interval %lus on %u cores\n", STATS_INTERVAL / 1000, CONFIG_FREERTOS_NUMBER_OF_CORES);
      logPrint("\n| %-16s | %-10s | %-3s | %-4s | %-6s |\n", "Task name", "State", "Pri", "Core", "Core%");
      logPrint("|------------------|------------|-----|------|--------|\n"); 
      // 将起始数组中各任务与结束数组中任务匹配
      for (int i = 0; i < statsArraySize; i++) {
        float percentage_time = ((float)statsArray[i].ulRunTimeCounter * 100.0) / runCounter;
        UBaseType_t coreId = statsArray[i].xCoreID;
        logPrint("| %-16s | %-10s | %3u | %4c | %5.1f%% |\n", 
          statsArray[i].pcTaskName, getTaskStateString(statsArray[i].eCurrentState), (int)statsArray[i].uxCurrentPriority, coreId == tskNO_AFFINITY ? '*' : '0' + (int)coreId, percentage_time);
      }
      logPrint("|------------------|------------|-----|------|--------|\n"); 
    } while (false);

    prevRunCounter = runCounter;
    free(statsArray);
    if (ret != ESP_OK) LOG_WRN("Failed to start task monitoring %s", espErrMsg(ret));
  } while (!onceOnly);
  vTaskDelete(NULL);
}

void runTaskStats(bool _onceOnly) {
  // 调用任务统计监控
  static bool onceOnly = _onceOnly;
  // 等待另一核心完成初始化
  vTaskDelay(pdMS_TO_TICKS(100));
  // 创建并启动统计任务
  xTaskCreatePinnedToCore(statsTask, "statsTask", 4096, &onceOnly, STATS_TASK_PRIO, NULL, tskNO_AFFINITY);
}
#endif

void checkMemory(const char* source) {
  LOG_INF("%s Free: heap %u, block: %u, min: %u, pSRAM %u", strlen(source) ? source : "Setup", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap(), ESP.getFreePsram());
  if (ESP.getFreeHeap() < WARN_HEAP) LOG_WRN("Free heap only %u, min %u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  if (ESP.getMaxAllocHeap() < WARN_ALLOC) LOG_WRN("Max allocatable heap block is only %u", ESP.getMaxAllocHeap());
  if (!strlen(source) && DEBUG_MEM) runTaskStats();
}

uint32_t checkStackUse(TaskHandle_t thisTask, int taskIdx) {
  // 获取任务启动以来的最小剩余栈大小
  // taskIdx 用于索引 minStack[] 数组
  static uint32_t minStack[20]; 
  uint32_t freeStack = 0;
  if (thisTask != NULL) {
    freeStack = (uint32_t)uxTaskGetStackHighWaterMark(thisTask);
    if (!minStack[taskIdx]) {
      minStack[taskIdx] = freeStack; // 初始化
      LOG_INF("Task %s on core %d, initial stack space %u", pcTaskGetTaskName(thisTask), xPortGetCoreID(), freeStack);
    }
    if (freeStack < minStack[taskIdx]) {
      minStack[taskIdx] = freeStack;
      if (freeStack < MIN_STACK_FREE) LOG_WRN("Task %s on core %d, stack space only: %u", pcTaskGetTaskName(thisTask), xPortGetCoreID(), freeStack);
      else LOG_INF("Task %s on core %d, stack space reduced to %u", pcTaskGetTaskName(thisTask), xPortGetCoreID(), freeStack);
    }
  }
  return freeStack;
}

void debugMemory(const char* caller) {
  if (DEBUG_MEM) {
    logPrint("%s > Free: heap %u, block: %u, min: %u, pSRAM %u\n", caller, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap(), ESP.getFreePsram());
    delay(FLUSH_DELAY);
  }
}

/****************** 设备休眠（浅睡/深睡）、看门狗、异常 ******************/

#include <esp_wifi.h>
#include <driver/gpio.h>

void doRestart(const char* restartStr) {
  LOG_ALT("Controlled restart: %s", restartStr);
#ifdef ISCAM
  appShutdown();
#endif
#if INCLUDE_MQTT
  if (mqtt_active) stopMqttClient();
#endif  
  resetCrashLoop();
  flush_log(true);
  delay(2000);
  ESP.restart();
}

static esp_sleep_wakeup_cause_t printWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0 : LOG_INF("Wakeup by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : LOG_INF("Wakeup by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER : LOG_INF("Wakeup by internal timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : LOG_INF("Wakeup by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : LOG_INF("Wakeup by ULP program"); break;
    case ESP_SLEEP_WAKEUP_GPIO: LOG_INF("Wakeup by GPIO"); break;    
    case ESP_SLEEP_WAKEUP_UART: LOG_INF("Wakeup by UART"); break; 
    default : LOG_INF("Wakeup by reset"); break;
  }
  return wakeup_reason;
}

void appPanicHandler(arduino_panic_info_t *info, void *arg) {
  // 保存崩溃回溯并延迟重启以避免反复重启
  // https://github.com/espressif/arduino-esp32/blob/master/cores/esp32/esp32-hal-misc.c
  strncpy(btReason, info->reason, sizeof(btReason) - 1);
  btCore = info->core;
  btLen = info->backtrace_len;
  for (int i = 0; i < info->backtrace_len; i++) backtrace[i] = info->backtrace[i];
  haveTrace = MAGIC_NUM; // 标志回溯可用
  esp_rom_delay_us(PANIC_DELAY * 1000 * 1000);
}

static void showBacktrace() {
  // 崩溃后显示回溯
  if (haveTrace == MAGIC_NUM) {
    haveTrace = 0;
    char bt[220];
    sprintf(bt, "%s on core %d", btReason, btCore);
    LOG_WRN("%s", bt);
    bt[0] = 0;
    for (int i = 0; i < btLen; i++) { 
      snprintf(bt + strlen(bt), sizeof(bt) - strlen(bt) - 11, "0x%08x ", (unsigned int)backtrace[i]); // 11 为每条新回溯十六进制的长度
    }
    LOG_WRN("Paste backtrace below into Arduino Exception Decoder:\n");
    logPrint("Backtrace: %s\n\n", bt);
  }
}

static esp_reset_reason_t printResetReason() {
  esp_reset_reason_t bootReason = esp_reset_reason();
  switch (bootReason) {
    case ESP_RST_UNKNOWN: LOG_INF("Reset for unknown reason"); break;
    case ESP_RST_POWERON: {
      LOG_INF("Power on reset");
      brownoutStatus = 0;
      messageLog[0] = 0;
      break;
    }
    case ESP_RST_EXT: LOG_INF("Reset from external pin"); break;
    case ESP_RST_SW: LOG_INF("Software reset via esp_restart"); break;
    case ESP_RST_PANIC: LOG_INF("Software reset due to exception/panic"); break;
    case ESP_RST_INT_WDT: LOG_INF("Reset due to interrupt watchdog"); break;
    case ESP_RST_TASK_WDT: LOG_INF("Reset due to task watchdog"); break;
    case ESP_RST_WDT: LOG_INF("Reset due to other watchdogs"); break;
    case ESP_RST_DEEPSLEEP: LOG_INF("Reset after exiting deep sleep mode"); break;
    case ESP_RST_BROWNOUT: LOG_INF("Software reset due to brownout"); break;
    case ESP_RST_SDIO: LOG_INF("Reset over SDIO"); break;
    default: LOG_WRN("Unhandled reset reason"); break;
  }
  showBacktrace();
  return bootReason;
}

esp_sleep_wakeup_cause_t wakeupResetReason() {
  printResetReason();
  esp_sleep_wakeup_cause_t wakeupReason = printWakeupReason();
  return wakeupReason;
}

void goToSleep(bool deepSleep) {
#if !CONFIG_IDF_TARGET_ESP32C3
  // 深睡时通过复位重启
  // 浅睡时通过继续执行本函数重启
  LOG_INF("Going into %s sleep", deepSleep ? "deep" : "light");
  delay(100);
  if (deepSleep) { 
    if (wakePin >= 0) {
      // 引脚低电平 (0) 或高电平 (1) 唤醒
      // 须为 RTC 引脚并支持输入上拉/下拉
      pinMode(wakePin, wakeLevel == 0 ? INPUT_PULLUP : INPUT_PULLDOWN);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)wakePin, wakeLevel);
    }
    esp_deep_sleep_start();
  } else {
    // 浅睡
    esp_wifi_stop();
    // 选定引脚唤醒
    if (wakePin >= 0) gpio_wakeup_enable((gpio_num_t)wakePin, wakeLevel == 0 ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL); 
    esp_light_sleep_start();
  }
  // 浅睡在此重启
  LOG_INF("Light sleep wakeup");
  esp_wifi_start();
#else
  LOG_WRN("This function not compatible with ESP32-C3");
#endif
}

// 捕获因欠压导致的软件复位
//https://github.com/espressif/esp-idf/blob/master/components/esp_system/port/brownout.c

#include "esp_private/system_internal.h"
#include "esp_private/rtc_ctrl.h"
#include "hal/brownout_ll.h"

#include "soc/rtc_periph.h"
#include "hal/brownout_hal.h"

#define BROWNOUT_DET_LVL 7

IRAM_ATTR static void notifyBrownout(void *arg) {
  esp_cpu_stall(!xPortGetCoreID());  // 停止另一核心
  esp_reset_reason_set_hint(ESP_RST_BROWNOUT);
  brownoutStatus = 'B';
  esp_restart_noos(); // 强制重启
}

static void initBrownout(void) {
  // 欠压警告仅输出一次，以防启动循环
  if (brownoutStatus == 'R') LOG_WRN("Brownout warning previously notified");
  else if (brownoutStatus == 'B') {
    LOG_WRN("Brownout occurred due to inadequate power supply");
    brownoutStatus = 'R';
  } else {
    brownout_hal_config_t cfg = {
      .threshold = BROWNOUT_DET_LVL,
      .enabled = true,
      .reset_enabled = false,
      .flash_power_down = true,
      .rf_power_down = true,
    };
    brownout_hal_config(&cfg);
    brownout_ll_intr_clear();
    rtc_isr_register(notifyBrownout, NULL, RTC_CNTL_BROWN_OUT_INT_ENA_M, RTC_INTR_FLAG_IRAM);
    brownout_ll_intr_enable(true);
    brownoutStatus = 0; 
  }
}

void forceCrash() {
  // 为测试目的强制崩溃
  delay(5000);
#pragma GCC diagnostic ignored "-Wdiv-by-zero"
  printf("%u\n", 1/0);
#pragma GCC diagnostic warning "-Wdiv-by-zero"
}

/************************ 板级信息 **************************/

static void boardInfo() {
  LOG_INF("Chip %s, %d cores @ %dMhz, rev %d", ESP.getChipModel(), ESP.getChipCores(), ESP.getCpuFreqMHz(), ESP.getChipRevision() / 100);
  FlashMode_t ideMode = ESP.getFlashChipMode();
  LOG_INF("Flash %s, mode %s @ %dMhz", fmtSize(ESP.getFlashChipSize()), (ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT" : ideMode == FM_DIO ? "DIO" : ideMode == FM_DOUT ? "DOUT" : "UNKNOWN"), ESP.getFlashChipSpeed() / OneMHz);

#if defined(CONFIG_SPIRAM_MODE_OCT)
  const char* psramMode = "OPI";
#else 
  const char* psramMode = "QSPI";
#endif
  char memInfo[100] = "none";
#if !CONFIG_IDF_TARGET_ESP32C3
  if (psramFound()) sprintf(memInfo, "%s, mode %s @ %dMhz", fmtSize(ESP.getPsramSize()), psramMode, CONFIG_SPIRAM_SPEED);
#endif
  LOG_INF("PSRAM %s", memInfo);
}

#include "esp32-hal-periman.h"
static void printGpioInfo() {
  // 来自 https://github.com/espressif/arduino-esp32/blob/master/cores/esp32/chip-debug-report.cpp
  logPrint("Assigned GPIO Info:\n");
  for (uint8_t i = 0; i < SOC_GPIO_PIN_COUNT; i++) {
    if (!perimanPinIsValid(i)) continue;  // 无效引脚
    peripheral_bus_type_t type = perimanGetPinBusType(i);
    if (type == ESP32_BUS_TYPE_INIT) continue;  // 未使用引脚

    char gpioInf[100];
    char* p = gpioInf;
#if defined(BOARD_HAS_PIN_REMAP)
    int dpin = gpioNumberToDigitalPin(i);
    if (dpin < 0) continue;  // 引脚未导出
    else p+= sprintf(p, "  D%-3d|%4u : ", dpin, i);
#else
    p+= sprintf(p, "  %4u : ", i);
#endif
    const char *extra_type = perimanGetPinBusExtraType(i);
    if (extra_type) p+= sprintf(p, "%s", extra_type);
    else p+= sprintf(p, "%s", perimanGetTypeName(type));
    int8_t bus_number = perimanGetPinBusNum(i);
    if (bus_number != -1) p+= sprintf(p, "[%u]", bus_number);

    int8_t bus_channel = perimanGetPinBusChannel(i);
    if (bus_channel != -1) p+= sprintf(p, "[%u]", bus_channel);
    *p = 0;
    logPrint("%s\n", gpioInf);
  }
}

// 显示分区映射
const char* partitionTypeToStr(uint8_t type) {
  // 将类型映射为字符串
  switch (type) {
    case ESP_PARTITION_TYPE_APP: return "APP";
    case ESP_PARTITION_TYPE_DATA: return "DATA";
    default: return "UNKNOWN";
  }
}

const char* partitionSubtypeToStr(uint8_t type, uint8_t subtype) {
  // 根据类型将子类型映射为字符串
  if (type == ESP_PARTITION_TYPE_APP) {
    switch (subtype) {
      case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "Factory";
      case ESP_PARTITION_SUBTYPE_APP_OTA_0: return "OTA_0";
      case ESP_PARTITION_SUBTYPE_APP_OTA_1: return "OTA_1";
      case ESP_PARTITION_SUBTYPE_APP_OTA_2: return "OTA_2";
      case ESP_PARTITION_SUBTYPE_APP_OTA_3: return "OTA_3";
      case ESP_PARTITION_SUBTYPE_APP_OTA_4: return "OTA_4";
      case ESP_PARTITION_SUBTYPE_APP_OTA_5: return "OTA_5";
      default: return "App_Other";
    }
  } else if (type == ESP_PARTITION_TYPE_DATA) {
    switch (subtype) {
      case ESP_PARTITION_SUBTYPE_DATA_OTA: return "OTA_Data";
      case ESP_PARTITION_SUBTYPE_DATA_PHY: return "PHY";
      case ESP_PARTITION_SUBTYPE_DATA_NVS: return "NVS";
      case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "NVS_Keys";
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS: return "SPIFFS";
      case ESP_PARTITION_SUBTYPE_DATA_FAT: return "FAT";
      default: return "Data_Other";
    }
  }
  return "Unknown";
}

static void printPartitionTable() {
  // 打印所有分区
  logPrint("%-12s %-6s %-12s %-10s %-12s %-10s", "Partition", "Type", "Subtype", "Address", "Size", "Encrypted\n");
  logPrint("-----------------------------------------------------------------\n");

  // 获取所有分区的迭代器
  esp_partition_iterator_t iter = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

  if (iter == NULL) {
    LOG_ERR("No partitions found");
    return;
  }

  // 遍历所有分区
  do {
    const esp_partition_t* part = esp_partition_get(iter);
    const char* typeStr = partitionTypeToStr(part->type);
    const char* subtypeStr = partitionSubtypeToStr(part->type, part->subtype);
    const char* label = part->label;
    logPrint("%-12s %-6s %-12s 0x%08lX %-12s %-10s\n",
      label, typeStr, subtypeStr, part->address, fmtSize(part->size), part->encrypted ? "Yes" : "No");
    iter = esp_partition_next(iter);
  } while (iter != NULL);

  // 释放迭代器
  esp_partition_iterator_release(iter);
}

void showSys() {
  // 将系统详情输出到 Web 日志
  logLine();
  boardInfo();
  logLine();
  logPrint("%s v%s, arduino-esp32 v%s\n", APP_NAME, APP_VER, ESP_ARDUINO_VERSION_STR);
  logLine();
  printPartitionTable();
  logLine();
  printGpioInfo();
  logLine();
  runTaskStats(true);
  logLine();
  //gpio_dump_io_configuration(stdout, SOC_GPIO_VALID_GPIO_MASK);
}