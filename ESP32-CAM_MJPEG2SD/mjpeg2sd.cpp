/*
  将 ESP32 Cam 的 JPEG 图像捕获到 AVI 文件并存储到 SD 卡
  文件写入与 SD 卡扇区大小对齐。
  存储在 SD 卡上的 AVI 文件也可被选中并流式传输到浏览器。
*/

#include "appGlobals.h"
#if INCLUDE_AF
#if __has_include("../libraries/OV5640_Auto_Focus_for_ESP32_Camera/src/ESP32_OV5640_AF.h") 
#include <ESP32_OV5640_AF.h>
OV5640 ov5640AF = OV5640();
#else
#error "Need to install OV5640_Auto_Focus_for_ESP32_Camera library"
#endif
#endif

#define FB_CNT 4 // 帧缓冲区数量

// 从网页设置的用户参数
bool useMotion = true; // 是否使用摄像头进行运动检测（配合 motionDetect.cpp）
bool forceRecord = false; // 由录制按钮启用录制

// 运动检测参数
int moveStartChecks = 5; // 每秒检测运动开始的次数
int moveStopSecs = 2; // 每次停止检测之间的秒数，也决定运动后时间
int maxFrames = 20000; // 视频自动关闭前的最大帧数

// 独立于运动捕获录制延时摄影 AVI，文件名格式与 avi 相同，但以 T 结尾
int tlSecsBetweenFrames; // 间隔过短会干扰其他活动
int tlDurationMins; // 上一个文件结束后开始新文件
int tlPlaybackFPS;  // 延时摄影回放帧率，最小为 1

// 状态与控制字段
uint8_t FPS = 0;
bool nightTime = false;
uint8_t fsizePtr; // frameData[] 的索引
uint8_t minSeconds = 5; // 默认最短视频长度（含 POST_MOTION_TIME）
bool doRecording = true; // 是否捕获到 SD 卡
uint8_t xclkMhz = 20; // 摄像头时钟频率（MHz）
bool doKeepFrame = false;
static bool haveSrt = false;
char camModel[11];
static int siodGpio = SIOD_GPIO_NUM;
static int siocGpio = SIOC_GPIO_NUM;
size_t maxFrameBuffSize;
static int frameLimit;

// 文件头与统计信息
static uint32_t vidSize; // 视频总大小
static uint16_t frameCnt;
static uint32_t startTime; // 总耗时
static uint32_t dTimeTot; // 帧解码/监控总时间
static uint32_t fTimeTot; // 帧缓冲总时间
static uint32_t wTimeTot; // SD 写入总时间
static uint32_t oTime; // 文件打开时间
static uint32_t cTime; // 文件关闭时间
static uint32_t sTime; // 文件流式传输时间
static uint32_t frameInterval; // 帧间隔（微秒）

// SD 卡存储
uint8_t iSDbuffer[(RAMSIZE + CHUNK_HDR) * 2];
static size_t highPoint;
static File aviFile;
static char aviFileName[FILE_NAME_LEN];

// SD 回放
static File playbackFile;
static char partName[FILE_NAME_LEN];
static size_t readLen;
static uint8_t recFPS;
static uint32_t recDuration;
static uint8_t saveFPS = 99;
bool doPlayback = false; // 控制回放

// 任务控制
TaskHandle_t captureHandle = NULL;
TaskHandle_t playbackHandle = NULL;
static SemaphoreHandle_t readSemaphore;
static SemaphoreHandle_t playbackSemaphore;
SemaphoreHandle_t frameSemaphore[MAX_STREAMS] = {NULL};
SemaphoreHandle_t motionSemaphore = NULL;
SemaphoreHandle_t aviMutex = NULL;
static volatile bool isPlaying = false; // 控制应用中的回放
bool isCapturing = false;
bool stopPlayback = false; // 控制是否允许回放
bool timeLapseOn = false;
int dashCamOn = 0; // 是否启用及行车记录仪式连续录制的时长（分钟）

#ifndef AUXILIARY
framesize_t maxFS = FRAMESIZE_SVGA; // 默认值

/**************** 定时器与 ISR ************************/

static void IRAM_ATTR frameISR() {
  // 按当前帧率触发中断
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (isPlaying) xSemaphoreGiveFromISR (playbackSemaphore, &xHigherPriorityTaskWoken ); // 通知回放任务发送帧
  if (captureHandle != NULL) vTaskNotifyGiveFromISR(captureHandle, &xHigherPriorityTaskWoken); // 唤醒捕获任务处理帧
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void controlFrameTimer(bool restartTimer) {
  // 帧定时器控制
  static hw_timer_t* frameTimer = NULL;
  // 停止当前定时器
  if (frameTimer) {
    timerDetachInterrupt(frameTimer);
    timerEnd(frameTimer);
    frameTimer = NULL;
  }
  if (restartTimer) {
    // （重新）启动定时器中断以实现所需帧率
    frameTimer = timerBegin(OneMHz);
    if (frameTimer) {
      frameInterval = OneMHz / FPS; // 单位为微秒
      LOG_VRB("Frame timer interval %ums for FPS %u", frameInterval / 1000, FPS);
      timerAttachInterrupt(frameTimer, &frameISR);
      timerAlarm(frameTimer, frameInterval, true, 0); // 微秒
    } else LOG_ERR("Failed to setup frameTimer");
  }
}

/**************** 捕获 AVI  ************************/

static void openAvi() {
  // 根据日期和时间生成文件名，存储到日期文件夹
  // SD 卡上文件越多，打开新文件耗时越长
  oTime = millis();
  dateFormat(partName, sizeof(partName), true);
  STORAGE.mkdir(partName); // 若不存在则创建日期文件夹
  dateFormat(partName, sizeof(partName), false);
  // 用临时名称打开 avi 文件
  aviFile = STORAGE.open(AVITEMP, FILE_WRITE);
  oTime = millis() - oTime;
  LOG_VRB("File opening time: %ums", oTime);
#if INCLUDE_AUDIO
  startAudioRecord();
#endif
#if INCLUDE_TELEM
  haveSrt = startTelemetry();
#endif
  // 计数器初始化
  startTime = millis();
  frameCnt = fTimeTot = wTimeTot = dTimeTot = vidSize = 0;
  highPoint = AVI_HEADER_LEN; // 为 AVI 文件头预留空间
  prepAviIndex();
}

static inline bool doMonitor(bool capturing) {
  // 监控输入帧以检测运动
  static uint16_t motionCnt = 0;
  // 捕获期间停止监控 / 捕获前运动检测的比例
  uint16_t checkRate = (capturing) ? FPS * moveStopSecs : FPS / moveStartChecks;
  if (!checkRate) checkRate = 1;
  if (++motionCnt / checkRate) motionCnt = 0; // 该检测运动了
  return !(bool)motionCnt;
}

static void timeLapse(camera_fb_t* fb, bool tlStop = false) {
  // 录制延时摄影 AVI
  // 注意：若延时摄影录制期间 FPS 发生变化，
  //  延时摄影计数器不会随之修改
  static int frameCntTL, requiredFrames, intervalCnt = 0;
  static int intervalMark = tlSecsBetweenFrames * saveFPS;
  static File tlFile;
  static char TLname[FILE_NAME_LEN];
  if (tlStop) {
    // 受控关闭时强制保存文件
    intervalCnt = 0;
    requiredFrames = frameCntTL - 1;
  }
  if (timeLapseOn) {
    if (timeSynchronized) {
      if (!frameCntTL) {
        // 初始化延时摄影 AVI
        requiredFrames = tlDurationMins * 60 / tlSecsBetweenFrames;
        if (requiredFrames > maxFrames) {
          LOG_WRN("Frames required for timelapse %u reduced to max frame limit %u", requiredFrames, maxFrames);
          requiredFrames = maxFrames;
        }
        dateFormat(partName, sizeof(partName), true);
        STORAGE.mkdir(partName); // 若不存在则创建日期文件夹
        dateFormat(partName, sizeof(partName), false);
        int tlen = snprintf(TLname, FILE_NAME_LEN - 1, "%s_%s_%u_%u_T.%s",
                            partName, frameData[fsizePtr].frameSizeStr, tlPlaybackFPS, tlDurationMins, AVI_EXT);
        if (tlen > FILE_NAME_LEN - 1) LOG_WRN("file name truncated");
        if (STORAGE.exists(TLTEMP)) STORAGE.remove(TLTEMP);
        tlFile = STORAGE.open(TLTEMP, FILE_WRITE);
        tlFile.write(aviHeader, AVI_HEADER_LEN); // 为文件头预留空间
        prepAviIndex(true);
        LOG_INF("Started time lapse file %s, duration %u mins, for %u frames",  TLname, tlDurationMins, requiredFrames);
        frameCntTL++; // 防止重入
      }
      // 若为夜间则在捕获帧前开灯
#if INCLUDE_PERIPH
      if (nightTime && intervalCnt == intervalMark - (saveFPS / 2)) setLamp(lampLevel);
#endif
      if (intervalCnt > intervalMark) {
        // 将本帧保存到延时摄影 AVI
#if INCLUDE_PERIPH
        if (!lampNight) setLamp(0);
#endif
        uint8_t hdrBuff[CHUNK_HDR];
        memcpy(hdrBuff, dcBuf, 4);
        // 将 jpeg 末尾对齐到 4 字节边界（AVI 要求）
        uint16_t filler = (4 - (fb->len & 0x00000003)) & 0x00000003;
        uint32_t jpegSize = fb->len + filler;
        memcpy(hdrBuff + 4, &jpegSize, 4);
        tlFile.write(hdrBuff, CHUNK_HDR); // jpeg 帧信息
        tlFile.write(fb->buf, jpegSize);
        buildAviIdx(jpegSize, true, true); // 保存该帧的 avi 索引
        frameCntTL++;
        intervalCnt = 0;
        intervalMark = tlSecsBetweenFrames * saveFPS;  // FPS 变化时重新计算
      }
      intervalCnt++;
      if (frameCntTL > requiredFrames) {
        // 完成延时摄影录制
        xSemaphoreTake(aviMutex, portMAX_DELAY);
        buildAviHdr(tlPlaybackFPS, fsizePtr, --frameCntTL, true);
        xSemaphoreGive(aviMutex);
        // 追加索引
        finalizeAviIndex(frameCntTL, true);
        size_t idxLen = 0;
        do {
          idxLen = writeAviIndex(iSDbuffer, RAMSIZE, true);
          tlFile.write(iSDbuffer, idxLen);
        } while (idxLen > 0);
        // 追加文件头
        tlFile.seek(0, SeekSet); // 文件起始
        tlFile.write(aviHeader, AVI_HEADER_LEN);
        tlFile.close();
        STORAGE.rename(TLTEMP, TLname);
        frameCntTL = intervalCnt = 0;
        LOG_INF("Finished time lapse: %s", TLname);
#if INCLUDE_FTP_HFS
        if (autoUpload) fsStartTransfer(TLname); // 若请求则传输到远程 FTP 服务器
#endif
      }
    }
  } else frameCntTL = intervalCnt = 0;
}

void keepFrame(camera_fb_t* fb) {
  // 保留所需帧供外部服务器告警使用
  if (fb->len < maxAlertBuffSize && alertBuffer != NULL) {
    memcpy(alertBuffer, fb->buf, fb->len);
    alertBufferSize = fb->len;
  }
}

static void saveFrame(camera_fb_t* fb) {
  // 将帧保存到 SD 卡
  uint32_t fTime = millis();
  // 将 jpeg 末尾对齐到 4 字节边界（AVI 要求）
  uint16_t filler = (4 - (fb->len & 0x00000003)) & 0x00000003;
  size_t jpegSize = fb->len + filler;
  // 添加 avi 帧头
  memcpy(iSDbuffer + highPoint, dcBuf, 4);
  memcpy(iSDbuffer + highPoint + 4, &jpegSize, 4);
  highPoint += CHUNK_HDR;
  if (highPoint >= RAMSIZE) {
    // 标记溢出缓冲区
    highPoint -= RAMSIZE;
    aviFile.write(iSDbuffer, RAMSIZE);
    // 将溢出部分移到缓冲区开头
    memcpy(iSDbuffer, iSDbuffer + RAMSIZE, highPoint);
  }
  // 添加帧内容
  size_t jpegRemain = jpegSize;
  uint32_t wTime = millis();
  while (jpegRemain >= RAMSIZE - highPoint) {
    // 缓冲区填满 RAMSIZE 时写入 SD
    memcpy(iSDbuffer + highPoint, fb->buf + jpegSize - jpegRemain, RAMSIZE - highPoint);
    aviFile.write(iSDbuffer, RAMSIZE);
    jpegRemain -= RAMSIZE - highPoint;
    highPoint = 0;
  }
  wTime = millis() - wTime;
  wTimeTot += wTime;
  LOG_VRB("SD storage time %u ms", wTime);
  // 剩余部分或小帧
  memcpy(iSDbuffer + highPoint, fb->buf + jpegSize - jpegRemain, jpegRemain);
  highPoint += jpegRemain;

  buildAviIdx(jpegSize); // 保存该帧的 avi 索引
  vidSize += jpegSize + CHUNK_HDR;
  frameCnt++;
  fTime = millis() - fTime - wTime;
  fTimeTot += fTime;
  LOG_VRB("Frame processing time %u ms", fTime);
  LOG_VRB("============================");
}

static bool closeAvi() {
  // 关闭已录制的文件
  uint32_t vidDuration = millis() - startTime;
  uint32_t vidDurationSecs = lround(vidDuration / 1000.0);
  logLine();
  LOG_VRB("Capture time %u, min seconds: %u ", vidDurationSecs, minSeconds);

  cTime = millis();
  // 将剩余帧内容写入 SD
  aviFile.write(iSDbuffer, highPoint);
  size_t readLen = 0;
  bool haveWav = false;
#if INCLUDE_AUDIO
  // 若存在则添加 wav 文件
  finishAudioRecord(true);
  haveWav = haveWavFile();
  if (haveWav) {
    do {
      readLen = writeWavFile(iSDbuffer, RAMSIZE);
      aviFile.write(iSDbuffer, readLen);
    } while (readLen > 0);
  }
#endif
  // 保存 avi 索引
  finalizeAviIndex(frameCnt);
  do {
    readLen = writeAviIndex(iSDbuffer, RAMSIZE);
    if (readLen) aviFile.write(iSDbuffer, readLen);
  } while (readLen > 0);
  // 在文件开头保存 avi 文件头
  float actualFPS = (1000.0f * (float)frameCnt) / ((float)vidDuration);
  uint8_t actualFPSint = (uint8_t)(lround(actualFPS));
  xSemaphoreTake(aviMutex, portMAX_DELAY);
  buildAviHdr(actualFPSint, fsizePtr, frameCnt);
  xSemaphoreGive(aviMutex);
  aviFile.seek(0, SeekSet); // 文件起始
  aviFile.write(aviHeader, AVI_HEADER_LEN);
  aviFile.close();
  LOG_VRB("Final SD storage time %lu ms", millis() - cTime);
  uint32_t hTime = millis();
#if INCLUDE_MQTT
  if (mqtt_active) {
    sprintf(jsonBuff, "{\"RECORD\":\"OFF\", \"TIME\":\"%s\"}", esp_log_system_timestamp());
    mqttPublish(jsonBuff);
    mqttPublishPath("record", "off");
  }
#endif
  if (vidDurationSecs >= minSeconds) {
    // 文件名包含实际日期时间、FPS、时长和帧数
    int alen = snprintf(aviFileName, FILE_NAME_LEN - 1, "%s_%s_%u_%lu%s%s%s.%s",
                        partName, frameData[fsizePtr].frameSizeStr, actualFPSint, vidDurationSecs,
                        haveWav ? "_S" : "", haveSrt ? "_M" : "", dashCamOn ? "_C" : "", AVI_EXT);
    if (alen > FILE_NAME_LEN - 1) LOG_WRN("file name truncated");
    STORAGE.rename(AVITEMP, aviFileName);
    LOG_VRB("AVI close time %lu ms", millis() - hTime);
    cTime = millis() - cTime;
#if INCLUDE_TELEM
    stopTelemetry(aviFileName);
#endif
    if (dashCamOn) forceRecord = true; // 重新开始连续录制
    else {
      // AVI 统计
      LOG_INF("******** AVI recording stats ********");
      LOG_ALT("Recorded %s", aviFileName);
      LOG_INF("AVI duration: %u secs", vidDurationSecs);
      LOG_INF("Number of frames: %u", frameCnt);
      LOG_INF("Required FPS: %u", FPS);
      LOG_INF("Actual FPS: %0.1f", actualFPS);
      LOG_INF("File size: %s", fmtSize(vidSize));
      if (frameCnt) {
        LOG_INF("Average frame length: %u bytes", vidSize / frameCnt);
        LOG_INF("Average frame monitoring time: %u ms", dTimeTot / frameCnt);
        LOG_INF("Average frame buffering time: %u ms", fTimeTot / frameCnt);
        LOG_INF("Average frame storage time: %u ms", wTimeTot / frameCnt);
      }
      LOG_INF("Average SD write speed: %u kB/s", ((vidSize / wTimeTot) * 1000) / 1024);
      LOG_INF("File open / completion times: %u ms / %u ms", oTime, cTime);
      LOG_INF("Busy: %u%%", std::min(100 * (wTimeTot + fTimeTot + dTimeTot + oTime + cTime) / vidDuration, (uint32_t)100));
      checkMemory();
      LOG_INF("*************************************");
      // 若请求则发送运动通知
#if INCLUDE_SMTP
      if (smtpUse) {
        // 发送带运动图像的邮件
        char subjectMsg[50];
        snprintf(subjectMsg, sizeof(subjectMsg) - 1, "from %s, in %s", hostName, aviFileName);
        emailAlert("Motion Alert", subjectMsg);
      } 
#endif
#if INCLUDE_TGRAM
      tgramAlert(aviFileName, "");
#endif
#if INCLUDE_FTP_HFS
      if (autoUpload) {
        if (deleteAfter) {
          // 问题 #380 - 若其他文件传输失败，则传输整个父文件夹
          dateFormat(partName, sizeof(partName), true);
          fsStartTransfer(partName);
        } else fsStartTransfer(aviFileName); // 将此文件传输到远程 FTP 服务器
      }
#endif
    }
    if (!checkFreeStorage()) doRecording = forceRecord = false;
    return true;
  } else {
    // 删除过短的文件（若存在）
    STORAGE.remove(AVITEMP);
    LOG_INF("Insufficient capture duration: %u secs", vidDurationSecs);
    return false;
  }
}

static boolean processFrame() {
  // 获取摄像头帧
  static bool haveMotion = false;
  bool res = true;
  uint32_t dTime = millis();

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == NULL || !fb->len || fb->len > maxFrameBuffSize) return false;
  timeLapse(fb);

  for (int i = 0; i < vidStreams; i++) {
    if (!streamBufferSize[i] && streamBuffer[i] != NULL) {
      memcpy(streamBuffer[i], fb->buf, fb->len);
      streamBufferSize[i] = fb->len;
      xSemaphoreGive(frameSemaphore[i]); // 通知帧已就绪可供流式传输
    }
  }
  if (doKeepFrame) {
    keepFrame(fb);
    doKeepFrame = false;
  }

  // 判断是否该检测运动变化
  int reasonId = 0;
  bool prevMotion = haveMotion;
  if (doMonitor(doRecording ? isCapturing : dbgMotion ? false : true)) {
    if (useMotion && checkMotion(fb, isCapturing)) reasonId = 1; // 每 N 帧检测 1 次
    if (!useMotion) checkMotion(fb, false, true); // 仅计算光照等级
#if INCLUDE_PERIPH
    if (pirUse && getPIRval()) reasonId = 2;
#endif
#if INCLUDE_I2C && (USE_MPU6050 || USE_MPU9250)
    if (accelUse && checkAccelMove()) reasonId = 3;
#endif
    haveMotion = (reasonId) ? true : false;
  }

  // 处理运动状态
  if (haveMotion && !prevMotion) {
    // 检测到运动开始
    keepFrame(fb);
#if INCLUDE_PERIPH
    buzzerAlert(true); // 若启用则鸣响蜂鸣器
    if (lampAuto && nightTime) setLamp(lampLevel);  // 若请求则在夜间开灯
#endif
  }
  if (!haveMotion) {
#if INCLUDE_PERIPH
    if (lampAuto) setLamp(0); // 关灯
    buzzerAlert(false); // 若仍在响则关闭蜂鸣器
#endif
  }

  // 录制状态
  bool prevCapture = isCapturing;
  isCapturing = haveMotion | forceRecord;
  if (isCapturing && !prevCapture) {
    // 发生新运动或按下录制按钮，开始录制
    stopPlaying(); // 终止任何回放
    stopPlayback = true; // 阻止后续回放
    if (!dashCamOn) LOG_ALT("Capture started by %s%s%s%s", reasonId == 0 ? "Button" : "", reasonId == 1 ? "Camera " : "", reasonId == 2 ? "PIR" : "", reasonId == 3 ? "Accelerometer" : "");
#if INCLUDE_MQTT
    if (mqtt_active) {
      sprintf(jsonBuff, "{\"RECORD\":\"ON\", \"TIME\":\"%s\"}", esp_log_system_timestamp());
      mqttPublish(jsonBuff);
      mqttPublishPath("record", "on");
    }
#endif
    wsAsyncSendJson("ustatus", "\"showRecord\":1");
    openAvi();
  }

  if (isCapturing) {
    // 正在捕获
    showProgress();
    if (frameCnt < frameLimit) {
      dTimeTot += millis() - dTime;
      saveFrame(fb);
      if (frameCnt == frameLimit) {
        // 达到上限，停止保存本 avi 的帧
        isCapturing = forceRecord = false;
        if (!dashCamOn) {
          logLine();
          LOG_WRN("Auto closed recording after %u frames", frameLimit);
        }
      }
    }
#if INCLUDE_PERIPH
    if (buzzerUse && frameCnt / FPS >= buzzerDuration) buzzerAlert(false); // 给定时间后关闭
#endif
  }

  esp_camera_fb_return(fb);
  if (!isCapturing && prevCapture) {
    // 完成录制（正常或强制）
    closeAvi();
    wsAsyncSendJson("ustatus", "\"showRecord\":0");
    stopPlayback = false; // 允许回放
  }
  return res;
}

static void captureTask(void* parameter) {
  // 到捕获帧时间时由帧定时器唤醒
  uint32_t ulNotifiedValue;
  while (true) {
    ulNotifiedValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (ulNotifiedValue > FB_CNT) ulNotifiedValue = FB_CNT; // FPS 过高时防止队列过大
    // 若任务被 SD 写入或 jpeg 解码延迟，可能有多于一个 ISR 待处理
    while (ulNotifiedValue-- > 0) processFrame();
  }
  vTaskDelete(NULL);
}

uint8_t setFPS(uint8_t val) {
  // 更改或读取 FPS 值
  if (val) {
    FPS = val;
    // 更改驱动任务的帧定时器
    controlFrameTimer(true);
    saveFPS = FPS; // 回放后用于恢复 FPS
  }
  return FPS;
}

uint8_t setFPSlookup(uint8_t val) {
  // 根据帧尺寸查表设置 FPS
  fsizePtr = val;
  return setFPS(frameData[fsizePtr].defaultFPS);
}

/********************** 将 AVI 作为 MJPEG 回放 ***********************/

static fnameStruct extractMeta(const char* fname) {
  // 从 avi 文件名提取 FPS、时长和帧数
  fnameStruct fnameMeta;
  char fnameStr[FILE_NAME_LEN];
  strcpy(fnameStr, fname);
  // 将所有 '_' 替换为空格供 sscanf 使用
  replaceChar(fnameStr, '_', ' ');
  int items = sscanf(fnameStr, "%*s %*s %*s %hhu %lu", &fnameMeta.recFPS, &fnameMeta.recDuration);
  if (items != 2) LOG_ERR("failed to parse %s, items %u", fname, items);
  return fnameMeta;
}

static void playbackFPS(const char* fname) {
  // 从文件名提取元数据以开始回放
  fnameStruct fnameMeta = extractMeta(fname);
  recFPS = fnameMeta.recFPS;
  if (recFPS < 1) recFPS = 1;
  recDuration = fnameMeta.recDuration;
  // 临时改为录制时的帧率
  FPS = recFPS;
  controlFrameTimer(true); // 设置帧定时器
}

static void readSD() {
  // 为回放读取 SD 上下一簇数据
  uint32_t rTime = millis();
  // 先读到临时 DRAM，再复制到 PSRAM
  readLen = 0;
  if (!stopPlayback) {
    readLen = playbackFile.read(iSDbuffer + RAMSIZE + CHUNK_HDR, RAMSIZE);
    LOG_VRB("SD read time %lu ms", millis() - rTime);
  }
  wTimeTot += millis() - rTime;
  xSemaphoreGive(readSemaphore); // 通知数据已就绪
  delay(10);
}


void openSDfile(const char* streamFile) {
  // 打开 SD 上选定文件以供流式传输
  if (stopPlayback) LOG_WRN("Playback refused - capture in progress");
  else {
    stopPlaying(); // 若已在运行则先停止
    strcpy(aviFileName, streamFile);
    LOG_INF("Playing %s", aviFileName);
    playbackFile = STORAGE.open(aviFileName, FILE_READ);
    playbackFile.seek(AVI_HEADER_LEN, SeekSet); // 跳过文件头
    playbackFPS(aviFileName);
    isPlaying = true; // 回放状态
    doPlayback = true; // 控制回放
    readSD(); // 预加载回放任务
  }
}

mjpegStruct getNextFrame(bool firstCall) {
  // 已打开 avi 且就绪时按需获取下一簇数据
  mjpegStruct mjpegData;
  static bool remainingBuff;
  static bool completedPlayback; // 表示回放已完成
  static size_t buffOffset;
  static uint32_t hTimeTot;
  static uint32_t tTimeTot;
  static uint32_t hTime;
  static size_t remainingFrame;
  static size_t buffLen;
  const uint32_t dcVal = 0x63643030; // 00dc 标记的值
  if (firstCall) {
    sTime = millis();
    hTime = millis();
    remainingBuff = completedPlayback = false;
    frameCnt = remainingFrame = vidSize = buffOffset = 0;
    wTimeTot = fTimeTot = hTimeTot = tTimeTot = 1; // 避免除以 0
  }
  LOG_VRB("http send time %lu ms", millis() - hTime);
  hTimeTot += millis() - hTime;
  uint32_t mTime = millis();
  if (!stopPlayback) {
    // 继续发送帧
    if (!remainingBuff) {
      // 从 SD 加载更多数据
      mTime = millis();
      // 将末尾字节移到缓冲区开头，以防 jpeg 标记在缓冲区末尾
      memcpy(iSDbuffer, iSDbuffer + RAMSIZE, CHUNK_HDR);
      xSemaphoreTake(readSemaphore, portMAX_DELAY); // 等待 SD 读取完成
      buffLen = readLen;
      LOG_VRB("SD wait time %lu ms", millis() - mTime);
      wTimeTot += millis() - mTime;
      mTime = millis();
      // 缓冲区重叠 CHUNK_HDR，防止 jpeg 标记被拆到两个缓冲区之间
      memcpy(iSDbuffer + CHUNK_HDR, iSDbuffer + RAMSIZE + CHUNK_HDR, buffLen); // 从双缓冲加载新簇
      LOG_VRB("memcpy took %lu ms for %u bytes", millis() - mTime, buffLen);
      fTimeTot += millis() - mTime;
      remainingBuff = true;
      if (buffOffset > RAMSIZE) buffOffset = 4; // 特殊情况：标记跨越缓冲区末尾
      else buffOffset = frameCnt ? 0 : CHUNK_HDR; // 仅第一帧之前
      xTaskNotifyGive(playbackHandle); // 唤醒任务读取下一簇 - 设置 readLen
    }
    mTime = millis();
    if (!remainingFrame) {
      // 位于 jpeg 帧标记起始处
      uint32_t inVal;
      memcpy(&inVal, iSDbuffer + buffOffset, 4);
      if (inVal != dcVal) {
        // 已到达待流式传输帧的末尾
        mjpegData.buffLen = buffOffset; // 最后一帧 jpeg 的剩余部分
        mjpegData.buffOffset = 0; // 从缓冲区起始
        mjpegData.jpegSize = 0;
        stopPlayback = completedPlayback = true;
        return mjpegData;
      } else {
        // 获取 jpeg 帧大小
        uint32_t jpegSize;
        memcpy(&jpegSize, iSDbuffer + buffOffset + 4, 4);
        remainingFrame = jpegSize;
        vidSize += jpegSize;
        buffOffset += CHUNK_HDR; // 跳过标记
        mjpegData.jpegSize = jpegSize; // 向 Web 服务器表示 JPEG 开始
        mTime = millis();
        // 在 playbackSemaphore 上等待以进行速率控制
        xSemaphoreTake(playbackSemaphore, portMAX_DELAY);
        LOG_VRB("frame timer wait %lu ms", millis() - mTime);
        tTimeTot += millis() - mTime;
        frameCnt++;
        showProgress();
      }
    } else mjpegData.jpegSize = 0; // 帧内
    // 确定发送给 Web 服务器的数据量
    if (buffOffset > RAMSIZE) mjpegData.buffLen = 0; // 特殊情况
    else mjpegData.buffLen = (remainingFrame > buffLen - buffOffset) ? buffLen - buffOffset : remainingFrame;
    mjpegData.buffOffset = buffOffset; // 从此处开始
    remainingFrame -= mjpegData.buffLen;
    buffOffset += mjpegData.buffLen;
    if (buffOffset >= buffLen) remainingBuff = false;
  } else {
    // 完成，关闭用于流式传输的 SD 文件
    playbackFile.close();
    logLine();
    if (!completedPlayback) LOG_INF("Force close playback");
    uint32_t playDuration = (millis() - sTime) / 1000;
    uint32_t totBusy = wTimeTot + fTimeTot + hTimeTot;
    LOG_INF("******** AVI playback stats ********");
    LOG_INF("Playback %s", aviFileName);
    LOG_INF("Recorded FPS %u, duration %u secs", recFPS, recDuration);
    LOG_INF("Playback FPS %0.1f, duration %u secs", (float)frameCnt / playDuration, playDuration);
    LOG_INF("Number of frames: %u", frameCnt);
    if (frameCnt) {
      LOG_INF("Average SD read speed: %u kB/s", ((vidSize / wTimeTot) * 1000) / 1024);
      LOG_INF("Average frame SD read time: %u ms", wTimeTot / frameCnt);
      LOG_INF("Average frame processing time: %u ms", fTimeTot / frameCnt);
      LOG_INF("Average frame delay time: %u ms", tTimeTot / frameCnt);
      LOG_INF("Average http send time: %u ms", hTimeTot / frameCnt);
      LOG_INF("Busy: %u%%", min(100 * totBusy / (totBusy + tTimeTot), (uint32_t)100));
    }
    checkMemory();
    LOG_INF("*************************************\n");
    setFPS(saveFPS); // 与浏览器重新对齐
    stopPlayback = isPlaying = false;
    mjpegData.buffLen = mjpegData.buffOffset = 0; // 表示 JPEG 结束
  }
  hTime = millis();
  delay(1);
  return mjpegData;
}

void stopPlaying() {
  if (isPlaying) {
    // 强制停止当前正在进行的回放
    stopPlayback = true;
    // 等待干净停止，但避免无限循环
    uint32_t timeOut = millis();
    while (doPlayback && millis() - timeOut < MAX_FRAME_WAIT) delay(10);
    if (doPlayback) {
      // 尚未关闭，强制关闭
      logLine();
      LOG_WRN("Force closed playback");
      doPlayback = false; // 停止 Web 服务器回放
      setFPS(saveFPS);
      xSemaphoreGive(playbackSemaphore);
      xSemaphoreGive(readSemaphore);
      delay(200);
    }
    stopPlayback = false;
    isPlaying = false;
  }
}

static void playbackTask(void* parameter) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    readSD();
  }
  vTaskDelete(NULL);
}

/******************* 启动 ********************/

static bool startSDtasks() {
  // 管理 SD 卡操作的任务
  xTaskCreateWithCaps(&playbackTask, "playbackTask", PLAYBACK_STACK_SIZE, NULL, PLAY_PRI, &playbackHandle, HEAP_MEM);
  xTaskCreate(&captureTask, "captureTask", CAPTURE_STACK_SIZE, NULL, CAPTURE_PRI, &captureHandle);
  if (captureHandle == NULL) {
    // 通常是内存不足
    OTAprereq();
    return false;
  }
  // 根据配置设置初始摄像头帧尺寸和 FPS
  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, (framesize_t)fsizePtr);
  setFPS(FPS);
  debugMemory("startSDtasks");
  return true;
}

bool prepRecording() {
  // AVI 捕获的初始化与准备
  readSemaphore = xSemaphoreCreateBinary();
  playbackSemaphore = xSemaphoreCreateBinary();
  aviMutex = xSemaphoreCreateMutex();
  motionSemaphore = xSemaphoreCreateBinary();
  for (int i = 0; i < vidStreams; i++) frameSemaphore[i] = xSemaphoreCreateBinary();
  reloadConfigs(); // 应用摄像头配置
  if (!startSDtasks()) return false;
#if INCLUDE_TINYML
  LOG_INF("%sUsing TinyML", mlUse ? "" : "Not ");
#endif

  if ((fs::LittleFSFS*)&STORAGE == &LittleFS) {
    // 禁止录制
    sdFreeSpaceMode = 0;
    sdMinCardFreeSpace = 0;
    doRecording = false;
    sdLog = false;
    useMotion = false;
    LOG_WRN("Recording disabled as no SD card");
  } else {
    LOG_INF("To record new AVI, do one of:");
    LOG_INF("- press Start Recording on web page");
#if INCLUDE_PERIPH
    if (pirUse) {
      LOG_INF("- attach PIR to pin %u", pirPin);
      LOG_INF("- raise pin %u to 3.3V", pirPin);
    }
#endif
#if INCLUDE_I2C
    if (accelUse) LOG_INF("- accelerometer movement");
#endif
    if (useMotion) LOG_INF("- move in front of camera");
  }
  logLine();
  debugMemory("prepRecording");
  return true;
}

void appShutdown() {
  timeLapse(NULL, true);
}

static void deleteTask(TaskHandle_t thisTaskHandle) {
  // 若尝试删除空 thisTaskHandle 会挂起
  if (thisTaskHandle != NULL) vTaskDelete(thisTaskHandle);
  thisTaskHandle = NULL;
}

void endTasks() {
  for (int i = 0; i < numStreams; i++) deleteTask(sustainHandle[i]);
  deleteTask(captureHandle);
  deleteTask(playbackHandle);
#if INCLUDE_TELEM
  deleteTask(telemetryHandle);
#endif
#if INCLUDE_PERIPH
  deleteTask(DS18B20handle);
  deleteTask(servoHandle);
  deleteTask(stickHandle);
#endif
#if INCLUDE_SMTP
  deleteTask(emailHandle);
#endif
#if INCLUDE_FTP_HFS
  deleteTask(fsHandle);
#endif
#if INCLUDE_TGRAM
  deleteTask(telegramHandle);
#endif
#if INCLUDE_AUDIO
  deleteTask(audioHandle);
#endif
}

void OTAprereq() {
  // 停止定时器 ISR 并释放堆空间，否则 esp32 会崩溃
  doPlayback = forceRecord = false;
  controlFrameTimer(false);
#if INCLUDE_PERIPH
  setStickTimer(false);
#endif
  stopPing();
  endTasks();
  esp_camera_deinit();
  delay(100);
}

#ifdef CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3
// 此板卡具有可配置电源
// 需安装以下库
#include "DFRobot_AXP313A.h" // 库源码见 https://github.com/cdjq/DFRobot_AXP313A
DFRobot_AXP313A axp;

static bool camPower() {
  int pwrRetry = 5;
  while (pwrRetry) {
    if (axp.begin() == 0) {
      axp.enableCameraPower(axp.eOV2640);
      return true;
    } else {
      delay(1000);
      pwrRetry--;
    }
  }
  LOG_ERR("Failed to power up camera");
  return false;
}

#else

static bool camPower() {
  // 占位
  return true;
}
#endif

static esp_err_t changeXCLK(camera_config_t config) {
  // 原始默认配置无法生成超过 20MHz 的时钟，此处强制配置
  if (config.xclk_freq_hz <= 20 * OneMHz) return ESP_OK;
  esp_err_t res = ESP_OK;
  // 反初始化现有 LEDC 配置
  ledc_stop(LEDC_LOW_SPEED_MODE, config.ledc_channel, 0);
  delay(5);
  // 配置 LEDC 定时器
  ledc_timer_config_t ledc_timer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_1_BIT,
    .timer_num = config.ledc_timer,
    .freq_hz = (uint32_t)config.xclk_freq_hz,
    .clk_cfg = LEDC_AUTO_CLK
  };
  res = ledc_timer_config(&ledc_timer);
  if (res != ESP_OK) {
    LOG_ERR("Failed to configure timer %s", espErrMsg(res));
    return res;
  }
  // 配置 LEDC 通道
  ledc_channel_config_t ledc_channel = {
    .gpio_num = XCLK_GPIO_NUM,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = config.ledc_channel,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = config.ledc_timer,
    .duty = 1,  // 1 位分辨率下 50% 占空比
    .hpoint = 0
  };
  res = ledc_channel_config(&ledc_channel);
  if (res != ESP_OK) {
    LOG_ERR("Failed to configure channel %s", espErrMsg(res));
    return res;
  }
  delay(200); // 依据数据手册，配置稳定需 < 300 ms，此处用 200 ms，无妨
  return res;
}

bool prepCam() {
  // 根据型号和板卡初始化摄像头
  if (FRAMESIZE_INVALID != sizeof(frameData) / sizeof(frameData[0]))
    LOG_ERR("framesize_t entries %d != frameData entries %d", FRAMESIZE_INVALID, sizeof(frameData) / sizeof(frameData[0]));
  if (!camPower()) return false;
#if INCLUDE_I2C
  if (shareI2C(SIOD_GPIO_NUM, SIOC_GPIO_NUM)) {
    // 若共享，则让摄像头使用共享 I2C
    siodGpio = -1;
    siocGpio = -1;
  }
#endif

  bool res = false;
  // 缓冲区大小取决于 PSRAM 容量（2M、4M 或 8M）
  // FRAMESIZE_QSXGA = 1MB，FRAMESIZE_UXGA = 375KB（JPEG 格式）
  // 豪威 Omnivision 摄像头型号
  maxFS = FRAMESIZE_SVGA; // 2M
  if (ESP.getPsramSize() > 5 * ONEMEG) maxFS = FRAMESIZE_QSXGA; // 8M
  else if (ESP.getPsramSize() > 3 * ONEMEG) maxFS = FRAMESIZE_UXGA; // 4M
#ifdef USE_PY260
  // PY260 摄像头帧尺寸不同
  maxFS = (ESP.getPsramSize() > 5 * ONEMEG) ? FRAMESIZE_5MP : FRAMESIZE_HD;
#endif
  // 根据可用最大帧尺寸定义缓冲区大小，esp32-camera/driver/cam_hal.c: cam_obj->recv_size
  maxFrameBuffSize = maxAlertBuffSize = frameData[maxFS].frameWidth * frameData[maxFS].frameHeight / 5;
  if (alertBuffer == NULL) alertBuffer = psramFound() ? (byte*)ps_malloc(maxAlertBuffSize) : (byte*)malloc(maxAlertBuffSize);
  LOG_INF("Max frame size for %s PSRAM is %s ", fmtSize(ESP.getPsramSize()), frameData[maxFS].frameSizeStr);

  // 配置摄像头
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_1;
  config.ledc_timer = LEDC_TIMER_1;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = siodGpio;
  config.pin_sccb_scl = siocGpio;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = xclkMhz * OneMHz;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  // 以高规格初始化以预分配更大缓冲区
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.frame_size = maxFS;
  config.jpeg_quality = 10;
  config.fb_count = FB_CNT;
  config.sccb_i2c_port = 0;// 使用 I2C 0，确保端口明确

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // 摄像头初始化
  esp_err_t err = ESP_FAIL;
  uint8_t retries = 2;
  while (retries && err != ESP_OK) {
    err = esp_camera_init(&config);
    if (err == ESP_OK) err = changeXCLK(config);
    if (err != ESP_OK) {
      // 对摄像头断电再上电（若引脚已连接）
#if (defined(PWDN_GPIO_NUM)) && (PWDN_GPIO_NUM > -1) // 两项检查均需要；若向 digitalWrite 传入 -1 可能崩溃
      digitalWrite(PWDN_GPIO_NUM, 1);
      delay(100);
      digitalWrite(PWDN_GPIO_NUM, 0);
      delay(100);
#else
      delay(200);
#endif
      retries--;
    }
  }
  uint16_t PID = 0;
  if (err != ESP_OK) snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Camera init error 0x%X:%s on %s", err, espErrMsg(err), CAM_BOARD);
  else {
    sensor_t* s = esp_camera_sensor_get();
    if (s == NULL) snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to access camera data on %s", CAM_BOARD);
    else {
      PID = s->id.PID;
      switch (PID) {
        case (OV2640_PID):
          strcpy(camModel, "OV2640");
          break;
        case (OV3660_PID):
          strcpy(camModel, "OV3660");
          break;
        case (OV5640_PID): {
          strcpy(camModel, "OV5640");
#if INCLUDE_AF
          // 为 OV5640 启用自动对焦（若支持）- 见 https://github.com/0015/ESP32-OV5640-AF
          ov5640AF.start(s);
          uint8_t res = ov5640AF.focusInit();
          if (res == 0) res = ov5640AF.autoFocusMode();
          res == 0 ? LOG_INF("OV5640 Auto Focus available") : LOG_WRN("OV5640 Auto Focus fail: %d", res);
#endif
          break;
        }
        case (MEGA_CCM_PID):
          strcpy(camModel, "PY260");
          break;
        default:
          // 无法识别
          sprintf(camModel, "PID=0x%X", s->id.PID);
          break;
      }
      // 将帧尺寸设为配置值
      char fsizePtr[4];
      if (retrieveConfigVal("framesize", fsizePtr)) s->set_framesize(s, (framesize_t)(atoi(fsizePtr)));
      else s->set_framesize(s, FRAMESIZE_VGA);

      // 型号特定校正
      if (PID == OV3660_PID) {
        // 初始传感器垂直翻转且色彩略过饱和
        s->set_vflip(s, 1);// 翻转回来
        s->set_brightness(s, 1);// 略微提高亮度
        s->set_saturation(s, -2);// 降低饱和度
      }

#if defined(CAMERA_MODEL_M5STACK_WIDE)
      s->set_vflip(s, 1);
      s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
      s->set_vflip(s, 1);
      s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
      s->set_vflip(s, 1);
#endif
      res = true;
    }
  }
  // 检查摄像头数据是否可访问
  if (res) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == NULL) {
      // 通常是摄像头硬件/排线故障
      snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to get camera frame - check camera hardware");
    } else {
      esp_camera_fb_return(fb);
      fb = NULL;
      res = true;
      LOG_INF("Camera model %s ready @ %uMHz", camModel, xclkMhz);
      if (timeLapseOn) dashCamOn = 0;
      if (dashCamOn) {
        timeLapseOn = useMotion = false; // 禁用延时摄影和运动录制
        frameLimit = FPS * dashCamOn * 60; // 若之后改 FPS 且不重启，frameLimit 不变
        if (frameLimit > maxFrames) {
          frameLimit = maxFrames;
          LOG_WRN("Max continuous recording time interval is %d mins", frameLimit / FPS / 60);
        } else LOG_INF("Do continuous recording at %d min intervals", dashCamOn);
        forceRecord = true;
      } else frameLimit = maxFrames;
    }
  }
  debugMemory("prepCam");
  return res;
}

#else

// 占位函数
void appShutdown() {}
void OTAprereq() {}
uint8_t setFPSlookup(uint8_t val) {
  return 0;
}

#endif
