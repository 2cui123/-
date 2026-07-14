// streamServer 处理流式传输、回放和文件下载
// 每个持续运行的活动在可用时使用独立任务
// - 网页流、回放、文件下载使用任务 0
// - 视频流使用任务 1
// - 音频流使用任务 2
// - 字幕流使用任务 3
//
// s60sc 2022 - 2025

#include "appGlobals.h"

// 流分隔符
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" BOUNDARY_VAL
#define JPEG_BOUNDARY "\r\n--" BOUNDARY_VAL "\r\n"
#define JPEG_TYPE "Content-Type: image/jpeg\r\nContent-Length: %10u\r\n\r\n"
#define HDR_BUF_LEN 64
#define END_WAIT 100

static bool forcePlayback = false; // 浏览器回放状态
bool streamVid = false;
bool streamAud = false;
bool streamSrt = false;
static bool isStreaming[MAX_STREAMS] = {false};
size_t streamBufferSize[MAX_STREAMS] = {0};
byte* streamBuffer[MAX_STREAMS] = {NULL}; // 流帧缓冲区
static char variable[FILE_NAME_LEN]; 
static char value[FILE_NAME_LEN];
uint16_t sustainId = 0;
uint8_t numStreams = 1;
uint8_t vidStreams = 1;
int srtInterval = 1; // 字幕间隔（秒）

#ifndef AUXILIARY

TaskHandle_t sustainHandle[MAX_STREAMS]; 
struct httpd_sustain_req_t {
  httpd_req_t* req = NULL;
  uint8_t taskNum; 
  char activity[16];
  bool inUse = false; 
};
httpd_sustain_req_t sustainReq[MAX_STREAMS];

#if INCLUDE_RTSP
static const bool includeRTSP = true;
#else
static const bool includeRTSP = false;
#endif

static void showPlayback(httpd_req_t* req) {
  // 将回放文件输出到浏览器
  esp_err_t res = ESP_OK; 
  stopPlaying();
  forcePlayback = true;
  if (STORAGE.exists(inFileName)) {
    if (stopPlayback) LOG_WRN("Playback refused - capture in progress");
    else {
      LOG_INF("Playback enabled (SD file selected)");
      doPlayback = true;
    }
  } else LOG_WRN("File %s doesn't exist when Playback requested", inFileName);

  if (doPlayback) {
    // 从 SD 卡回放 mjpeg
    mjpegStruct mjpegData;
    // 输出回放请求的响应头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    char hdrBuf[HDR_BUF_LEN];
    openSDfile(inFileName);
    mjpegData = getNextFrame(true);
    while (doPlayback) {
      size_t jpgLen = mjpegData.buffLen;
      size_t buffOffset = mjpegData.buffOffset;
      if (!jpgLen && !buffOffset) {
        // mjpeg 回放流完成
        res = httpd_resp_sendstr_chunk(req, JPEG_BOUNDARY);
        doPlayback = false; 
      } else {
        if (jpgLen) {
          if (mjpegData.jpegSize) { // 帧开始
            // 发送 mjpeg 响应头 
            if (res == ESP_OK) res = httpd_resp_sendstr_chunk(req, JPEG_BOUNDARY);
            snprintf(hdrBuf, HDR_BUF_LEN-1, JPEG_TYPE, mjpegData.jpegSize);
            if (res == ESP_OK) res = httpd_resp_sendstr_chunk(req, hdrBuf);   
          } 
          // 发送缓冲区 
          if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)iSDbuffer+buffOffset, jpgLen);
        }
        if (res == ESP_OK) mjpegData = getNextFrame(); 
        else {
          // 浏览器关闭回放时会出现发送错误
          LOG_VRB("Playback aborted due to error: %s", espErrMsg(res));
          stopPlaying();
        }
      }
    }
    if (res == ESP_OK) httpd_resp_sendstr_chunk(req, NULL);
    sustainId = currEpoch;
  } 
  if (!doPlayback && forcePlayback) {
    // 在浏览器上关闭回放
    forcePlayback = false;
    wsAsyncSendJson("ustatus", "\"forcePlayback\":0");
  }
}

static void showStream(httpd_req_t* req, uint8_t taskNum) {
  // 开始向浏览器进行实时流式传输
  esp_err_t res = ESP_OK; 
  size_t jpgLen = 0;
  uint8_t* jpgBuf = NULL;
  uint32_t startTime = millis();
  uint32_t frameCnt = 0;
  uint32_t mjpegLen = 0;
  isStreaming[taskNum] = true;
  streamBufferSize[taskNum] = 0;
  if (!taskNum) motionJpegLen = 0;
  // 输出流式传输请求的响应头
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  char hdrBuf[HDR_BUF_LEN];
  while (isStreaming[taskNum]) {
    // 以当前帧率从摄像头流式传输
    if (xSemaphoreTake(frameSemaphore[taskNum], pdMS_TO_TICKS(MAX_FRAME_WAIT)) == pdFAIL) {
      // 获取信号量失败，允许重试
      streamBufferSize[taskNum] = 0;
      continue;
    }
    if (dbgMotion && !taskNum) {
      // 运动跟踪流仅在任务 0 上，等待新的运动映射图像
      if (xSemaphoreTake(motionSemaphore, pdMS_TO_TICKS(MAX_FRAME_WAIT)) == pdFAIL) continue;
      // 使用 checkMotion() 创建的图像
      jpgLen = motionJpegLen;
      if (!jpgLen) continue;
      jpgBuf = motionJpeg;
    } else {
      // 实时流 
      if (!streamBufferSize[taskNum]) continue;
      jpgLen = streamBufferSize[taskNum];
      // 使用 processFrame() 存储的帧
      jpgBuf = streamBuffer[taskNum];
    }
    if (res == ESP_OK) {
      // 发送流中的下一帧
      res = httpd_resp_sendstr_chunk(req, JPEG_BOUNDARY);  
      snprintf(hdrBuf, HDR_BUF_LEN-1, JPEG_TYPE, jpgLen);
      if (res == ESP_OK) res = httpd_resp_sendstr_chunk(req, hdrBuf);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)jpgBuf, jpgLen);
      frameCnt++;
    }
    mjpegLen += jpgLen;
    jpgLen = streamBufferSize[taskNum] = 0;
    if (dbgMotion && !taskNum) motionJpegLen = 0;
    if (res != ESP_OK) {
      // 浏览器关闭流时会出现发送错误 
      LOG_VRB("Streaming aborted due to error: %s", espErrMsg(res));
      isStreaming[taskNum] = false;
    }     
  }
  if (res == ESP_OK) httpd_resp_sendstr_chunk(req, NULL);
  uint32_t mjpegTime = millis() - startTime;
  float mjpegTimeF = float(mjpegTime) / 1000; // 秒
  LOG_INF("MJPEG: %u frames, total %s in %0.1fs @ %0.1ffps", frameCnt, fmtSize(mjpegLen), mjpegTimeF, (float)(frameCnt) / mjpegTimeF);
}

static void audioStream(httpd_req_t* req, uint8_t taskNum) {
  // 向远程 NVR 输出 WAV 音频流
#if INCLUDE_AUDIO
  if (micGain) {
    esp_err_t res = ESP_OK;
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    isStreaming[taskNum] = true;
    uint32_t totalSamples = 0;
    audioBytes = WAV_HDR_LEN;
    updateWavHeader();
    while (isStreaming[taskNum]) {
      if (audioBytes) {
        res = httpd_resp_send_chunk(req, (const char*)audioBuffer, audioBytes); 
        audioBytes = 0;
      } else delay(20); // 等待缓冲区加载
      if (res != ESP_OK) isStreaming[taskNum] = false; // 客户端连接已关闭
      else totalSamples += audioBytes / 2; // 16 位采样
    }
    audioBytes = 1; // 停止加载缓冲区
    if (res == ESP_OK) httpd_resp_sendstr_chunk(req, NULL);
    LOG_INF("WAV: sent %lu samples", totalSamples);
  } else LOG_WRN("No ESP mic defined or mic is off");
#else 
  httpd_resp_sendstr(req, NULL);
#endif
}

static void srtStream(httpd_req_t* req, uint8_t taskNum) {
  // 生成用于流式传输的字幕条目，包含时间戳
  // 若启用遥测则附加遥测数据
  esp_err_t res = ESP_OK;
  isStreaming[taskNum] = true;
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); 
  int srtSeqNo = 0;
  uint32_t srtTime = 0;
  const uint32_t sampleInterval = 1000 * (srtInterval < 1 ? 1 : srtInterval);
  char srtHdr[100];
  char timeStr[10];
  while (isStreaming[taskNum]) {
    srtSeqNo++;
    uint32_t startTime = millis();
    formatElapsedTime(timeStr, srtTime, true);
    size_t srtPtr = sprintf(srtHdr, "%d\n%s --> ", srtSeqNo, timeStr);
    srtTime += sampleInterval;
    formatElapsedTime(timeStr, srtTime, true);
    srtPtr += sprintf(srtHdr + srtPtr, "%s\n", timeStr);
    time_t currEpoch = getEpoch();
    srtPtr += strftime(srtHdr + srtPtr, 12, "%H:%M:%S  ", localtime(&currEpoch));
    httpd_resp_send_chunk(req, (const char*)srtHdr, srtPtr);
#if INCLUDE_TELEM
    // 添加遥测数据 
    if (teleUse) {
      storeSensorData(true);
      if (srtBytes) res = httpd_resp_send_chunk(req, (const char*)srtBuffer, srtBytes);
      srtBytes = 0;
    }
#endif
    if (res == ESP_OK) res = httpd_resp_sendstr_chunk(req, "\n\n");
    if (res != ESP_OK) isStreaming[taskNum] = false; // 客户端连接已关闭
    else while (isStreaming[taskNum] && millis() - sampleInterval < startTime) delay(50);
  }
  if (res == ESP_OK) httpd_resp_sendstr_chunk(req, NULL);
  LOG_INF("SRT: sent %d subtitles", srtSeqNo);
}

void stopSustainTask(int taskId) {
  isStreaming[taskId] = false;
}

static void sustainTask(void* p) {
  // 作为独立任务处理持续的 http(s) 请求 
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t i = *(uint8_t*)p; // 识别任务编号
    if (i == 0) {
      if (!strcmp(sustainReq[i].activity, "download")) fileHandler(sustainReq[i].req, true); 
      else if (!strcmp(sustainReq[i].activity, "playback")) showPlayback(sustainReq[i].req);
      else if (!strcmp(sustainReq[i].activity, "stream")) showStream(sustainReq[i].req, i);
    } 
    else if (i == 1) showStream(sustainReq[i].req, i);
    else if (i == 2) audioStream(sustainReq[i].req, i);
    else if (i == 3) srtStream(sustainReq[i].req, i);
    // 请求返回时清理
    if (httpd_req_async_handler_complete(sustainReq[i].req) != ESP_OK) LOG_ERR("Failed to free req for sustain task: %i", i);
    sustainReq[i].inUse = false; 
  }
  vTaskDelete(NULL);
}

void startSustainTasks() {
  // 启动 httpd 持续任务
  if (streamVid) numStreams = vidStreams = 2;
  if (streamAud) numStreams = 3;
  if (streamSrt) numStreams = 4;
  if (numStreams > MAX_STREAMS) {
    LOG_WRN("numStreams %d exceeds MAX_STREAMS %d", numStreams, MAX_STREAMS);
    numStreams = MAX_STREAMS;
  }
  if (maxFrameBuffSize * (vidStreams + 1) > ESP.getFreePsram()) {
    LOG_WRN("Insufficient PSRAM for NVR streams");
    vidStreams = 1;
    streamVid = streamAud = streamSrt = false;
  }
  for (int i = 0; i < vidStreams; i++)
    if (streamBuffer[i] == NULL) streamBuffer[i] = (byte*)ps_malloc(maxFrameBuffSize); 

  for (int i = 0; i < numStreams; i++) {
    sustainReq[i].taskNum = i; // 让任务知道自己的编号
    if (includeRTSP && i > 0) continue; // RTSP 任务在 rtsp.cpp 中创建
    xTaskCreateWithCaps(sustainTask, "sustainTask", SUSTAIN_STACK_SIZE, &sustainReq[i].taskNum, SUSTAIN_PRI, &sustainHandle[i], HEAP_MEM); 
  }
  
  LOG_INF("Started %d sustain tasks", numStreams);
  debugMemory("startSustainTasks");
}

esp_err_t appSpecificSustainHandler(httpd_req_t* req) {
  // 首先检查是否需要并通过身份验证
  esp_err_t res = ESP_FAIL;
  if (checkAuth(req)) { 
    // 将长时间运行的请求作为独立任务处理
    // 从查询字符串获取详情
    if (extractQueryKeyVal(req, variable, value) == ESP_OK) {
      // 回放、下载、网页流使用任务 0
      // 远程流（如视频）使用任务 1，音频任务 2，srt 任务 3
      uint8_t taskNum = 99;
      if (!strcmp(variable, "download")) taskNum = 0;
      else if (!strcmp(variable, "playback")) taskNum = 0;
      else if (!strcmp(variable, "stream")) taskNum = 0;
      else if (!strcmp(variable, "video")) taskNum = 1;
      else if (!strcmp(variable, "audio")) taskNum = 2;
      else if (!strcmp(variable, "srt")) taskNum = 3;
      // 使用 RTSP 时 http(s) 流不可用
      if (includeRTSP && taskNum > 0) taskNum = 99;
      if (taskNum < numStreams) {
        if (taskNum == 0) {
          if (req->method == HTTP_HEAD) { 
            // 来自应用网页的任务检查请求
            if (sustainReq[taskNum].inUse) {
              // 任务未空闲，尝试停止以便新流使用
              if (!strcmp(variable, "stream")) {
                isStreaming[taskNum] = false;
                if (!taskNum) doPlayback = false; // 仅用于任务 0
                delay(END_WAIT + 100);
              }
            } 
            if (sustainReq[taskNum].inUse) {
              LOG_WRN("Task %d not free", taskNum);
              httpd_resp_set_status(req, "500 No free task");
            }
            else {
              sustainId = currEpoch; // 任务可用
              res = ESP_OK;
            }
            httpd_resp_sendstr(req, NULL);
            return res;
          }
        } else {
          // 若远程流当前处于活动状态则停止
          if (taskNum < MAX_STREAMS) {
            if (sustainReq[taskNum].inUse) {
              isStreaming[taskNum] = false;
              delay(END_WAIT + 100);
            }
          }
        }
            
        // 任务可用时执行请求
        if (!sustainReq[taskNum].inUse) {
          // 复制请求数据并将请求传递给对应编号的任务
          uint8_t i = taskNum;
          sustainReq[i].inUse = true;
          httpd_req_t* copy = NULL;
          if ((res = httpd_req_async_handler_begin(req, &copy)) != ESP_OK) {
            LOG_ERR("Failed to copy req for sustain task: %i", i);
            return res;
          }
          sustainReq[i].req = copy;
          strncpy(sustainReq[i].activity, variable, sizeof(sustainReq[i].activity) - 1); 
          // 激活相应任务
          xTaskNotifyGive(sustainHandle[i]);
          return ESP_OK;
        } else httpd_resp_set_status(req, "500 No free task");
      } else {
        if (taskNum < MAX_STREAMS) LOG_WRN("Task not created for stream: %s, numStreams %d", variable, numStreams);
        else LOG_WRN("Invalid task id: %s", variable);
        httpd_resp_set_status(req, "400 Invalid url");
      }
    } else httpd_resp_set_status(req, "400 Bad URL");
    httpd_resp_sendstr(req, NULL);
  } 
  return res;
}

#else

// 占位函数
esp_err_t appSpecificSustainHandler(httpd_req_t* req) {return ESP_OK;}

#endif
