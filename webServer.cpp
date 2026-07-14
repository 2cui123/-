// 提供 Web 服务器供用户控制应用
// 
// s60sc 2022 - 2023

#include "appGlobals.h"

#define MAX_HANDLERS 12

char inFileName[IN_FILE_NAME_LEN];
static char variable[FILE_NAME_LEN]; 
static char value[IN_FILE_NAME_LEN]; 
static char retainAction[2];
int refreshVal = 5000; // 毫秒

static httpd_handle_t httpServer = NULL; // Web 服务器端口
static int fdWs = -1; // WebSocket 套接字描述符
static httpd_handle_t sseSocketHD; // SSE 支持
static int sseSocketFD;
bool useHttps = false;
bool useSecure = false;
bool heartBeatDone = false;

static byte* chunk;

esp_err_t sendChunks(File df, httpd_req_t *req, bool endChunking) {   
  // 使用分块编码向浏览器发送大体积内容
  size_t chunksize = 0;
  esp_err_t res = ESP_OK;
  while ((chunksize = df.read(chunk, CHUNKSIZE))) {
    res = httpd_resp_send_chunk(req, (char*)chunk, chunksize);
    if (res != ESP_OK) break;
    // httpd_sess_update_lru_counter(req->handle, httpd_req_to_sockfd(req));
  } 
  if (endChunking) {
    df.close();
    httpd_resp_sendstr_chunk(req, NULL);
  }
  if (res != ESP_OK) LOG_WRN("Failed to send to browser: %s, err %s", inFileName, espErrMsg(res));
  return res;
}

esp_err_t fileHandler(httpd_req_t* req, bool download) {
  // 将文件内容发送给浏览器
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (!strcmp(inFileName, LOG_FILE_PATH)) flush_log(false);
  File df = STORAGE.open(inFileName);
  if (!df) {
    LOG_WRN("File does not exist or cannot be opened: %s", inFileName);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  } 
  if (!df.size()) {
    // 文件为空
    df.close();
    httpd_resp_sendstr(req, NULL);
    return ESP_OK;
  }
  
  // 检查浏览器是否已有该文件版本
  char inVer[10];
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", inVer, sizeof(inVer)) == ESP_OK) {
    if (atoi(inVer) == CFG_VER) {
      // 已有缓存版本，无需重发
      httpd_resp_set_status(req, "304 Not Modified");
      return httpd_resp_send(req, NULL, 0); 
    }
  }
  // 该版本未缓存，发送文件
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  itoa(CFG_VER, inVer, 10);
  httpd_resp_set_hdr(req, "ETag", inVer);
  return (download) ? downloadFile(df, req) : sendChunks(df, req);
}

static void displayLog(httpd_req_t *req) {
  // 将 RAM 日志输出到浏览器
  if (logType == 0) {
    int startPtr, endPtr;
    startPtr = endPtr = mlogEnd;  
    httpd_resp_set_type(req, "text/plain"); 
    
    // 分块输出日志
    do {
      int maxChunk = startPtr < endPtr ? endPtr - startPtr : RAM_LOG_LEN - startPtr;
      size_t chunkSize = std::min(CHUNKSIZE, maxChunk);    
      if (chunkSize > 0) httpd_resp_send_chunk(req, messageLog + startPtr, chunkSize); 
      startPtr += chunkSize;
      if (startPtr >= RAM_LOG_LEN) startPtr = 0;
    } while (startPtr != endPtr);
    httpd_resp_sendstr_chunk(req, NULL);
  } 
}

bool checkAuth(httpd_req_t* req) {
  // 检查是否需要身份验证
  if (strlen(Auth_Name)) {
    // 需要身份验证
    size_t credLen = strlen(Auth_Name) + strlen(Auth_Pass) + 2; // +2 为冒号与结束符
    char credentials[credLen];
    snprintf(credentials, credLen, "%s:%s", Auth_Name, Auth_Pass);
    size_t authLen = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (authLen) {
      // 检查提供的凭据是否有效
      char auth[authLen];
      httpd_req_get_hdr_value_str(req, "Authorization", auth, authLen);
      if (!strstr(auth, encode64(credentials))) authLen = 0; // 凭据无效
    }
    if (!authLen) {
      // 未通过身份验证
      httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic");
      httpd_resp_set_status(req, "401 Unauthorised");
      httpd_resp_sendstr(req, NULL);
      return false;
    }
  }
  return true; // 身份验证通过或无需验证
}

static esp_err_t indexHandler(httpd_req_t* req) {
  strcpy(inFileName, INDEX_PAGE_PATH);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  // 首先检查是否需要报告启动失败
  if (strlen(startupFailure)) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, failPageS_html);
    httpd_resp_sendstr_chunk(req, startupFailure);
    httpd_resp_sendstr_chunk(req, failPageE_html);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
  } 
  // 若未配置且处于 AP 模式，显示 WiFi 向导
  if (!STORAGE.exists(INDEX_PAGE_PATH) && WiFi.status() != WL_CONNECTED) {
    // 打开基础 WiFi 设置页
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, setupPage_html);
  } else if (!checkAuth(req)) return ESP_OK; // 检查是否需要身份验证且已通过

  return fileHandler(req);
}

esp_err_t extractHeaderVal(httpd_req_t *req, const char* variable, char* value) {
  // 检查请求头字段是否存在，若存在则提取值
  esp_err_t res = ESP_FAIL;
  size_t hdrFieldLen = httpd_req_get_hdr_value_len(req, variable);
  if (!hdrFieldLen) return ESP_ERR_INVALID_ARG; // 请求头不存在
  else if (hdrFieldLen >= IN_FILE_NAME_LEN - 1) LOG_WRN("Field %s value too long (%d)", variable, hdrFieldLen);
  else {
    res = httpd_req_get_hdr_value_str(req, variable, value, hdrFieldLen + 1);
    if (res != ESP_OK) LOG_ERR("Value for %s could not be retrieved: %s", variable, espErrMsg(res));
  }
  return res;
}

esp_err_t extractQueryKeyVal(httpd_req_t *req, char* variable, char* value) {
  // 从 URL 查询字符串获取变量与值对
  size_t queryLen = httpd_req_get_url_query_len(req) + 1;
  httpd_req_get_url_query_str(req, variable, queryLen);
  urlDecode(variable);
  // 提取键名
  char* endPtr = strchr(variable, '=');
  if (endPtr != NULL) {
    *endPtr = 0; // 将变量拆成两个字符串，前者为键名
    strcpy(value, variable + strlen(variable) + 1); // 值为字符串后半部分
  } else {
    LOG_ERR("Invalid query string %s", variable);
    httpd_resp_set_status(req, "400 Invalid query string");
    httpd_resp_sendstr(req, NULL);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t webHandler(httpd_req_t* req) {
  // 根据查询字符串中的文件名，向浏览器返回所需网页或组件
  size_t queryLen = httpd_req_get_url_query_len(req) + 1;
  httpd_req_get_url_query_str(req, variable, queryLen);
  urlDecode(variable);

  // 根据文件扩展名决定响应发送前的处理方式
  if (!strcmp(variable, "OTA.htm")) {
    // 请求内置 OTA 页（当 index html 损坏时）
    httpd_resp_set_type(req, "text/html"); 
    return httpd_resp_sendstr(req, otaPage_html);
  } else if (!strcmp(HTML_EXT, variable+(strlen(variable)-strlen(HTML_EXT)))) {
    // 其他 html 文件
    httpd_resp_set_type(req, "text/html");
  } else if (!strcmp(JS_EXT, variable+(strlen(variable)-strlen(JS_EXT)))) {
    // 任意 js 文件
    httpd_resp_set_type(req, "text/javascript");
  } else if (!strcmp(CSS_EXT, variable+(strlen(variable)-strlen(CSS_EXT)))) {
    // 任意 css 文件
    httpd_resp_set_type(req, "text/css");
  } else if (!strcmp(TEXT_EXT, variable+(strlen(variable)-strlen(TEXT_EXT)))) {
    // 任意文本文件
    httpd_resp_set_type(req, "text/plain");
  } else if (!strcmp(ICO_EXT, variable+(strlen(variable)-strlen(ICO_EXT)))) {
    // 任意图标文件
    httpd_resp_set_type(req, "image/x-icon");
  } else if (!strcmp(SVG_EXT, variable+(strlen(variable)-strlen(SVG_EXT)))) {
    // 任意 svg 文件
    httpd_resp_set_type(req, "image/svg+xml");
  } else LOG_WRN("Unknown file type %s", variable);  
  int dlen = snprintf(inFileName, IN_FILE_NAME_LEN - 1, "%s/%s", DATA_DIR, variable);               
  if (dlen >= IN_FILE_NAME_LEN) LOG_WRN("file name truncated");
  return fileHandler(req);
}

static esp_err_t controlHandler(httpd_req_t *req) {
  // 处理来自浏览器的控制查询
  // 从查询字符串获取参数
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (extractQueryKeyVal(req, variable, value) != ESP_OK) return ESP_FAIL;
  if (!strcmp(variable, "displayLog")) displayLog(req);
  else {
    strcpy(value, variable + strlen(variable) + 1); // value 指向字符串后半部分
    if (!strcmp(variable, "reset")) {
      httpd_resp_sendstr(req, NULL); // 阻止浏览器重复发送 reset
      doRestart(value); 
      return ESP_OK;
    }
    if (!strcmp(variable, "startOTA")) snprintf(inFileName, IN_FILE_NAME_LEN - 1, "%s/%s", DATA_DIR, value); 
    else {
      // 若 appSpecificWebHandler() 未处理，则尝试 updateStatus()
      if (appSpecificWebHandler(req, variable, value) == ESP_FAIL) updateStatus(variable, value);
    }
  }
  httpd_resp_sendstr(req, NULL); 
  return ESP_OK;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  uint8_t filter = (uint8_t)httpd_req_get_url_query_len(req); // 过滤器编号为查询字符串长度
  buildJsonString(filter);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, jsonBuff);
  return ESP_OK;
}

bool parseJson(int rxSize) {
  // 处理 jsonBuff 中的 JSON，提取格式正确的扁平 key:value 对
  jsonBuff[rxSize - 1] = ','; // 将末尾 '}' 替换为 ','
  jsonBuff[rxSize] = 0; // 结束符
  char* ptr = jsonBuff + 1; // 跳过开头的 '{'
  size_t itemLen = 0; 
  bool retAction = false;
  do {
    // 依次获取并处理每个 key:value
    char* endItem = strchr(ptr += itemLen, ':');
    itemLen = endItem - ptr;
    memcpy(variable, ptr, itemLen);
    variable[itemLen] = 0;
    removeChar(variable, '"');
    ptr++;
    endItem = strchr(ptr += itemLen, ',');
    itemLen = endItem - ptr;
    memcpy(value, ptr, itemLen);
    value[itemLen] = 0;
    removeChar(value, '"');
    ptr++;
    if (!strcmp(variable, "action")) {
      strcpy(retainAction, value);
      retAction = true;
    } else updateStatus(variable, value);
  } while (ptr + itemLen - jsonBuff < rxSize);
  return retAction;
}

static esp_err_t sseHandler(httpd_req_t *req) {
  // 启用 Server Sent Events
  const char* sseHeader = "HTTP/1.1 200 OK\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: keep-alive\r\n"
                          "Content-Type: text/event-stream\r\n\r\n";
  sseSocketHD = req->handle;
  sseSocketFD = httpd_req_to_sockfd(req);
  httpd_socket_send(sseSocketHD, sseSocketFD, sseHeader, strlen(sseHeader), 0); 
  sendSSE("open", "opened");
  return ESP_OK;
}

#define SSESEP "\r\n\r\n" // SSE 事件分隔符
void sendSSE(const char* eventType, const char* eventData) {
  // 向浏览器发送事件数据
  if (sseSocketFD > 0) {
    char eventMsg[30];
    snprintf(eventMsg, 30 - 1, "event: %s\ndata: ", eventType);
    int res = httpd_socket_send(sseSocketHD, sseSocketFD, eventMsg, strlen(eventMsg), 0);
    res = httpd_socket_send(sseSocketHD, sseSocketFD, eventData, strlen(eventData), 0);
    res = httpd_socket_send(sseSocketHD, sseSocketFD, SSESEP, strlen(SSESEP), 0);
    if (res == HTTPD_SOCK_ERR_TIMEOUT) LOG_WRN("Timeout/interrupted while using socket");
    if (res == HTTPD_SOCK_ERR_FAIL) LOG_WRN("Unrecoverable error while using socket");
    if (res == HTTPD_SOCK_ERR_INVALID) LOG_WRN("Invalid arguments %s, %s", eventType, eventData);
  } else LOG_ERR("SSE not initiated");
}

static esp_err_t updateHandler(httpd_req_t *req) {
  // 批量更新配置，从接收的 JSON 字符串提取键值对
  size_t rxSize = min(req->content_len, (size_t)JSON_BUFF_LEN);
  int ret = 0;
  // 获取 JSON 载荷
  do {
    ret = httpd_req_recv(req, jsonBuff, rxSize);
    if (ret < 0) {  
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
      else {
        LOG_WRN("Update request failed with status %i", ret);
      }
    }
  } while (ret > 0);
  httpd_resp_sendstr(req, NULL); 
  if (ret >= 0 && parseJson(rxSize)) appSpecificWebHandler(req, "action", retainAction); 
  return ret < 0 ? ESP_FAIL : ESP_OK;
}

void progress(size_t prg, size_t sz) {
  static uint8_t pcProgress = 0;
  if (calcProgress(prg, sz, 5, pcProgress)) LOG_INF("OTA uploaded %d%%", pcProgress); 
}

esp_err_t uploadHandler(httpd_req_t *req) {
  // 上传文件到存储或进行固件更新
  esp_err_t res = ESP_OK;
  size_t fileSize = req->content_len;
  size_t rxSize = min(fileSize, (size_t)JSON_BUFF_LEN);
  int bytesRead = -1;
  LOG_INF("Upload file %s", inFileName);
  
  if (strstr(inFileName, ".bin") != NULL) {
    // 分区更新 - 程序或 SPIFFS
    LOG_INF("Firmware update using file %s", inFileName);
    OTAprereq();
    if (fdWs >= 0) httpd_sess_trigger_close(httpServer, fdWs);
    // SPIFFS 二进制文件名须包含 'spiffs'
    int cmd = (strstr(inFileName, "spiffs") != NULL) ? U_SPIFFS : U_FLASH;
    if (cmd == U_SPIFFS) STORAGE.end(); // 关闭相应文件系统
    if (Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
      do {
        bytesRead = httpd_req_recv(req, jsonBuff, rxSize);
        if (bytesRead < 0) {  
          if (bytesRead == HTTPD_SOCK_ERR_TIMEOUT) {
            delay(10);
            continue;
          } else {
            LOG_WRN("Upload request failed with status %i", bytesRead);
            break;
          }
        }
        Update.write((uint8_t*)jsonBuff, (size_t)bytesRead);
        Update.onProgress(progress);
        fileSize -= bytesRead;
      } while (bytesRead > 0);
      if (!fileSize) Update.end(true); // true 表示将大小设为当前进度
    }
    if (Update.hasError()) LOG_WRN("OTA failed with error: %s", Update.errorString());
    else LOG_INF("OTA update complete for %s", cmd == U_FLASH ? "Sketch" : "SPIFFS");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, Update.hasError() ? "OTA update failed, restarting ..." : "OTA update complete, restarting ...");   
    doRestart("Restart after OTA");

  } else {
    // 在存储上创建/替换数据文件
    File uf = STORAGE.open(inFileName, FILE_WRITE);
    if (!uf) LOG_WRN("Failed to open %s on storage", inFileName);
    else {
      // 获取文件内容
      do {
        bytesRead = httpd_req_recv(req, jsonBuff, rxSize);
        if (bytesRead < 0) {  
          if (bytesRead == HTTPD_SOCK_ERR_TIMEOUT) {
            delay(10);
            continue;
          } else {
            LOG_WRN("Upload request failed with status %i", bytesRead);
            break;
          }
        }
        uf.write((const uint8_t*)jsonBuff, bytesRead);
      } while (bytesRead > 0);
      uf.close();
      res = bytesRead < 0 ? ESP_FAIL : ESP_OK;
      httpd_resp_sendstr(req, res == ESP_OK ? "Completed upload file" : "Failed to upload file, retry");
      if (res == ESP_OK) LOG_INF("Uploaded file %s", inFileName);
      else LOG_WRN("Failed to upload file %s", inFileName);     
    }
  }
  return res;
}

static esp_err_t setupHandler(httpd_req_t *req) {
  // 扫描 WiFi 网络
  int w = (netMode == 0) ? WiFi.scanNetworks() : 0;
  // 开始构建 JSON 字符串
  char* p = jsonBuff;
  p += sprintf(p, "{\"networks\":[");
  // 用扫描结果填充 JSON 字符串
  for (int i = 0; i < w; ++i) {
    p += sprintf(p, "{\"ssid\":\"%s\",\"encryption\":\"%s\",\"strength\":\"%ld\"},", WiFi.SSID(i).c_str(), getEncType(i), WiFi.RSSI(i));
  }
  // 去掉末尾逗号并闭合 JSON 数组
  p += sprintf(p-1, "]}");
  // 设置响应类型为 JSON 并发送
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_sendstr(req, jsonBuff);
  return ESP_OK;
}

void showHttpHeaders(httpd_req_t *req) {
  // httpd_req_aux 结构体成员被隐藏，需通过偏移访问
  // 计算偏移时，未按 4 字节对齐的元素须打包
  LOG_DBG("HTTP: %s %s", HTTP_METHOD_STRING(req->method), req->uri); 
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
  size_t maxHdrLen = max(CONFIG_HTTPD_MAX_REQ_HDR_LEN, CONFIG_HTTPD_MAX_URI_LEN);
#else
  size_t maxHdrLen = max(HTTPD_MAX_REQ_HDR_LEN, HTTPD_MAX_URI_LEN);
#endif
  uint32_t req_hdrs_count = *((uint8_t*)req->aux + 4 + maxHdrLen + 1 + 3 + 4 + 4 + 4 + 1 + 3);
  char* header = (char*)req->aux + 4; // 请求头字符串起始位置
  // 依次获取每个请求头字符串
  while (req_hdrs_count--) {
    LOG_DBG("  %s", header);
    header += strlen(header) + 2;
  }
}

static esp_err_t sendCrossOriginHeader(httpd_req_t *req) {
  // 防止 CORS 拦截请求
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST,GET,HEAD,OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
  httpd_resp_set_status(req, "204");
  httpd_resp_sendstr(req, NULL); 
  return ESP_OK;
}

static bool checkWsSocketStatus() {
  // 检查连接是否活跃且为 WebSocket
  return (httpd_ws_get_fd_info(httpServer, fdWs) == HTTPD_WS_CLIENT_WEBSOCKET) ? true : false;
}

bool wsAsyncSendText(const char* wsData) {
  // WebSocket 发送文本，用于异步日志与状态更新
  if (checkWsSocketStatus()) {
    // 连接活跃时发送
    httpd_ws_frame_t wsPkt;
    wsPkt.payload = (uint8_t*)wsData;
    wsPkt.len = strlen(wsData);
    wsPkt.type = HTTPD_WS_TYPE_TEXT;
    wsPkt.final = true;
    esp_err_t ret = httpd_ws_send_frame_async(httpServer, fdWs, &wsPkt);
    if (ret != ESP_OK) LOG_WRN("websocket send failed with %s", esp_err_to_name(ret));
    return ret == ESP_OK ? true : false;
  } 
  return false;
}

bool wsAsyncSendJson(const char* dataType, const char* wsData) {
  // 构建待发送的 JSON
  char wsJson[strlen(dataType) + strlen(wsData) + 30];
  sprintf(wsJson, "{\"type\":\"%s\",\"payload\":{%s}}", dataType, wsData);
  return wsAsyncSendText(wsJson);
}

void wsAsyncSendBinary(uint8_t* data, size_t len) {
  // WebSocket 发送二进制，用于应用特定功能
  if (checkWsSocketStatus()) {
    if (data == NULL || len == 0) {
      LOG_WRN("Invalid data or length: data=%p, len=%u", data, len);
      return;
    }
    // 连接活跃时发送
    httpd_ws_frame_t wsPkt;
    memset(&wsPkt, 0, sizeof(httpd_ws_frame_t)); // 将所有字段初始化为零
    wsPkt.type = HTTPD_WS_TYPE_BINARY;
    wsPkt.payload = data;
    wsPkt.len = len;
    esp_err_t ret = httpd_ws_send_frame_async(httpServer, fdWs, &wsPkt);
    if (ret != ESP_OK) LOG_WRN("websocket send failed with %s", esp_err_to_name(ret));
  } // 否则忽略
}

static esp_err_t wsHandler(httpd_req_t *req) {
  // 接收 WebSocket 数据并决定响应
  // 若收到新连接，旧连接会关闭，但新连接上的浏览器页面
  // 可能需要手动刷新才能接管日志
  esp_err_t ret = ESP_OK;
  if (req->method == HTTP_GET) {
    // 来自浏览器客户端的 WebSocket 连接请求
    if (fdWs != -1) {
      if (fdWs != httpd_req_to_sockfd(req)) {
        // 已有其他浏览器 WebSocket 连接时收到新连接
        LOG_VRB("closing connection, as newer Websocket on %u", httpd_req_to_sockfd(req));
        // 关闭旧连接
        killSocket();
      }
    }
    fdWs = httpd_req_to_sockfd(req);
    if (fdWs < 0) {
      LOG_WRN("failed to get socket number");
      ret = ESP_FAIL;
    } else LOG_VRB("Websocket connection: %d", fdWs);
  } else {
    // 收到数据内容
    httpd_ws_frame_t wsPkt;
    uint8_t wsMsg[MAX_PAYLOAD_LEN];
    memset(&wsPkt, 0, sizeof(httpd_ws_frame_t));
    wsPkt.payload = wsMsg;
    ret = httpd_ws_recv_frame(req, &wsPkt, MAX_PAYLOAD_LEN); 
    if (ret == ESP_OK) {
      if (wsPkt.len >= MAX_PAYLOAD_LEN) LOG_ERR("websocket payload too long %d", wsPkt.len);
      wsMsg[wsPkt.len] = 0; // 结束符
      if (wsPkt.type == HTTPD_WS_TYPE_BINARY && wsPkt.len) appSpecificWsBinHandler(wsMsg, wsPkt.len);
      else if (wsPkt.type == HTTPD_WS_TYPE_TEXT) appSpecificWsHandler((const char*)wsMsg);
      else if (wsPkt.type == HTTPD_WS_TYPE_CLOSE) appSpecificWsHandler("X");
    } else LOG_ERR("websocket receive failed with %s", esp_err_to_name(ret));
  }
  return ret;
}

void killSocket(int skt) {
  // 用户请求
  if (skt == -99) {
    skt = fdWs;
    fdWs = -1;
  }
  if (skt >= 0) httpd_sess_trigger_close(httpServer, skt);
}

/*
static void https_server_user_callback(esp_https_server_user_cb_arg_t *user_cb) {
  LOG_DBG("Session created, socket: %d", user_cb->tls->sockfd);
}
*/

static esp_err_t customOrNotFoundHandler(httpd_req_t *req, httpd_err_code_t err) {
  // 处理 WebDAV 方法或报告 URI 不存在
  if (req->method == HTTP_OPTIONS) sendCrossOriginHeader(req);
#if INCLUDE_WEBDAV
  if (strncmp(req->uri, WEBDAV, strlen(WEBDAV)) == 0) return handleWebDav(req) ? ESP_OK : ESP_FAIL;
#endif
  // 其他 URI 返回 404 并关闭套接字
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

bool startWebServer() {
  esp_err_t res = ESP_FAIL;
  chunk = psramFound() ? (byte*)ps_malloc(CHUNKSIZE) : (byte*)malloc(CHUNKSIZE);
#if INCLUDE_CERTS
  if (useHttps) {
    // HTTPS 服务器
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32S3
    config.httpd.stack_size = SERVER_STACK_SIZE;
#endif  
    config.prvtkey_pem = (const uint8_t*)serverCerts[0];
    config.prvtkey_len = strlen(serverCerts[0]) + 1;
    config.servercert = (const uint8_t*)serverCerts[1];
    config.servercert_len = strlen(serverCerts[1]) + 1;
  
    //config.user_cb = https_server_user_callback;
    config.httpd.server_port = HTTPS_PORT;
    config.httpd.lru_purge_enable = true; // 关闭最少使用的套接字
    config.httpd.max_uri_handlers = MAX_HANDLERS;
    config.httpd.max_open_sockets = HTTP_CLIENTS + MAX_STREAMS;
    config.httpd.task_priority = HTTP_PRI;
    //config.httpd.uri_match_fn = httpd_uri_match_wildcard;
    res = httpd_ssl_start(&httpServer, &config);
  } else {
#else
  if (!useHttps) {
#endif
    // HTTP 服务器
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
#if CONFIG_IDF_TARGET_ESP32S3
    config.stack_size = SERVER_STACK_SIZE;
#endif
    config.server_port = HTTP_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = MAX_HANDLERS;
    config.max_open_sockets = HTTP_CLIENTS + MAX_STREAMS;
    config.task_priority = HTTP_PRI;
    //config.uri_match_fn = httpd_uri_match_wildcard;
    res = httpd_start(&httpServer, &config);
  }
  httpd_uri_t indexUri = {.uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = NULL};
  httpd_uri_t webUri = {.uri = "/web", .method = HTTP_GET, .handler = webHandler, .user_ctx = NULL};
  httpd_uri_t controlUri = {.uri = "/control", .method = HTTP_GET, .handler = controlHandler, .user_ctx = NULL};
  httpd_uri_t updateUri = {.uri = "/update", .method = HTTP_POST, .handler = updateHandler, .user_ctx = NULL};
  httpd_uri_t statusUri = {.uri = "/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = NULL};
  httpd_uri_t uploadUri = {.uri = "/upload", .method = HTTP_POST, .handler = uploadHandler, .user_ctx = NULL};
  httpd_uri_t wifiUri = {.uri = "/wifi", .method = HTTP_GET, .handler = setupHandler, .user_ctx = NULL};
  httpd_uri_t sseUri = {.uri = "/sse", .method = HTTP_GET, .handler = sseHandler, .user_ctx = NULL};
  httpd_uri_t wsUri = {.uri = "/ws", .method = HTTP_GET, .handler = wsHandler, .user_ctx = NULL, .is_websocket = true};
  httpd_uri_t sustainUri = {.uri = "/sustain", .method = HTTP_GET, .handler = appSpecificSustainHandler, .user_ctx = NULL};
  httpd_uri_t checkUri = {.uri = "/sustain", .method = HTTP_HEAD, .handler = appSpecificSustainHandler, .user_ctx = NULL};

  if (res == ESP_OK) {
    httpd_register_uri_handler(httpServer, &indexUri);
    httpd_register_uri_handler(httpServer, &webUri);
    httpd_register_uri_handler(httpServer, &controlUri);
    httpd_register_uri_handler(httpServer, &updateUri);
    httpd_register_uri_handler(httpServer, &statusUri);
    httpd_register_uri_handler(httpServer, &uploadUri);
    httpd_register_uri_handler(httpServer, &sseUri);
    httpd_register_uri_handler(httpServer, &wifiUri);
    httpd_register_uri_handler(httpServer, &wsUri);
    httpd_register_uri_handler(httpServer, &sustainUri);
    httpd_register_uri_handler(httpServer, &checkUri);
    httpd_register_err_handler(httpServer, HTTPD_404_NOT_FOUND, customOrNotFoundHandler);

    LOG_INF("Starting web server on port: %u", useHttps ? HTTPS_PORT : HTTP_PORT);
    LOG_INF("Remote server certificates %s checked", useSecure ? "are" : "not");
    if (DEBUG_MEM) {
      uint32_t freeStack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
      LOG_INF("Task httpServer stack space %u", freeStack);
    }
  } else snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to start webserver %s",espErrMsg(res));
  if (!DBG_ON) esp_log_level_set("*", ESP_LOG_NONE); // 抑制 ESP_LOG_ERROR 消息
  debugMemory("startWebserver");
  if (strlen(startupFailure)) {
    LOG_WRN("%s", startupFailure);
    return false;
  }
  return true;
}
