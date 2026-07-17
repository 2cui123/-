// 库地址：https://github.com/rjsachse/ESP32-RTSPServer.git 或在 Arduino 库中

  // 初始化 RTSP 服务器
  /**
   * @brief 使用指定配置初始化 RTSP 服务器。
   * 
   * 可传入具体参数，或在调用 begin() 前直接在 RTSPServer 实例中设置参数。
   * 若某参数未显式设置，则使用默认值。
   * 
   * @param transport 传输类型。默认为 VIDEO_AND_SUBTITLES。可选：VIDEO_ONLY、AUDIO_ONLY、VIDEO_AND_AUDIO、VIDEO_AND_SUBTITLES、AUDIO_AND_SUBTITLES、VIDEO_AUDIO_SUBTITLES。
   * @param rtspPort 使用的 RTSP 端口。默认为 554。
   * @param sampleRate 音频流采样率。默认为 0；使用音频时必须传入或设置。
   * @param port1 第一个端口（根据传输类型用于视频、音频或字幕）。默认为 5430。
   * @param port2 第二个端口（根据传输类型用于音频或字幕）。默认为 5432。
   * @param port3 第三个端口（用于字幕）。默认为 5434。
   * @param rtpIp RTP 组播流 IP 地址。默认为 IPAddress(239, 255, 0, 1)。
   * @param rtpTTL RTP 组播包 TTL 值。默认为 64。
   * @return 初始化成功返回 true，否则 false。

#include "appGlobals.h"

#if INCLUDE_RTSP
#if __has_include("../libraries/ESP32-RTSPServer/src/ESP32-RTSPServer.h") 
#include <ESP32-RTSPServer.h> 
#else
#error "Need to install ESP32-RTSPServer library"
#endif
RTSPServer rtspServer;

// 注释掉以对所有传输（TCP、UDP、组播）启用多客户端
//#define OVERRIDE_RTSP_SINGLE_CLIENT_MODE

bool rtspVideo;
bool rtspAudio;
bool rtspSubtitles;
int rtspPort;
uint16_t rtpVideoPort;
uint16_t rtpAudioPort;
uint16_t rtpSubtitlesPort;
char RTP_ip[MAX_IP_LEN];
uint8_t rtspMaxClients;
uint8_t rtpTTL;
char RTSP_Name[MAX_HOST_LEN-1] = "";
char RTSP_Pass[MAX_PWD_LEN-1] = "";
bool useAuth;

IPAddress rtpIp;
char transportStr[30];  // 按需调整大小

RTSPServer::TransportType determineTransportType() { 
  if (rtspVideo && rtspAudio && rtspSubtitles) { 
    strcpy(transportStr, "s: Video, Audio & Subtitles");
    return RTSPServer::VIDEO_AUDIO_SUBTITLES; 
  } else if (rtspVideo && rtspAudio) { 
    strcpy(transportStr, "s: Video & Audio");
    return RTSPServer::VIDEO_AND_AUDIO; 
  } else if (rtspVideo && rtspSubtitles) { 
    strcpy(transportStr, "s: Video & Subtitles");
    return RTSPServer::VIDEO_AND_SUBTITLES; 
  } else if (rtspAudio && rtspSubtitles) { 
    strcpy(transportStr, "s: Audio & Subtitles");
    return RTSPServer::AUDIO_AND_SUBTITLES; 
  } else if (rtspVideo) { 
    strcpy(transportStr, ": Video");
    return RTSPServer::VIDEO_ONLY; 
  } else if (rtspAudio) { 
    strcpy(transportStr, ": Audio");
    return RTSPServer::AUDIO_ONLY; 
  } else if (rtspSubtitles) { 
    strcpy(transportStr, ": Subtitles");
    return RTSPServer::SUBTITLES_ONLY; 
  } else { 
    strcpy(transportStr, ": None!");
    return RTSPServer::NONE; 
  }
}

#ifdef ISCAM

static void sendRTSPVideo(void* p) {
  // 按当前帧率通过 RTSP 发送 JPEG 帧
  uint8_t taskNum = 1;
  streamBufferSize[taskNum] = 0;
  while (true) {
    if (frameSemaphore[taskNum] != NULL) {
      if (xSemaphoreTake(frameSemaphore[taskNum], pdMS_TO_TICKS(MAX_FRAME_WAIT)) == pdTRUE) {
        if (streamBufferSize[taskNum] && rtspServer.readyToSendFrame()) {
          // 使用 processFrame() 存储的帧
          rtspServer.sendRTSPFrame(streamBuffer[taskNum], streamBufferSize[taskNum], quality, frameData[fsizePtr].frameWidth, frameData[fsizePtr].frameHeight);
        }
      }
      streamBufferSize[taskNum] = 0; 
    } else delay(100);
  }
  vTaskDelete(NULL);
}

void sendRTSPSubtitles(void* arg) { 
  char data[100];
  time_t currEpoch = getEpoch();
  size_t len = strftime(data, 12, "%H:%M:%S  ", localtime(&currEpoch));
  len += sprintf(data + len, "FPS: %lu", rtspServer.rtpFps);
#if INCLUDE_TELEM
  // 添加遥测数据
  if (teleUse) {
    storeSensorData(true);
    if (srtBytes) len += sprintf(data + len, "%s", (const char*)srtBuffer);
    srtBytes = 0;
  }
#endif
  rtspServer.sendRTSPSubtitles(data, len);
}

static void startRTSPSubtitles(void* arg) {
  rtspServer.startSubtitlesTimer(sendRTSPSubtitles); // 1 秒周期
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  vTaskDelete(NULL); // 不会执行到
}

#endif

static void sendRTSPAudio(void* p) {
#if INCLUDE_AUDIO
  // 通过 RTSP 发送音频块
  audioBytes = 0;
  while (true) {
    if (micGain && audioBytes && rtspServer.readyToSendAudio()) {
      rtspServer.sendRTSPAudio((int16_t*)audioBuffer, audioBytes);
      audioBytes = 0;
    } 
    delay(20);
  }
#endif
  vTaskDelete(NULL);
}

static void initRTSP() {
#ifdef ISVC
  // 为 VC 使用常量初始化 RTSP 服务器
  rtspVideo = rtspSubtitles = false;
  rtspAudio = true;
  strcpy(RTP_ip, "239.255.0.1");
  rtspPort = 554;
  rtpAudioPort = 5432; 
  rtpVideoPort = 0; 
  rtpSubtitlesPort = 0;
  rtspMaxClients = 1;
  rtpTTL = 1; 
#endif
}

void prepRTSP() {
  initRTSP();
  useAuth = rtspServer.setCredentials(RTSP_Name, RTSP_Pass); // 设置 RTSP 认证
  RTSPServer::TransportType transport = determineTransportType();
  rtpIp.fromString(RTP_ip);
  rtspServer.transport = transport;
#if INCLUDE_AUDIO
  rtspServer.sampleRate = SAMPLE_RATE; 
#endif
  rtspServer.rtspPort = rtspPort; 
  rtspServer.rtpVideoPort = rtpVideoPort; 
  rtspServer.rtpAudioPort = rtpAudioPort; 
  rtspServer.rtpSubtitlesPort = rtpSubtitlesPort;
  rtspServer.rtpIp = rtpIp; 
  rtspServer.maxRTSPClients = rtspMaxClients;
  rtspServer.rtpTTL = rtpTTL; 
    
  if (transport != RTSPServer::NONE) {
    if (rtspServer.init()) { 
      LOG_INF("RTSP server started successfully with transport%s", transportStr);
      LOG_INF("Connect to: rtsp://%s%s:%d%s", useAuth ? "<username>:<password>@" : "", netLocalIP().toString().c_str(), 
        rtspServer.rtspPort, useAuth ? " (credentials not shown for security reasons)" : "");

      // 启动 RTSP 任务，视频需要更大栈
#ifdef ISCAM
      if (rtspVideo) xTaskCreateWithCaps(sendRTSPVideo, "sendRTSPVideo", 1024 * 5, NULL, SUSTAIN_PRI, &sustainHandle[1], HEAP_MEM); 
      if (rtspAudio) xTaskCreateWithCaps(sendRTSPAudio, "sendRTSPAudio", 1024 * 5, NULL, SUSTAIN_PRI, &sustainHandle[2], HEAP_MEM);
      if (rtspSubtitles) xTaskCreateWithCaps(startRTSPSubtitles, "startRTSPSubtitles", 1024 * 1, NULL, SUSTAIN_PRI, &sustainHandle[3], HEAP_MEM);
#endif
#ifdef ISVC
      xTaskCreate(sendRTSPAudio, "sendRTSPAudio", 1024 * 5, NULL, 5, NULL);
#endif
    } else LOG_ERR("Failed to start RTSP server"); 
  } else LOG_WRN("RTSP server not started, no transport selected");
}

#endif
