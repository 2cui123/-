// 全局通用声明
//
#include "esp_arduino_version.h"

#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 1, 1)
#error Must be compiled with arduino-esp32 core v3.1.1 or higher
#endif

#pragma once

#if __has_include("./devUtilities.cpp") 
#define DEV_ONLY 
#endif
#ifdef DEV_ONLY
// 使用 -Wall -Werror=all -Wextra 编译
#pragma GCC diagnostic error "-Wformat=2"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#pragma GCC diagnostic ignored "-Wformat-y2k"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
//#pragma GCC diagnostic ignored "-Wunused-variable"
//#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
//#pragma GCC diagnostic ignored "-Wignored-qualifiers"
//#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wvolatile"
#endif

/******************** 库 *******************/

#include "Arduino.h"
#include <ESPmDNS.h> 
#include "lwip/sockets.h"
#include <vector>
#include "ping/ping_sock.h"
#include <Preferences.h>
#include <regex>
#if (!CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32S2)
#include <SD_MMC.h>
#endif
#include <LittleFS.h>
#include <sstream>
#include <Update.h>
#include <WiFi.h>
#include <ETH.h>
#ifdef APP_BT_ENABLED
#include <esp_bt.h>
#endif
#include <HTTPClient.h>
#include <NetworkClient.h> 
#include <NetworkClientSecure.h> 
#include <esp_http_server.h>
#include <esp_https_server.h>

// ADC（模数转换）
#define ADC_ATTEN ADC_11db
#define ADC_SAMPLES 16
#if CONFIG_IDF_TARGET_ESP32S3
#define ADC_BITS 13
#define MAX_ADC 8191 // 给定分辨率下的最大 ADC 值
#else
#define ADC_BITS 12
#define MAX_ADC 4095 // 给定分辨率下的最大 ADC 值
#endif
#define CENTER_ADC (MAX_ADC / 2) 

// 数据文件夹定义
#define DATA_DIR "/data"
#define HTML_EXT ".htm"
#define TEXT_EXT ".txt"
#define JS_EXT ".js"
#define CSS_EXT ".css"
#define ICO_EXT ".ico"
#define SVG_EXT ".svg"
#define JPG_EXT ".jpg"
#define CONFIG_FILE_PATH DATA_DIR "/configs" TEXT_EXT
#define LOG_FILE_PATH DATA_DIR "/log" TEXT_EXT
#define OTA_FILE_PATH DATA_DIR "/OTA" HTML_EXT
#define COMMON_JS_PATH DATA_DIR "/common" JS_EXT 
#define WEBDAV "/webdav"
#define GITHUB_HOST "raw.githubusercontent.com"
#define FILLSTAR "****************************************************************"
#define DELIM '~'
#define ONEMEG (1024 * 1024)
#define MAX_PWD_LEN 64
#define MAX_HOST_LEN 32
#define MAX_IP_LEN 16
#define BOUNDARY_VAL "123456789000000000000987654321"
#define SF_LEN 128
#define WAV_HDR_LEN 44
#define RAM_LOG_LEN (1024 * 7) // 存储于慢速 RTC RAM 的系统消息日志大小（字节，最大 8KB - 变量）
#define MIN_STACK_FREE 512
#define STARTUP_FAIL "Startup Failure: "
#define MAX_PAYLOAD_LEN 672 // 须大于任意传入 WebSocket 载荷（20ms 音频）
#define NULL_TEMP -127
#define OneMHz 1000000
#define USECS 1000000
#define MAGIC_NUM 987654321
#define MAX_FAIL 5
#define PANIC_DELAY 5 // 崩溃后重启前的秒数

// 全局必需的应用专用函数，位于 appSpecific.cpp
bool appDataFiles();
esp_err_t appSpecificSustainHandler(httpd_req_t* req);
esp_err_t appSpecificWebHandler(httpd_req_t *req, const char* variable, const char* value);
void appSpecificWsBinHandler(uint8_t* wsMsg, size_t wsMsgLen);
void appSpecificWsHandler(const char* wsMsg);
void appSpecificTelegramTask(void* p);
void buildAppJsonString(bool filter);
bool updateAppStatus(const char* variable, const char* value, bool fromUser = true);

// 位于 utils.cpp / utilsFS.cpp / peripherals.cpp 等的全局通用工具函数
void appPanicHandler(arduino_panic_info_t *info, void *arg);
void buildJsonString(uint8_t filter);
bool calcProgress(int progressVal, int totalVal, int percentReport, uint8_t &pcProgress);
bool changeExtension(char* fileName, const char* newExt);
bool checkAlarm();
bool checkAuth(httpd_req_t* req);
bool checkDataFiles();
bool checkFreeStorage();
bool checkI2Cdevice(const char* devName);
void checkMemory(const char* source = "");
uint32_t checkStackUse(TaskHandle_t thisTask, int taskIdx);
void debugMemory(const char* caller);
void dateFormat(char* inBuff, size_t inBuffLen, bool isFolder);
void deleteFolderOrFile(const char* deleteThis);
void devSetup();
void doAppPing();
void doRestart(const char* restartStr);
esp_err_t downloadFile(File& df, httpd_req_t* req);
void emailAlert(const char* _subject, const char* _message);
const char* encode64(const char* inp);
const uint8_t* encode64chunk(const uint8_t* inp, int rem);
const char* espErrMsg(esp_err_t errCode);
void externalAlert(const char* subject, const char* message);
bool externalPeripheral(byte pinNum, uint32_t outputData = 0);
esp_err_t extractHeaderVal(httpd_req_t *req, const char* variable, char* value);
esp_err_t extractQueryKeyVal(httpd_req_t *req, char* variable, char* value);
esp_err_t fileHandler(httpd_req_t* req, bool download = false);
void flush_log(bool andClose = false);
char* fmtSize (uint64_t sizeVal);
void forceCrash();
void formatElapsedTime(char* timeStr, uint32_t timeVal, bool noDays = false);
void formatHex(const char* inData, size_t inLen);
bool formatSDcard();
bool fsStartTransfer(const char* fileFolder);
const char* getEncType(int ssidIndex);
void getExtIP();
time_t getEpoch();
size_t getFreeStorage();
uint32_t getFrequency();
bool getLocalNTP();
float getNTCcelsius(uint16_t resistance, float oldTemp);
void goToSleep(bool deepSleep);
bool handleWebDav(httpd_req_t* rreq);
void initStatus(int cfgGroup, int delayVal);
void killSocket(int skt = -99);
void listBuff(const uint8_t* b, size_t len); 
bool listDir(const char* fname, char* jsonBuff, size_t jsonBuffLen, const char* extension);
void loadCerts();
bool loadConfig();
void logLine();
void logPrint(const char *fmtStr, ...);
void logSetup();
void OTAprereq();
bool parseJson(int rxSize);
bool prepFreq(int maxFreq, int sampleInterval);
bool prepI2C();
void prepPeripherals();
void prepSMTP();
bool prepTelegram();
void prepTemperature();
void prepUpload();
void reloadConfigs();
float readInternalTemp();
float readTemperature(bool isCelsius, bool onlyDS18 = false);
float readVoltage();
void remote_log_init();
void remoteServerClose(NetworkClientSecure& sclient);
bool remoteServerConnect(NetworkClientSecure& sclient, const char* serverName, uint16_t serverPort, const char* serverCert, uint8_t connIdx);
void remoteServerReset();
void removeChar(char* s, char c);
void replaceChar(char* s, char c, char r);
void reset_log();
void resetWatchDog(int wdIndex, uint32_t wdTimeout = 1);
bool retrieveConfigVal(const char* variable, char* value);
void runTaskStats(bool _onceOnly = false);
esp_err_t sendChunks(File df, httpd_req_t *req, bool endChunking = true);
void sendSSE(const char* eventType, const char* eventData);
void setFolderName(const char* fname, char* fileName);
void setPeripheralResponse(const byte pinNum, const uint32_t responseData);
void setupADC();
void showHttpHeaders(httpd_req_t *req);
void showProgress(const char* marker = ".");
void showSys();
uint16_t smoothAnalog(int analogPin, int samples = ADC_SAMPLES);
float smoothSensor(float latestVal, float smoothedVal, float alpha);
void startOTAtask();
void startSecTimer(bool startTimer);
bool startStorage();
bool startWebServer();
void stopPing();
void syncToBrowser(uint32_t browserUTC);
char* toCase(char *s, bool toLower = true);
char* trim(char* str);
bool updateConfigVect(const char* variable, const char* value);
void updateStatus(const char* variable, const char* _value, bool fromUser = true);
esp_err_t uploadHandler(httpd_req_t *req);
void urlDecode(char* inVal);
bool urlEncode(const char* inVal, char* encoded, size_t maxSize);
uint32_t usePeripheral(const byte pinNum, const uint32_t receivedData);
esp_sleep_wakeup_cause_t wakeupResetReason();
void wsAsyncSendBinary(uint8_t* data, size_t len);
bool wsAsyncSendJson(const char* dataType, const char* wsData);
bool wsAsyncSendText(const char* wsData);
// 统一网络辅助函数（WiFi 或以太网）
bool startNetwork(bool firstcall = true);
IPAddress netLocalIP();
IPAddress netGatewayIP();
String netMacAddress();
int netRSSI();
bool netIsConnected();
// mqtt.cpp 相关函数
void startMqttClient();  
void stopMqttClient();  
void mqttPublish(const char* payload);
void mqttPublishPath(const char* suffix, const char* payload, const char *device = "sensor");
// telegram.cpp 相关函数
bool getTgramUpdate(char* response);
bool sendTgramMessage(const char* info, const char* item, const char* parseMode);
bool sendTgramPhoto(uint8_t* photoData, size_t photoSize, const char* caption);
bool sendTgramFile(const char* fileName, const char* contentType, const char* caption);
void tgramAlert(const char* subject, const char* message);
// externalHeartbeat.cpp 相关函数
void sendExternalHeartbeat();

/******************** 全局工具声明 *******************/

extern char AP_SSID[];
extern char AP_Pass[];
extern char AP_ip[];
extern char AP_sn[];
extern char AP_gw[];

extern int netMode; // 0=WiFi, 1=以太网, 2=Eth+AP
extern char hostName[]; // DDNS 主机名
extern char ST_SSID[]; // 路由器 SSID
extern char ST_Pass[]; // 路由器密码
extern bool useHttps;
extern bool useSecure;
extern bool useFtps;

extern char ST_ip[]; // 留空则使用 DHCP
extern char ST_sn[];
extern char ST_gw[];
extern char ST_ns1[];
extern char ST_ns2[];
extern char extIP[];

extern int ethCS;   // 片选
extern int ethInt;  // 中断
extern int ethRst;  // 复位
extern int ethSclk; // SPI 时钟
extern int ethMiso; // SPI 数据引脚
extern int ethMosi; // SPI 数据引脚

extern char Auth_Name[]; 
extern char Auth_Pass[];

extern int responseTimeoutSecs; // 等待远程服务器的超时时间（秒）
extern bool allowAP; // 设为 true 则无法重连 STA（路由器）时允许启动 AP
extern uint32_t wifiTimeoutSecs; // 检查 WiFi 状态的间隔
extern uint8_t percentLoaded;
extern int refreshVal;
extern bool dataFilesChecked;
extern char ipExtAddr[];
extern bool doGetExtIP;
extern bool usePing; // 若与此问题相关则设为 false：https://github.com/s60sc/ESP32-CAM_MJPEG2SD/issues/221
extern uint16_t sustainId;
extern bool heartBeatDone;
extern TaskHandle_t heartBeatHandle;
extern char portFwd[];

// 远程文件服务器
extern char fsServer[];
extern char ftpUser[];
extern uint16_t fsPort;
extern char FS_Pass[];
extern char fsWd[];
extern bool autoUpload;
extern bool deleteAfter;
extern bool fsUse;
extern char inFileName[];

// SMTP 服务器
extern char smtp_login[];
extern char SMTP_Pass[];
extern char smtp_email[];
extern char smtp_server[];
extern uint16_t smtp_port;
extern bool smtpUse; // 是否使用 SMTP
extern int emailCount;

// MQTT 代理
extern bool mqtt_active;
extern char mqtt_broker[];
extern char mqtt_port[];
extern char mqtt_user[];
extern char mqtt_user_Pass[];
extern char mqtt_topic_prefix[];  

// 控制告警发送
extern size_t alertBufferSize;
extern size_t maxAlertBuffSize;
extern byte* alertBuffer;

// Telegram 相关
extern bool tgramUse;
extern char tgramToken[];
extern char tgramChatId[];
extern char tgramHdr[];

// 证书
extern const char* git_rootCACertificate;
extern const char* ftps_rootCACertificate;
extern const char* smtp_rootCACertificate;
extern const char* mqtt_rootCACertificate;
extern const char* telegram_rootCACertificate;
extern const char* hfs_rootCACertificate;
extern char* serverCerts[];

// Web 服务
extern char timezone[];
extern char ntpServer[];
extern const char* setupPage_html;
extern const char* otaPage_html;
extern const char* failPageS_html;
extern const char* failPageE_html;
extern char startupFailure[];

// 应用状态
extern uint8_t alarmHour;
extern char* jsonBuff; 
extern bool dbgVerbose;
extern bool sdLog;
extern int logType;
extern char messageLog[];
extern uint16_t mlogEnd;
extern bool timeSynchronized;
extern bool monitorOpen; 
extern time_t currEpoch;
extern bool RCactive;
extern int wakePin;
extern int wakeLevel;
extern UBaseType_t uxHighWaterMarkArr[];
extern UBaseType_t HEAP_MEM;

// SD 存储
extern int sdMinCardFreeSpace; // 启用 freeSpaceMode 操作前 SD 卡最小剩余空间（MB）
extern int sdFreeSpaceMode; // 0 - 不检查, 1 - 删除最旧目录, 2 - 上传到 FTP 后删除 SD 上的文件夹
extern bool formatIfMountFailed ; // 挂载失败时自动格式化文件系统。设为 false 则不自动格式化。

// I2C 引脚
extern int I2Csda;
extern int I2Cscl;

#define HTTP_METHOD_STRING(method) \
  (method == HTTP_DELETE) ? "DELETE" : \
  (method == HTTP_GET) ? "GET" : \
  (method == HTTP_HEAD) ? "HEAD" : \
  (method == HTTP_POST) ? "POST" : \
  (method == HTTP_PUT) ? "PUT" : \
  (method == HTTP_CONNECT) ? "CONNECT" : \
  (method == HTTP_OPTIONS) ? "OPTIONS" : \
  (method == HTTP_TRACE) ? "TRACE" : \
  (method == HTTP_COPY) ? "COPY" : \
  (method == HTTP_LOCK) ? "LOCK" : \
  (method == HTTP_MKCOL) ? "MKCOL" : \
  (method == HTTP_MOVE) ? "MOVE" : \
  (method == HTTP_PROPFIND) ? "PROPFIND" : \
  (method == HTTP_PROPPATCH) ? "PROPPATCH" : \
  (method == HTTP_SEARCH) ? "SEARCH" : \
  (method == HTTP_UNLOCK) ? "UNLOCK" : \
  (method == HTTP_BIND) ? "BIND" : \
  (method == HTTP_REBIND) ? "REBIND" : \
  (method == HTTP_UNBIND) ? "UNBIND" : \
  (method == HTTP_ACL) ? "ACL" : \
  (method == HTTP_REPORT) ? "REPORT" : \
  (method == HTTP_MKACTIVITY) ? "MKACTIVITY" : \
  (method == HTTP_CHECKOUT) ? "CHECKOUT" : \
  (method == HTTP_MERGE) ? "MERGE" : \
  (method == HTTP_MSEARCH) ? "MSEARCH" : \
  (method == HTTP_NOTIFY) ? "NOTIFY" : \
  (method == HTTP_SUBSCRIBE) ? "SUBSCRIBE" : \
  (method == HTTP_UNSUBSCRIBE) ? "UNSUBSCRIBE" : \
  (method == HTTP_PATCH) ? "PATCH" : \
  (method == HTTP_PURGE) ? "PURGE" : \
  (method == HTTP_MKCALENDAR) ? "MKCALENDAR" : \
  (method == HTTP_LINK) ? "LINK" : \
  (method == HTTP_UNLINK) ? "UNLINK" : \
  "UNKNOWN"

enum RemoteFail {SETASSIST, GETEXTIP, TGRAMCONN, FSFTP, EMAILCONN, EXTERNALHB, BLOCKLIST, REMFAILCNT}; // REMFAILCNT 始终在最后

/*********************** 日志格式 ************************/

//#define USE_LOG_COLORS  // 取消注释以给日志消息上色（例如使用 idf.py 时，Arduino 下不可用）
#ifdef USE_LOG_COLORS 
// ANSI 颜色代码
#define LOG_COLOR_ERR  "\033[0;31m" // 红色
#define LOG_COLOR_WRN  "\033[0;33m" // 黄色
#define LOG_COLOR_VRB  "\033[0;36m" // 青色
#define LOG_COLOR_DBG  "\033[0;34m" // 蓝色
#define LOG_NO_COLOR   
#else
#define LOG_COLOR_ERR
#define LOG_COLOR_WRN
#define LOG_COLOR_VRB
#define LOG_COLOR_DBG
#define LOG_NO_COLOR
#endif 

#define INF_FORMAT(format) "[%s %s] " format "\n", esp_log_system_timestamp(), __FUNCTION__
#define LOG_INF(format, ...) logPrint(INF_FORMAT(format), ##__VA_ARGS__)
#define LOG_ALT(format, ...) logPrint(INF_FORMAT(format "~"), ##__VA_ARGS__)
#define WRN_FORMAT(format) LOG_COLOR_WRN "[%s WARN %s] " format LOG_NO_COLOR "\n", esp_log_system_timestamp(), __FUNCTION__
#define LOG_WRN(format, ...) logPrint(WRN_FORMAT(format "~"), ##__VA_ARGS__)
#define ERR_FORMAT(format) LOG_COLOR_ERR "[%s ERROR @ %s:%u] " format LOG_NO_COLOR "\n", esp_log_system_timestamp(), pathToFileName(__FILE__), __LINE__
#define LOG_ERR(format, ...) logPrint(ERR_FORMAT(format "~"), ##__VA_ARGS__)
#define VRB_FORMAT(format) LOG_COLOR_VRB "[%s VERBOSE @ %s:%u] " format LOG_NO_COLOR "\n", esp_log_system_timestamp(), pathToFileName(__FILE__), __LINE__
#define LOG_VRB(format, ...) if (dbgVerbose) logPrint(VRB_FORMAT(format), ##__VA_ARGS__)
#define DBG_FORMAT(format) LOG_COLOR_DBG "[%s ### DEBUG @ %s:%u] " format LOG_NO_COLOR "\n", esp_log_system_timestamp(), pathToFileName(__FILE__), __LINE__
#define LOG_DBG(format, ...) do { logPrint(DBG_FORMAT(format), ##__VA_ARGS__); delay(FLUSH_DELAY); } while (0)
#define LOG_PRT(buff, bufflen) log_print_buf((const uint8_t*)buff, bufflen)
