//
// 处理麦克风输入，以及通过功放输出的扬声器。
// 麦克风输入和功放输出各使用 ESP32 或 ESP32S3 中
// 一个独立的 I2S 外设。
// 支持 I2S 和 PDM 麦克风。
// 支持 I2S 功放。
//
// 若同时使用 I2S 麦克风和 I2S 功放，以下引脚应设为相同的共享值：
// - micSckPin = mampBckIo（位时钟引脚共享）
// - micSWsPin = mampSwsIo（字选引脚共享）
// PDM 麦克风必须使用与 I2S 功放不同的引脚
//
// 可使用 PC 或手机上的浏览器麦克风：
// - 对于变声（VoiceChanger）应用，使用浏览器麦克风而非本地麦克风
//   - 选择操作前需按下 PC 麦克风按钮
// - 对于 MJPEG2SD 应用，音频直通到扬声器，与本地麦克风无关
//   - 需在 配置 / 外设 中启用功放和引脚，网页上才会显示「启动麦克风」按钮
//   - 仅在需要说话时才应激活浏览器麦克风

#include "appGlobals.h"

#if INCLUDE_AUDIO 

#include <ESP_I2S.h>
I2SClass I2Spdm;
I2SClass I2Sstd;

// 在 ESP32 上，与摄像头配合时仅 I2S1 可用
i2s_port_t MIC_CHAN = I2S_NUM_1;
i2s_port_t AMP_CHAN = I2S_NUM_0;

static bool micUse = false; // ESP 麦克风可用
bool micRem = false; // 使用浏览器麦克风（取决于应用）
static bool ampUse = false; // ESP 功放/扬声器是否可用
bool spkrRem = false; // 使用浏览器扬声器
bool volatile stopAudio = false;
static bool micRecording = false;

// I2S 设备
bool I2Smic; // true 为 I2S，false 为 PDM
// I2S 串行时钟和 I2S 位时钟可共用同一引脚
// I2S 字选和 I2S 左右时钟可共用同一引脚

// I2S 外接麦克风引脚
// INMP441 I2S 麦克风引脚，L/R 接 GND 为左声道
// MP34DT01 PDM 麦克风引脚，SEL 接 GND 为左声道
int micSckPin = -1; // I2S 串行时钟
int micSWsPin = -1; // I2S 字选, PDM 时钟
int micSdPin = -1;  // I2S 串行数据, PDM 数据

// I2S 功放引脚
// MAX98357A 
// SD 保持单声道（悬空）
// 增益：100k 到 GND 可用，不要直接接 GND。悬空为 9 dB 
int mampBckIo = -1; // I2S 位时钟或串行时钟
int mampSwsIo = -1;  // I2S 左右时钟或字选
int mampSdIo = -1;   // I2S 数据输入

int ampTimeout = 1000; // 无输出时放弃功放写入的超时（毫秒）
uint32_t SAMPLE_RATE = 16000;  // 音频采样率（Hz）
int micGain = 0;  // 麦克风增益，0 为关闭 
int8_t ampVol = 0; // 功放音量系数，0 为关闭

TaskHandle_t audioHandle = NULL;

static int totalSamples = 0;
static const uint8_t sampleWidth = sizeof(int16_t);
const size_t sampleBytes = DMA_BUFF_LEN * sampleWidth;
int16_t* sampleBuffer = NULL;
static uint8_t* wsBuffer = NULL;
static size_t wsBufferLen = 0;
uint8_t* audioBuffer = NULL; // 麦克风输入流式传输到 NVR 或 RTSP
size_t audioBytes = 0; 

static const char* micLabels[2] = {"PDM", "I2S"};

#ifdef CONFIG_IDF_TARGET_ESP32S3
#define psramMax (ONEMEG * 6)
#else
#define psramMax (ONEMEG * 2)
#endif
#ifdef ISCAM
bool AudActive = false; // 是否显示音频功能
static File wavFile;
#endif
#ifdef ISVC
uint8_t* recAudioBuffer = NULL;
size_t recAudioBytes = 0; 
#endif
static uint8_t wavHeader[WAV_HDR_LEN] = { // WAV 文件头模板
  0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6D, 0x74, 0x20,
  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x11, 0x2B, 0x00, 0x00, 0x11, 0x2B, 0x00, 0x00,
  0x02, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00,
};

void applyVolume() {
  // 确定所需音量设置
  int8_t adjVol = ampVol * 2; // 使用网页设置
#ifdef ISVC
  adjVol = checkPotVol(adjVol);  // 若可用则使用电位器设置
#endif
  if (adjVol) {
    // 增大或减小音量，6 为基准（如电位器/网页滑块中点）
    adjVol = adjVol > 5 ? adjVol - 5 : adjVol - 7; 
    // 对采样应用音量控制
    for (int i = 0; i < DMA_BUFF_LEN; i++) {   
      // 应用音量控制 
      sampleBuffer[i] = adjVol < 0 ? sampleBuffer[i] / abs(adjVol) : constrain((int32_t)sampleBuffer[i] * adjVol, SHRT_MIN, SHRT_MAX);
    }
  } // 否则关闭音量
}

static bool setupMic() {
  bool res;
  if (micSckPin < 0 && I2Smic) {
    LOG_WRN("Switching to PDM mic setup as I2S SCK pin not defined");
    I2Smic = false;
    updateConfigVect("mtype", "0");
  }
  if (I2Smic) {
    // I2S 麦克风和 I2S 功放可共用同一 I2S 通道
    I2Sstd.setPins(micSckPin, micSWsPin, mampSdIo, micSdPin, -1); // 位时钟/串行时钟, 字选/左右时钟, 串行数据输出, 串行数据输入, 主时钟
    res = I2Sstd.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
  } else {
    // PDM 麦克风需要与 I2S 独立的通道
    I2Spdm.setPinsPdmRx(micSWsPin, micSdPin);
    res = I2Spdm.begin(I2S_MODE_PDM_RX, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
  }
  return res;
}

static bool setupAmp() {
  bool res = true;
  if (!micUse || !I2Smic) {
    // 若 setupMic() 尚未启动
    I2Sstd.setPins(mampBckIo, mampSwsIo, mampSdIo, -1, -1); // 位时钟/串行时钟, 字选/左右时钟, 串行数据输出, 串行数据输入, 主时钟
    res = I2Sstd.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
  } // 已由 setupMic() 启动
  return res;
}

void closeI2S() {
  I2Sstd.end();
  I2Spdm.end();
}

static void applyMicGain(size_t bytesRead) {
  // 按所需系数调整 ESP 麦克风增益
  uint8_t gainFactor = pow(2, micGain - MIC_GAIN_CENTER);
  for (int i = 0; i < bytesRead / sampleWidth; i++) {
    sampleBuffer[i] = constrain(sampleBuffer[i] * gainFactor, SHRT_MIN, SHRT_MAX);
  }
}

static size_t espMicInput() {
  // 读取 ESP 麦克风
  size_t bytesRead = 0;
  if (micUse) {
    bytesRead = I2Smic ? I2Sstd.readBytes((char*)sampleBuffer, sampleBytes) : I2Spdm.readBytes((char*)sampleBuffer, sampleBytes);
    applyMicGain(bytesRead);
  }
  return bytesRead;
}

size_t updateWavHeader() {
  // 更新 WAV 文件头
  uint32_t dataBytes = totalSamples * sampleWidth;
  uint32_t wavFileSize = dataBytes ? dataBytes + WAV_HDR_LEN - 8 : 0; // 不含数据块头的 WAV 文件大小
  memcpy(wavHeader+4, &wavFileSize, 4);
  memcpy(wavHeader+24, &SAMPLE_RATE, 4); // 采样率
  uint32_t byteRate = SAMPLE_RATE * sampleWidth; // 字节率（采样率 * 声道数 * 位深度/8）
  memcpy(wavHeader+28, &byteRate, 4); 
  memcpy(wavHeader+WAV_HDR_LEN-4, &dataBytes, 4); // WAV 数据大小
  memcpy(audioBuffer, wavHeader, WAV_HDR_LEN);
  return dataBytes;
}

/*********************************************************************/

#ifdef ISVC

#if !INCLUDE_RTSP
bool rtspAudio = false;
#endif

static size_t micInput() {
  // 从浏览器麦克风或 ESP 麦克风获取输入
  size_t bytesRead = (micRem) ? wsBufferLen : espMicInput();
  if (bytesRead && micRem) {
    // 双缓冲浏览器麦克风输入
    memcpy(sampleBuffer, wsBuffer, bytesRead);
    wsBufferLen = 0;
    applyMicGain(bytesRead);
  } else if (micRem) delay(20);
  return bytesRead;
}

void browserMicInput(uint8_t* wsMsg, size_t wsMsgLen) {
  // 通过 WebSocket 接收浏览器麦克风输入
  if (micRem && !wsBufferLen) {
    // 将浏览器麦克风输入复制到 sampleBuffer 供功放使用
    wsBufferLen = wsMsgLen;
    memcpy(wsBuffer, wsMsg, wsMsgLen);
  }
}

static void ampOutput(size_t bytesRead = sampleBytes) {
  // 输出到功放，应用所需滤波和音量
  applyFilters();
  if (spkrRem) wsAsyncSendBinary((uint8_t*)sampleBuffer, bytesRead); // 浏览器扬声器
  else if (ampUse) I2Sstd.write((uint8_t*)sampleBuffer, bytesRead); // ESP 功放扬声器
  if (!audioBytes) {
    // 填充音频缓冲区以发送到 RTSP
    memcpy(audioBuffer, sampleBuffer, bytesRead);
    audioBytes = bytesRead;
  }
  displayAudioLed(sampleBuffer[0]);
}

static void passThru() {
  // 将麦克风缓冲区直接播放到功放
  size_t bytesRead = micInput();
  if (bytesRead) ampOutput(bytesRead);
}

static void makeRecording() {
  if (psramFound()) {
    LOG_INF("Recording ...");
    recAudioBytes = WAV_HDR_LEN; // 为 WAV 文件头预留空间
    wsBufferLen = 0;
    while (recAudioBytes < psramMax) {
      size_t bytesRead = micInput();
      if (bytesRead) {
        memcpy(recAudioBuffer + recAudioBytes, sampleBuffer, bytesRead);
        recAudioBytes += bytesRead;
      }
      if (stopAudio) break;
    } // PSRAM 已满
    if (!stopAudio) wsJsonSend("stopRec", "1");
    totalSamples = (recAudioBytes  - WAV_HDR_LEN) / sampleWidth;
    LOG_INF("%s recording of %d samples", stopAudio ? "Stopped" : "Finished",  totalSamples);  
    stopAudio = true;
  } else LOG_WRN("PSRAM needed to record and play");
}

static void playRecording() {
  if (psramFound()) {
    LOG_INF("Playing %d samples, initial volume: %d", totalSamples, ampVol); 
    for (int i = WAV_HDR_LEN; i < totalSamples * sampleWidth; i += sampleBytes) { 
      memcpy(sampleBuffer, recAudioBuffer+i, sampleBytes);
      ampOutput();
      if (stopAudio) break;
    }
    if (!stopAudio) wsJsonSend("stopPlay", "1");
    LOG_INF("%s playing of %d samples", stopAudio ? "Stopped" : "Finished", totalSamples);
    stopAudio = true;
  } else LOG_WRN("PSRAM needed to record and play");
}

static void VCactions() {
  // 执行用户请求的操作
  stopAudio = false;
  closeI2S();
  prepAudio();
  setupFilters();

  // 音频动作枚举定义于 appGlobals.h
  switch (THIS_ACTION) {
    case RECORD_ACTION:
      if (micRem) wsAsyncSendText("#M1");
      if (micUse || micRem) makeRecording();
    break;
    case PLAY_ACTION:
      // 持续播放直到停止
      if (ampUse || spkrRem || rtspAudio) playRecording(); // 播放上次录音
    break;
    case PASS_ACTION:
      if (ampUse || spkrRem || rtspAudio) {
        if (micRem) wsAsyncSendText("#M1");
        LOG_INF("Passthru started");
        wsBufferLen = 0;
        while (!stopAudio) passThru();
        LOG_INF("Passthru stopped"); 
      }
    break;
    default: 
    break;
  }
  displayAudioLed(0);
  xSemaphoreGive(audioSemaphore);
}

#endif

/*****************************************************************/

#ifdef ISCAM

void browserMicInput(uint8_t* wsMsg, size_t wsMsgLen) {
  // 通过 WebSocket 接收浏览器麦克风输入，发送到 ESP 功放
  if (micRem && !wsBufferLen) {
    wsBufferLen = wsMsgLen;
    memcpy(wsBuffer, wsMsg, wsMsgLen);
    int8_t adjVol = ampVol * 2; // 使用网页设置
    if (adjVol) {
      // 增大或减小音量，6 为基准（如网页滑块中点）
      adjVol = adjVol > 5 ? adjVol - 5 : adjVol - 7; 
      // 对采样应用音量控制
      int16_t* wsPtr = (int16_t*) wsBuffer;
      for (int i = 0; i < wsBufferLen / sizeof(int16_t); i++) {   
        // 应用音量控制 
        wsPtr[i] = adjVol < 0 ? wsPtr[i] / abs(adjVol) : constrain((int32_t)wsPtr[i] * adjVol, SHRT_MIN, SHRT_MAX);
      }
    }
    I2Sstd.write(wsBuffer, wsBufferLen);
    wsBufferLen = 0;
  }
}    

void startAudioRecord() {
  // 由 mjpeg2sd.cpp 中的 openAvi() 调用
  // 开始音频录制并将录制的音频以 WAV 文件写入 SD 卡
  // 在 FTP 上传或浏览器下载时合并到 AVI 文件的 PCM 通道
  // 以便媒体播放器读取
  if (micUse && micGain) {
      wavFile = STORAGE.open(WAVTEMP, FILE_WRITE);
      wavFile.write(wavHeader, WAV_HDR_LEN); 
      micRecording = true;
      totalSamples = 0;
  } else {
    micRecording = false;
    LOG_WRN("No ESP mic defined or mic is off");
  }
}

void finishAudioRecord(bool isValid) {
  // 由 mjpeg2sd.cpp 中的 closeAvi() 调用
  if (micRecording) {
    // 完成录制，若有效则保存
    micRecording = false; 
    if (isValid) {
      size_t dataBytes = updateWavHeader();
      wavFile.seek(0, SeekSet); // 文件开头
      wavFile.write(wavHeader, WAV_HDR_LEN); // 覆盖默认文件头
      wavFile.close();  
      LOG_INF("Captured %d audio samples with gain factor %i", totalSamples, micGain - MIC_GAIN_CENTER);
      LOG_INF("Saved %s to SD for %s", fmtSize(dataBytes + WAV_HDR_LEN), WAVTEMP);
    }
  }
}

static void camActions() {
  // 将 ESP 麦克风输入应用到所需输出
  while (true) {
    size_t bytesRead = 0;
    if (micRecording || !audioBytes || spkrRem) bytesRead = espMicInput(); // 加载 sampleBuffer
    if (bytesRead) {
      if (micRecording) {
        // 将麦克风输入录制到 SD
        wavFile.write((uint8_t*)sampleBuffer, bytesRead);
        totalSamples += bytesRead / sampleWidth; 
      }
      if (!audioBytes) {
        // 填充 audioBuffer 以发送到 NVR
        memcpy(audioBuffer, sampleBuffer, bytesRead);
        audioBytes = bytesRead;
      }
      // 对讲：ESP 麦克风到浏览器扬声器
      if (spkrRem) wsAsyncSendBinary((uint8_t*)sampleBuffer, bytesRead);
    } else delay(20);
  }
}

#endif

/************************************************************************/

void setI2Schan(int whichChan) {
  // 设置麦克风的 I2S 端口，功放使用相反端口
  if (whichChan) {
    MIC_CHAN = I2S_NUM_1;
    AMP_CHAN = I2S_NUM_0;
  } else {
    MIC_CHAN = I2S_NUM_0;
    AMP_CHAN = I2S_NUM_1;
  }
}

static void predefPins() {
char audPin[3];
#if defined(I2S_SD)
  sprintf(audPin, "%d", I2S_SD);
  updateStatus("micSdPin", audPin);
  sprintf(audPin, "%d", I2S_WS);
  updateStatus("micSWsPin", audPin);
  sprintf(audPin, "%d", I2S_SCK);
  updateStatus("micSckPin", audPin);
#endif
#if defined(I2S_BCLK)
  sprintf(audPin, "%d", I2S_BCLK);
  updateStatus("mampBckIo", audPin);
  sprintf(audPin, "%d", I2S_LRCLK);
  updateStatus("mampSwsIo", audPin);
  sprintf(audPin, "%d", I2S_DIN);
  updateStatus("mampSdIo", audPin);
#endif

  I2Smic = micSckPin == -1 ? false : true;

#ifdef CONFIG_IDF_TARGET_ESP32S3
  MIC_CHAN = I2S_NUM_0;
#endif
}

static void audioTask(void* parameter) {
  // 循环处理各项音频需求
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#ifdef ISCAM
    camActions(); // 持续运行
#endif
#ifdef ISVC
    VCactions(); // 运行一次
#endif
  }
  vTaskDelete(NULL);
}

void prepAudio() {
  // 变声应用使用音频任务处理所有活动
  // 摄像应用使用音频任务处理麦克风，对讲任务处理功放
#ifdef ISCAM
  predefPins();
#endif
  if (MIC_CHAN == I2S_NUM_1 && !I2Smic) LOG_WRN("Only I2S devices supported on I2S_NUM_1");
  else {
    if (micSdPin <= 0) LOG_WRN("Microphone pins not defined");
    else {
      micUse = setupMic(); 
      if (micUse) LOG_INF("Sound capture is available using %s mic on I2S%i with gain %d", micLabels[I2Smic], MIC_CHAN, micGain);
      else LOG_WRN("Unable to start ESP mic");
    }
    if (mampSdIo <= 0) LOG_WRN("Amplifier pins not defined");
    else {
      ampUse = setupAmp();
      if (ampUse) LOG_INF("Speaker output is available using I2S amp on I2S%i with vol %d", AMP_CHAN, ampVol);
      else LOG_WRN("Unable to start ESP amp");
    }
  }

  if (sampleBuffer == NULL) sampleBuffer = (int16_t*)malloc(sampleBytes);
  if (wsBuffer == NULL) wsBuffer = (uint8_t*)malloc(MAX_PAYLOAD_LEN);
  if (audioBuffer == NULL && psramFound()) audioBuffer = (uint8_t*)ps_malloc(sampleBytes);
#ifdef ISVC
  if (recAudioBuffer == NULL && psramFound()) recAudioBuffer = (uint8_t*)ps_malloc(psramMax + (sizeof(int16_t) * DMA_BUFF_LEN));
  // 无 ESP 麦克风或功放时变声应用仍可使用音频任务
  if (!micUse && !ampUse) LOG_WRN("Only browser mic and speaker can be used");
#endif
#ifdef ISCAM
  wsBufferLen = 0;
  // 仅 ESP 麦克风需要音频任务
  if (!micUse) return;
#endif
  if (audioHandle == NULL) xTaskCreateWithCaps(audioTask, "audioTask", AUDIO_STACK_SIZE, NULL, AUDIO_PRI, &audioHandle, HEAP_MEM);
#ifdef ISCAM
  xTaskNotifyGive(audioHandle);
#endif
  debugMemory("prepAudio");
}

#endif
