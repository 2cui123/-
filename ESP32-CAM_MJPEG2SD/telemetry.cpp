//
// 摄像头录制期间将遥测数据记录到存储
// 格式化为 CSV 文件以便在电子表格中展示
// 以及 SRT 文件，在媒体播放器中用作视频字幕
// 传感器数据来自用户提供的库和代码
// 需在编辑配置网页的外设按钮下勾选「Use telemetry recording」
// 并已下载相应设备库
// 建议在 ESP32S3 上使用，未在 ESP32 上测试

#include "appGlobals.h"

#if INCLUDE_TELEM
#if !INCLUDE_I2C
#error "Need INCLUDE_I2C true"
#endif

// 若未定义独立 I2C 引脚，则遥测 I2C 设备
// 与摄像头共用 I2C 引脚：camera_pins.h 中的 SIOD_GPIO_NUM 与 SIOC_GPIO_NUM 共享

#define NUM_BUFF 2 // CSV、SRT
#define MAX_LINE_LEN 128 // 调整为格式化遥测行的最大长度

TaskHandle_t telemetryHandle = NULL;
bool teleUse = false;
static int teleInterval = 1;
static char* teleBuf[NUM_BUFF]; // CSV 与 SRT 遥测数据缓冲区
size_t highPoint[NUM_BUFF]; // 缓冲区索引
static bool capturing = false;
static char teleFileName[FILE_NAME_LEN];
char srtBuffer[MAX_LINE_LEN]; // 存储每条 SRT 条目，用于字幕流
char csvHeader[MAX_LINE_LEN]; // CSV 文件列标题
size_t srtBytes = 0;

/*************** 用户需根据所需传感器修改以下代码 ******************/

// BMx280 与 MPU9250 I2C 传感器示例代码
// 若使用 GY-91 板（BMP280 + MPU9250 组合），
//  则在 periphsI2C.cpp 中将 USE_BMx280 与 USE_MPU9250 均设为 true
// GY-91 建议经 VIN 接 5V（使用内部 LDO）供电，优于直接接 3V3

// 用户定义的 CSV 表头行，每种设备一行，须以逗号开头
#define BME_CSV ",Temperature (C),Humidity (%),Pressure (mb),Altitude (m)"
#define BMP_CSV ",Temperature (C),Pressure (mb),Altitude (m)"
#define MPU_CSV ",Pitch,Roll,Heading"
// 用户定义的 SRT 内容行，每种设备一行，须以 2 个空格开头
#define BME_SRT "  %0.1fC  %0.1fRH  %0.1fmb  %0.1fm"
#define BMP_SRT "  %0.1fC  %0.1fmb  %0.1fm"
#define MPU_SRT "  %0.1f  %0.1f  %0.1f"

static bool isBME = false;
static bool haveBMX = false;
static bool haveMPU = false;

static bool setupSensors() {
  // 初始化所需传感器
  bool res = false;
#if USE_BMx280  
  if (checkI2Cdevice("BMx280")) {
    bool isBME = identifyBMx();
    LOG_INF("%s available", isBME ? "BME280" : "BMP280");
    if (isBME) strncat(csvHeader, BME_CSV, MAX_LINE_LEN - strlen(csvHeader) - 1);
    else strncat(csvHeader, BMP_CSV, MAX_LINE_LEN - strlen(csvHeader) - 1);
    haveBMX = res = true;
  } else LOG_WRN("%s not available", isBME ? "BME280" : "BMP280");
#endif

#if USE_MPU9250
  const char* whichMPU = "MPU9250";
#endif
#if USE_MPU6050
  const char* whichMPU = "MPU6050";
#endif
#if (USE_MPU6050 || USE_MPU9250)
  if (checkI2Cdevice(whichMPU)) {
    LOG_INF("%s available", whichMPU);
    strncat(csvHeader, MPU_CSV, MAX_LINE_LEN - strlen(csvHeader) - 1);
    haveMPU = res = true;
  } else LOG_WRN("%s not available", whichMPU);
#endif
  return res; 
}

static void getSensorData() {
  // 获取传感器数据并格式化为 CSV 行与 SRT 条目写入缓冲区
#if USE_BMx280
  if (haveBMX) {
    float* bmxData = getBMx280();
    if (isBME) {
      highPoint[0] += sprintf(teleBuf[0] + highPoint[0], ",%0.1f,%0.1f,%0.1f,%0.1f", bmxData[0], bmxData[3], bmxData[1], bmxData[2]);
      highPoint[1] += sprintf(teleBuf[1] + highPoint[1], BME_SRT, bmxData[0], bmxData[3], bmxData[1], bmxData[2]);
    } else {
      highPoint[0] += sprintf(teleBuf[0] + highPoint[0], ",%0.1f,%0.1f,%0.1f", bmxData[0], bmxData[1], bmxData[2]);
      highPoint[1] += sprintf(teleBuf[1] + highPoint[1], BMP_SRT, bmxData[0], bmxData[1], bmxData[2]);
    }
  #if INCLUDE_MQTT
    if (mqtt_active) {
      sprintf(jsonBuff, "{\"Temp\":\"%0.1f\", \"TIME\":\"%s\"}", bmxData[0], esp_log_system_timestamp());
      mqttPublish(jsonBuff);
    }
#endif
  }
#endif

#if (USE_MPU9250 || USE_MPU6050)
  if (haveMPU) {
    float* mpuData = getMPUdata();
    highPoint[0] += sprintf(teleBuf[0] + highPoint[0], ",%0.1f,%0.1f,%0.1f", mpuData[0], mpuData[1], mpuData[2]); 
    highPoint[1] += sprintf(teleBuf[1] + highPoint[1], MPU_SRT, mpuData[0], mpuData[1], mpuData[2]);  
  }
#endif
}

/*************** 除非你清楚在做什么，否则请勿修改以下代码 ******************/

void storeSensorData(bool fromStream) {
  // 可由遥测任务或流任务调用
  if (fromStream) {
    // 由流任务调用
    if (capturing) return; // 正由遥测任务存储
    else highPoint[0] = highPoint[1] = 0;
  }
  size_t startData = highPoint[1];
  getSensorData();
  if (!srtBytes) { 
    srtBytes = min(highPoint[1] - startData, (size_t)MAX_LINE_LEN);
    memcpy(srtBuffer, teleBuf[1] + startData, srtBytes);
  }
}

static void telemetryTask(void* pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    capturing = true;
    int srtSeqNo = 1;
    uint32_t srtTime = 0;
    char timeStr[10];
    uint32_t sampleInterval = 1000 * (teleInterval < 1 ? 1 : teleInterval);
    // 打开存储文件
    if (STORAGE.exists(TELETEMP)) STORAGE.remove(TELETEMP);
    if (STORAGE.exists(SRTTEMP)) STORAGE.remove(SRTTEMP);
    File teleFile = STORAGE.open(TELETEMP, FILE_WRITE);
    File srtFile = STORAGE.open(SRTTEMP, FILE_WRITE);
    // 将 CSV 表头行写入缓冲区
    highPoint[0] = sprintf(teleBuf[0], "Time%s\n", csvHeader); 
    highPoint[1] = 0;
    
    // 摄像头录制期间循环
    while (capturing) {
      uint32_t startTime = millis();
      // 写入本条字幕的头部
      formatElapsedTime(timeStr, srtTime, true);
      highPoint[1] += sprintf(teleBuf[1] + highPoint[1], "%d\n%s,000 --> ", srtSeqNo++, timeStr);
      srtTime += sampleInterval;
      formatElapsedTime(timeStr, srtTime, true);
      highPoint[1] += sprintf(teleBuf[1] + highPoint[1], "%s,000\n", timeStr);
      // 写入 CSV 行与 SRT 条目的当前时间
      time_t currEpoch = getEpoch();
      for (int i = 0; i < NUM_BUFF; i++) highPoint[i] += strftime(teleBuf[i] + highPoint[i], 10, "%H:%M:%S", localtime(&currEpoch));
      // 从传感器获取并存储数据
      storeSensorData(false);
      // 添加换行以结束行
      highPoint[0] += sprintf(teleBuf[0] + highPoint[0], "\n"); 
      highPoint[1] += sprintf(teleBuf[1] + highPoint[1], "\n\n");
      
      // 若标记溢出缓冲区，则写入存储
      for (int i = 0; i < NUM_BUFF; i++) {
        if (highPoint[i] >= RAMSIZE) {
          highPoint[i] -= RAMSIZE;
          if (i) srtFile.write((uint8_t*)teleBuf[i], RAMSIZE);
          else teleFile.write((uint8_t*)teleBuf[i], RAMSIZE);
          // 将溢出部分推到缓冲区开头
          memcpy(teleBuf[i], teleBuf[i]+RAMSIZE, highPoint[i]);
        }
      }
      // 等待下一采集间隔
      while (millis() - sampleInterval < startTime) delay(10);
    }
    
    // 采集结束，将剩余缓冲区写入存储
    if (highPoint[0]) teleFile.write((uint8_t*)teleBuf[0], highPoint[0]);
    if (highPoint[1]) srtFile.write((uint8_t*)teleBuf[1], highPoint[1]);
    teleFile.close();
    srtFile.close();
    // 使用 AVI 文件名及相应扩展名重命名临时文件
    changeExtension(teleFileName, CSV_EXT);
    STORAGE.rename(TELETEMP, teleFileName);
    changeExtension(teleFileName, SRT_EXT);
    STORAGE.rename(SRTTEMP, teleFileName);
    LOG_INF("Saved %d entries in telemetry files", srtSeqNo);
  }
}

void prepTelemetry() {
  // 由应用初始化调用
  if (teleUse) {
    teleInterval = srtInterval;
    for (int i=0; i < NUM_BUFF; i++) teleBuf[i] = psramFound() ? (char*)ps_malloc(RAMSIZE + MAX_LINE_LEN) : (char*)malloc(RAMSIZE + MAX_LINE_LEN);
    if (setupSensors()) xTaskCreateWithCaps(&telemetryTask, "telemetryTask", TELEM_STACK_SIZE, NULL, TELEM_PRI, &telemetryHandle, HEAP_MEM);
    else teleUse = false;
    LOG_INF("Telemetry recording %s available", teleUse ? "is" : "NOT");
    debugMemory("prepTelemetry");
  }
}

bool startTelemetry() {
  // 摄像头开始录制时调用
  bool res = true;
  if (teleUse && telemetryHandle != NULL) xTaskNotifyGive(telemetryHandle); // 唤醒任务
  else res = false;
  return res;
}

void stopTelemetry(const char* fileName) {
  // 摄像头停止录制时调用
  if (teleUse) strcpy(teleFileName, fileName); 
  capturing = false; // 停止任务
}

#endif
