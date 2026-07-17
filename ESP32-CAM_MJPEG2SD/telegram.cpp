// 简单的通用 Telegram 机器人，支持：
// - 消息交互
// - 照片上传
// - 文件上传（避免与 ftp、smtp 或浏览器文件上传同时使用）
// 在 appSpecific.cpp 的 appSetupTelegram() 中添加自定义处理
//
// 参考思路来自：
// - https://github.com/jameszah/ESP32-CAM-Video-Telegram
// - https://github.com/cotestatnt/AsyncTelegram2
// 

#include "appGlobals.h"

#if INCLUDE_TGRAM
#define TELEGRAM_HOST "api.telegram.org"
#define LONG_POLL 60 // 无回复时保持连接打开的时长（秒）
#define MAX_HTTP_MSG 2048 // HTTP 请求或响应体的最大缓冲区大小
#define FORM_OFFSET 256 // tgramBuff 中用于准备表单数据的偏移量
#define MAX_TGRAM_SIZE (50 * ONEMEG) // Telegram 文件上传最大尺寸

#define HTTP_VER "HTTP/1.1"
#define HTTP_CODE HTTP_VER " %d %*s\r"
#define POST_HDR "POST /bot%s/%s " HTTP_VER "\r\nHost: " TELEGRAM_HOST "\r\nContent-Length: %u\r\nContent-Type: "
#define FORM_DATA "--" BOUNDARY_VAL "\r\nContent-disposition: form-data; name=\""
#define CONTENT_TYPE "\"; filename=\"%s\"\r\nContent-Type: \"%s\"\r\n\r\n"
#define MULTI_TYPE "multipart/form-data; boundary=" BOUNDARY_VAL
#define JSON_TYPE "application/json"
#define GETUP_JSON "{\"limit\":1,\"timeout\":%d,\"offset\":%ld}"
#define POST_JSON "{\"chat_id\":%s,\"text\":\"%s\n\n%s%s\n\"}"
#define PARSE_MODE ",\"parse_mode\":\"%s\"}"
#define END_BOUNDARY "\r\n--" BOUNDARY_VAL "--\r\n"

#if (!INCLUDE_CERTS)
const char* telegram_rootCACertificate = "";
#endif

// 通过网页界面设置
bool tgramUse = false;
char tgramToken[MAX_PWD_LEN] = "";
char tgramChatId[MAX_IP_LEN] = "";

char tgramHdr[FILE_NAME_LEN];
static char keyValue[100] = ""; // 保存 JSON 响应中搜索键的值
static char* tgramBuff = NULL; // 保存发送与接收的数据
static int32_t lastUpdate = 0;

TaskHandle_t telegramHandle = NULL;
NetworkClientSecure tclient;

static inline bool connectTelegram() {
  // 若尚未连接则连接 Telegram 服务器
  return remoteServerConnect(tclient, TELEGRAM_HOST, HTTPS_PORT, telegram_rootCACertificate, TGRAMCONN);
}

static bool searchJsonResponse(const char* keyName) {
  // 在 JSON 中搜索给定键并提取值，键名须以冒号结尾
  char* keyPtr = strstr(tgramBuff, keyName);
  if (keyPtr == NULL) return false;
  char* startItem = keyPtr + strlen(keyName);
  char* endItem = strchr(startItem, ',');
  int valSize = endItem - startItem;
  if (valSize > sizeof(keyValue) - 1) {
    LOG_WRN("Telegram JSON value too long %d", valSize); 
    valSize = sizeof(keyValue - 1);
  }
  strncpy(keyValue, startItem, valSize);
  keyValue[valSize] = 0;
  return true;
}

size_t getResponseHeader(NetworkClientSecure& sclient, const char* host, int waitSecs) {
  // 若可用则从远程服务器获取响应头
  if (!waitSecs) waitSecs = responseTimeoutSecs;
  bool endOfHeader = false;
  size_t contentLen = 0;
  int httpCode = 0;
  uint32_t startTime = millis();
  if (sclient.available()) {
    while (!endOfHeader && millis() - startTime < waitSecs * 1000) {
      if (sclient.available()) { 
        String tline = sclient.readStringUntil('\n');
        //printf("Res: %s\n", tline.c_str());
        endOfHeader = tline.length() > 1 ? false : true; // 空行表示头部结束
        if (!httpCode) sscanf(tline.c_str(), HTTP_CODE, &httpCode);  
        // 从头部获取 contentLength
        if (!contentLen) sscanf(tline.c_str(), "Content-Length: %d\r", &contentLen); 
      } else delay(100); 
    }
    if (!endOfHeader) {
      LOG_WRN("Timed out waiting for response from %s", host);
      return 0;
    } 
  } 
  return contentLen;
}

static bool getTgramResponse() {
  // 若有可用响应则接收并检查是否成功
  bool haveResponse = false;
  size_t readLen = 0;
  size_t contentLen = getResponseHeader(tclient, TELEGRAM_HOST, LONG_POLL);
  if (contentLen) {
    if (contentLen >= MAX_HTTP_MSG - 1) {
      LOG_WRN("contentLen %d exceeds buffer size", contentLen);
      contentLen = MAX_HTTP_MSG - 1;
    }
    while (contentLen - readLen > 0) {
      // 获取响应内容
      size_t availLen = tclient.available();
      if (availLen) readLen += tclient.readBytes((uint8_t*)tgramBuff + readLen, availLen);
      delay(50);
    }
    // 格式化 tgramBuff 供 searchJsonResponse() 使用
    if (readLen != contentLen) LOG_WRN("Telegram data %d not equal to contentLength %d", readLen, contentLen);
    tgramBuff[contentLen] = 0;
    removeChar(tgramBuff, '"');
    replaceChar(tgramBuff, '}', ',');
    // 检查 Telegram 响应是否确认请求
    if (searchJsonResponse("ok:")) {
      if (strcmp(keyValue, "true")) {
        // 获取错误描述
        if (searchJsonResponse("description:")) LOG_WRN("Telegram error: %s", keyValue);
        else LOG_WRN("Telegram error, but description not retrieved");
      } else if (searchJsonResponse("result:")) {
        // 若 result 含数据则有响应，否则仅为确认
        if (strcmp(keyValue, "[]")) haveResponse = true;
      }
    } 
    //printf("Cnt: %s\n", tgramBuff);
    remoteServerClose(tclient); // 事务结束
  } // 否则未收到数据，保持连接
  return haveResponse;
}

static bool sendTgramHeader(const char* tmethod, const char* contentType, const char* dataType, 
  size_t fileSize, const char* fileName, const char* caption) {
  if (connectTelegram()) {
    // 创建 HTTP POST 头
    char* p = tgramBuff + FORM_OFFSET; // 为 HTTP 请求数据预留空间
    bool isFile = dataType != NULL ? true : false; 
    if (isFile) {
      p += sprintf(p, FORM_DATA "chat_id\"\r\n\r\n%s", tgramChatId);
      if (caption != NULL) p += sprintf(p, "\r\n" FORM_DATA "caption\"\r\n\r\n%s", caption);
      p += sprintf(p, "\r\n" FORM_DATA "%s", dataType);
      p += sprintf(p, CONTENT_TYPE, fileName, contentType);
    } // 否则 JSON 数据已由 sendTgramMessage 加载
    size_t formLen = strlen(tgramBuff + FORM_OFFSET);
    // 创建 HTTP 请求头
    p = tgramBuff;
    if (isFile) fileSize += formLen + strlen(END_BOUNDARY);
    p += sprintf(p, POST_HDR, tgramToken, tmethod, fileSize);
    isFile ? strcat(p, MULTI_TYPE) : strcat(p, JSON_TYPE);
    strcat(p, "\r\n\r\n");
    size_t reqLen = strlen(tgramBuff);
    // 拼接请求与表单数据
    if (formLen) {
      memmove(tgramBuff + reqLen, tgramBuff + FORM_OFFSET, formLen);
      tgramBuff[reqLen + formLen] = 0;
    }
    tclient.print(tgramBuff); // HTTP 头
    //printf("header:\n%s\n", tgramBuff);
    return true;
  }
  return false;
}

static bool sendTgramBuff(uint8_t* buffData, size_t buffSize) {
  // 通用：发送缓冲区内容的 POST 消息，例如照片
  if (connectTelegram()) {
    // 分块发送
    for (size_t i = 0; i < buffSize; i += CHUNKSIZE) tclient.write(buffData + i, min((int)(buffSize - i), CHUNKSIZE));
    tclient.println(END_BOUNDARY);
    return true;
  } 
  return false; 
}
    
bool prepTelegram() {
  // 若需要则设置并检查 Telegram 访问
  if (tgramUse) {
    if (strlen(tgramToken)) {
      if (tgramBuff == NULL) tgramBuff = psramFound() ? (char*)ps_malloc(MAX_HTTP_MSG) : (char*)malloc(MAX_HTTP_MSG); 
      // 用 getMe 请求检查连接
      bool res = false;
      sendTgramHeader("getMe", NULL, NULL, 0, NULL, NULL);
      uint32_t startTime = millis();
      while (!res && (millis() - startTime < responseTimeoutSecs * 1000)) {
        if (getTgramResponse()) res = true;
        delay(200);
      }
      if (res) {
        // 响应已加载到 tgramBuff
        if (searchJsonResponse("username:")) {      
          LOG_INF("Connected to Telegram Bot Handle: %s", keyValue);
          xTaskCreateWithCaps(appSpecificTelegramTask, "telegramTask", TGRAM_STACK_SIZE, NULL, TGRAM_PRI, &telegramHandle, HEAP_MEM); 
          debugMemory("setupTelegramTask");
          return true;
        } else LOG_WRN("getMe response not parsed %s", tgramBuff);
      } else LOG_WRN("Failed to communicate with Telegram server");
    } else LOG_WRN("No Telegram Bot token supplied");
  } else LOG_INF("Telegram not being used");
  return false;
} 

bool getTgramUpdate(char* responseText) {
  // 获取并处理来自 Telegram 的消息
  if (tclient.connected()) {
    // 检查是否有传入消息
    if (getTgramResponse()) {
      // 处理响应并提取命令（若有）
      if (searchJsonResponse("update_id:")) {
        int32_t update_id = atoi(keyValue);
        if (lastUpdate < update_id) {
          // 新消息，可以处理
          lastUpdate = update_id;
          if (searchJsonResponse("chat:{id:")) {
            if (!strcmp(tgramChatId, keyValue)) {
              if (searchJsonResponse("text:")) {
                strncpy(responseText, keyValue, FILE_NAME_LEN - 1);
                return true; // 用户请求供应用处理
              } // 无文本，忽略
            } else LOG_WRN("Message from unknown chat id: %s", keyValue);
          } else LOG_WRN("No chat id found");
        } else LOG_WRN("Old update_id: %d", update_id);
      } // 无 update_id，忽略
    } 
  } else {
    // 未连接则发送 getUpdates 请求
    char* t = tgramBuff + FORM_OFFSET;
    t += sprintf(t, GETUP_JSON, LONG_POLL, lastUpdate + 1);
    sendTgramHeader("getUpdates", NULL, NULL, strlen(tgramBuff + FORM_OFFSET), NULL, NULL);
  }
  return false; // 无响应供应用处理
}

bool sendTgramMessage(const char* info, const char* item, const char* parseMode) {
  // 将消息格式化为 JSON，追加到 HTTP 头（缓冲区溢出可能性很小）
  char* t = tgramBuff + FORM_OFFSET;
  t += sprintf(t, POST_JSON, tgramChatId, tgramHdr, info, item);
  if (strlen(parseMode)) t += sprintf(t - 1, PARSE_MODE, parseMode); // 覆盖前一个 '}'
  return sendTgramHeader("sendMessage", NULL, NULL, strlen(tgramBuff + FORM_OFFSET), NULL, NULL);
}

bool sendTgramPhoto(uint8_t* photoData, size_t photoSize, const char* caption) {
  // 将缓冲区中的照片发送到 Telegram
  // Telegram 照片上传最大 10MB，大于 ESP 相机上限
  if (sendTgramHeader("sendPhoto", "image/jpeg", "photo", photoSize, "frame.jpg", caption))
    return sendTgramBuff(photoData, photoSize);
  return false;
}

bool sendTgramFile(const char* fileName, const char* contentType, const char* caption) {
  // 从选定存储读取指定文件并发送到 Telegram
  if (connectTelegram()) {
    File df = STORAGE.open(fileName);
    char errMsg[100] = "";
    if (df) {
      if (df.size() < MAX_TGRAM_SIZE) {
        sendTgramHeader("sendDocument", contentType, "document", df.size(), fileName, caption);
        // 分块上传文件内容
        uint8_t percentLoaded = 0;
        size_t chunksize = 0, totalSent = 0;
        while ((chunksize = df.read((uint8_t*)tgramBuff, MAX_HTTP_MSG))) {
          tclient.write((uint8_t*)tgramBuff, chunksize);
          totalSent += chunksize;
          if (calcProgress(totalSent, df.size(), 5, percentLoaded)) LOG_INF("Downloaded %u%%", percentLoaded); 
        }
        df.close();
        tclient.println(END_BOUNDARY);
      } else snprintf(errMsg, sizeof(errMsg) - 1, "File size too large: %s", fmtSize(df.size()));        
    } else snprintf(errMsg, sizeof(errMsg) - 1, "File does not exist or cannot be opened: %s", fileName);
    if (strlen(errMsg)) {
      LOG_WRN("%s", errMsg);
      sendTgramMessage("ERROR: ", errMsg, "");
    }
  } else return false;
  return true;
}

#endif
