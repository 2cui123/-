/*
* 将 ESP32 Cam 的 JPEG 图像捕获到 AVI 文件并存储到 SD 卡
* 存储在 SD 卡上的 AVI 文件也可选择并通过浏览器以 MJPEG 方式流式传输。
*
* s60sc 2020 - 2024
*/

#include "appGlobals.h"

void setup() {
  logSetup();
  LOG_INF("Selected board %s", CAM_BOARD);
  // 准备存储
  if (startStorage()) {
    // 加载已保存的用户配置
    if (loadConfig()) {
#ifndef AUXILIARY
      // 初始化摄像头
      if (psramFound()) {
        if (ESP.getPsramSize() > 1 * ONEMEG) prepCam();
        else snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Insufficient PSRAM for app: %s", fmtSize(ESP.getPsramSize()));
      } else snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Need PSRAM to be enabled");
#else
      LOG_INF("AUXILIARY mode without camera");
#endif
    }
  }

#ifdef DEV_ONLY
  devSetup();
#endif

  // 连接网络（按配置使用 WiFi 或以太网）
  startNetwork();
  if (startWebServer()) {
    // 启动其余服务
#ifndef AUXILIARY
    startSustainTasks(); 
#endif
#if INCLUDE_SMTP
    prepSMTP(); 
#endif
#if INCLUDE_FTP_HFS
    prepUpload();
#endif
#if INCLUDE_UART
    prepUart();
#endif
#if INCLUDE_PERIPH
    prepPeripherals();
  #if INCLUDE_MCPWM 
    prepMotors();
  #endif
#endif
#if INCLUDE_AUDIO
    prepAudio(); 
#endif
#if INCLUDE_TGRAM
    prepTelegram();
#endif
#if INCLUDE_I2C
    prepI2C();
  #if INCLUDE_TELEM
    prepTelemetry();
  #endif
#endif
#if INCLUDE_PERIPH
    startHeartbeat();
#endif
#ifndef AUXILIARY
 #if INCLUDE_RTSP
    prepRTSP();
 #endif
    if (!prepRecording()) {
      snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Insufficient memory, remove optional features");
      LOG_WRN("%s", startupFailure);
    }
#endif
    checkMemory();
  }
}

void loop() {
  // 确认 setup 未阻塞
  LOG_INF("=============== Total tasks: %u ===============\n", uxTaskGetNumberOfTasks() - 1);
  delay(1000);
  vTaskDelete(NULL); // 释放 8k RAM
}
