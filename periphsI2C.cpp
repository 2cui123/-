
// I2C 驱动与设备
//
// OLED SSD1306 显示屏 128*64
// PCF8591 ADC
// BM*280 温度、气压、海拔（BMP280），BME280 另含湿度
// MPU6050 六轴加速度计与陀螺仪
// MPU9250 九轴加速度计、陀螺仪与磁力计
// DS3231 RTC
// LCD 1602 显示屏 2*16 
//
// 要启用某设备，请在 appGlobals.h 中设置相应的 USE_* 定义
//
// s60sc 2023, 2024, 2025
// 包含 rjsachse 的贡献

#include "appGlobals.h"

#if INCLUDE_I2C

#define SENSOR_TIMEOUT 100 // 等待传感器响应的最长时间（毫秒）

#include <Wire.h>

// 在 initializeI2C() 调用中定义 I2C 总线使用的引脚
// 若引脚未针对板型正确配置，会出现异常结果
int I2Csda = -1;
int I2Cscl = -1;
static byte I2CDATA[10]; // 存储 I2C 接收或待发送的数据
static int I2Cdevices = -1;
static bool isShared = false;
  
// I2C 设备名称，按地址索引
static bool deviceStatus[128] = {false}; // 设备是否存在
static const char* clientName[128] = {
  "", "", "", "", "", "", "", "", "", "", "", "", "AK8963", "", "", "",
  "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "PY260",
  "", "", "", "", "", "", "", "LCD1602", "", "", "", "", "", "", "", "",
  "OV2640", "", "", "", "", "", "", "", "", "", "", "", "OV5640/SSD1306", "SSD1306", "", "",
  "", "", "", "", "", "", "", "", "PCF8591", "", "", "", "", "", "", "",
  "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
  "OV2640", "OV2640", "", "", "", "", "", "", "MPUxx50/DS3231", "MPUxx50", "", "", "", "", "", "",
  "", "", "", "", "", "", "BMx280", "BMx280", "OV5640", "OV5640", "", "", "", "", "", ""};

static bool prepI2Cdevices();
static void startPollTask();

/********************* 通用 I2C 工具 ***********************/

static bool sendTransmission(int clientAddr, bool scanning) {
  // 向 I2C 设备发送请求并判断结果的通用函数
  byte result = Wire.endTransmission(true);
    /*1: 数据过长，无法放入发送缓冲区
      2: 发送地址时收到 NACK
      3: 发送数据时收到 NACK
      4: 其他错误，例如已关闭
      5: I2C 总线忙
      8: 未知 pcf8591 状态 */
      
  if (!scanning && result > 0) LOG_WRN("Client %s at 0x%02X with connection error: %d", clientName[clientAddr], clientAddr, result);
  return (result == 0) ? true : false;
}

static void scanI2C() {
  // 查找所有活跃的 I2C 设备信息
  LOG_INF("I2C device scanning");
  for (byte address = 0; address < 127; address++) {
    Wire.beginTransmission(address);
    // 仅当该客户端设备应存在时才报告错误
    if (sendTransmission(address, true)) {
      LOG_INF("I2C device %s present at address: 0x%02X", clientName[address], address);
      I2Cdevices++;
      deviceStatus[address] = true;
    }
  }
  LOG_INF("I2C devices found: %d", I2Cdevices);
}

static bool getI2Cdata (uint8_t clientAddr, uint8_t controlByte, uint8_t numBytes) {
  // 向 I2C 客户端发送命令并接收响应
  // clientAddr 为 I2C 地址
  // controlByte 为控制指令
  // numBytes 为请求的字节数
  Wire.beginTransmission(clientAddr); // 选择要使用的客户端
  Wire.write(controlByte); // 发送设备命令
  if (sendTransmission(clientAddr, false)) {
    // 获取所需字节数
    Wire.requestFrom (clientAddr, numBytes);
    for (int i=0; i<numBytes; i++) I2CDATA[i] = Wire.read();
    return sendTransmission(clientAddr, false);
  } 
  return false; 
}

static bool sendI2Cdata(int clientAddr, uint8_t controlByte, uint8_t numBytes) {
  // 向 I2C 设备发送数据
  // clientAddr 为 I2C 地址
  // controlByte 为控制指令
  // numBytes 为发送的字节数
  Wire.beginTransmission(clientAddr);
  if (controlByte) Wire.write(controlByte);
  for (int i=numBytes-1; i>=0; i--) Wire.write(I2CDATA[i]);
  return sendTransmission(clientAddr, false);
}

bool shareI2C(int sdaShare, int sclShare) {
  // 若总线需共享则应用给定引脚
  /* 需要 arduino-esp32 core v3.3.0 或更高版本 */
  if (I2Csda < 0) { 
    // I2C 总线与另一外设（如摄像头）共享
    I2Csda = sdaShare;
    I2Cscl = sclShare;
    isShared = true;
    Wire.begin(I2Csda, I2Cscl); // 作为主设备加入 I2C 总线
    LOG_INF("I2C bus shared with camera");
  }
  return isShared;
}

bool prepI2C() {
  // 若未共享则启动 I2C 端口，然后初始化 I2C 外设
  if (I2Csda == I2Cscl) {
    LOG_ALT("I2C pins not defined: %d", I2Csda);
    return false;
  } 
  // 启动 I2C
  if (!isShared) {
    Wire.begin(I2Csda, I2Cscl); // 作为主设备加入 I2C 总线
    //Wire.setClock(400000); // 默认 100kHz，最高 400kHz
  }
  LOG_INF("%sI2C initialised at %dkHz using pins SDA: %d, SCL: %d", isShared ? "Shared " : "", Wire.getClock() / 1000, I2Csda, I2Cscl);

  I2Cdevices = 0;
  scanI2C();
  return prepI2Cdevices();
}

/***************************************** OLED 显示屏 *************************************/

#define SSD1306_BIaddr 0x3d // 板载 OLED
#define SSD1306_Extaddr 0x3c // 外接 OLED（也是 OV5640 的地址）
#if USE_SSD1306
#include "SSD1306Wire.h" // https://github.com/ThingPulse/esp8266-oled-ssd1306
SSD1306Wire oledBI(SSD1306_BIaddr);
SSD1306Wire oledExt(SSD1306_Extaddr);
SSD1306Wire* thisOled;

static bool oledOK = false;
bool flipOled = false; // 为 true 表示 OLED 引脚位于显示屏上方

// OLED SSD1306 显示屏 128*64
void oledLine(const char* msg, int hpos, int vpos, int msgwidth, int fontsize) { 
  // 在 OLED SSD1306 上显示文本
  // 为避免闪烁，仅周期性调用
  // 参数：消息字符串、水平像素起点、垂直像素起点、清除宽度、字体类型
  // 清除原行
  if (oledOK) {
    thisOled->setTextAlignment(TEXT_ALIGN_LEFT);
    thisOled->setColor(BLACK);
    thisOled->fillRect(hpos, vpos, msgwidth, fontsize*5/4); // 为字体下延留出空间
    // 显示给定文本，字体大小 10、16、24，起点为水平像素 hpos、垂直像素 vpos
    thisOled->setFont(ArialMT_Plain_10);
    if (fontsize == 16) thisOled->setFont(ArialMT_Plain_16);
    if (fontsize == 24) thisOled->setFont(ArialMT_Plain_24);
    thisOled->setColor(WHITE);
    thisOled->drawString(hpos, vpos, msg);
  }
}

static void tellTale() { 
  static bool ledState = false;
  ledState = !ledState;
  static const char* tellTaleStr[] = {"*", ""}; // 表示 OLED（及 I2C）正在运行
  oledLine(tellTaleStr[ledState],124,60,4,10); 
}

void oledDisplay() {
  if (oledOK) {
    tellTale();   // OLED 运行指示
    thisOled->display();
  }
}

static bool setupOled() {
  if (!oledOK) {
    oledOK = true;
    if (deviceStatus[SSD1306_BIaddr]) thisOled = &oledBI;
    else if (deviceStatus[SSD1306_Extaddr]) thisOled = &oledExt;
    else oledOK = false;
    if (oledOK) {
      thisOled->end();
      if (thisOled->init()) { if (flipOled) thisOled->flipScreenVertically(); }
      else oledOK = false;
    }
    if (!oledOK) LOG_WRN("SSD1306 oled not available");
  }
  return oledOK;
}

void finalMsg(const char* finalTxt) {
  if (oledOK) {
    // 在 ESP32 进入睡眠前于持久 OLED 屏上显示消息
    thisOled->resetDisplay();
    oledLine(finalTxt,0,0,128,16);
    thisOled->display();
    delay(2000); // 保持显示
  }
}
#endif

/*********************** PCF8591 ************************/

#define PCF8591addr 0x48 // PCF8591 ADC

byte* getPCF8591() { // 模拟通道
/*   
   YL-40 模块
   使用自动递增控制指令，返回 4 个 ADC 通道的 8 位数值
   PC8591 命令：
   位 0-1：通道 0 (00) -> 3 (11)
   位 3：自动递增
   位 4-5：输入编程，独立输入 (00) 等
   位 6：模拟输出使能
  */
  static byte PCF8591[4] = {0}; 
  if (deviceStatus[PCF8591addr]) {
    if (getI2Cdata(PCF8591addr, 0x44, 5)) {
      // 需读 5 字节，忽略第一字节（为上一通道 0 的数据）
      // 顺序为高到低：通道 3 2 1 0
      for (int i = 0; i < 4; i++) PCF8591[i] = smoothAnalog(I2CDATA[i + 1]);
    } 
  } else LOG_WRN("PCF8591 ADC not available");
  return PCF8591;
}

/******************************* BMP280 / BME280 ******************************/

#define BMx280_Def 0x76 // BMX280 默认地址
#define BMx280_Alt 0x77 // BMX280 备用地址

#if USE_BMx280
#define STD_PRESSURE 1013.25 // 海平面参考气压（mB/hPa）
#define DEGREE_SYMBOL "\xC2\xB0"

#include <BMx280I2C.h> // https://github.com/christandlg/BMx280MI
BMx280I2C bmxDef(BMx280_Def); 
BMx280I2C bmxAlt(BMx280_Alt);
BMx280I2C* thisBmx;

static bool BMx280ok = false;
static bool isBME = false;

static bool setupBMx() {
  // 若可用则初始化 BMx280
  if (!BMx280ok) {
    BMx280ok = true;
    if (deviceStatus[BMx280_Def]) thisBmx = &bmxDef;
    else if (deviceStatus[BMx280_Alt]) thisBmx = &bmxAlt;
    else BMx280ok = false;
    if (BMx280ok) {
      BMx280ok = thisBmx->begin();
      if (BMx280ok) {
        isBME = thisBmx->isBME280();
        thisBmx->resetToDefaults();
        thisBmx->writeOversamplingPressure(BMx280MI::OSRS_P_x16);
        thisBmx->writeOversamplingTemperature(BMx280MI::OSRS_T_x16);
        if (isBME) thisBmx->writeOversamplingHumidity(BMx280MI::OSRS_H_x16);
        thisBmx->measure();
      }
    }
    if (!BMx280ok) LOG_WRN("BMx280 not available");
  } 
  return BMx280ok;
}

float* getBMx280() { 
  // 获取并返回气压、温度、海拔、湿度
  static float BMx280[4] = {0};
  if (BMx280ok) {
    thisBmx->measure();
    uint32_t bmxWait = millis();
    while(!thisBmx->hasValue() && millis() - bmxWait < SENSOR_TIMEOUT) delay(10);
    if (thisBmx->hasValue()) {
      // PSI = 帕斯卡 * 0.000145
      // 环境温度（但受芯片发热影响）
      BMx280[0] = thisBmx->getTemperature(); // 摄氏度
      BMx280[1] = thisBmx->getPressure() * 0.01;  // 帕斯卡转 mB/hPa
      BMx280[2] = 44330.0 * (1.0 - pow(BMx280[1] / STD_PRESSURE, 1.0 / 5.255)); // 海拔（米）
      if (isBME) BMx280[3] = thisBmx->getHumidity(); // 相对湿度（%）
    }
  }
  return BMx280;
}

bool identifyBMx() {
  return isBME;
}
#endif

/**************************** MPU6050 / MPU9250 ******************************/
static float mpuData[4] = {0};

#define MPUxx50_HIGH 0x69 // AD0 拉高时 MPU6050 / MPU9250 的 I2C 地址
#define MPUxx50_LOW 0x68  // AD0 接地时 MPU6050 / MPU9250 的 I2C 地址

#if USE_MPU6050
// MPU6050 定义 - 不含陀螺仪
#define SENS_2G (32768.0/2.0) // 2G 灵敏度读数除数
#define ACCEL_BYTES 6 // 每轴 2 字节
#define CONFIG 0x1A
#define ACCEL_CONFIG 0x1C
#define ACCEL_XOUT_H 0x3B
#define PWR_MGMT_1 0x6B

static uint8_t MPU6050addr;
static bool MPU6050ok = false;

bool sleepMPU6050(bool doSleep) {
  // 关闭或唤醒 MPU6050
  I2CDATA[0] = doSleep ? 0x40 : 0x01;
  // PWR_MGMT_1 寄存器设为睡眠
  return sendI2Cdata(MPU6050addr, PWR_MGMT_1, 1);
}

static bool setupMPU6050() {
  if (!MPU6050ok) {
    MPU6050ok = true;
    if (deviceStatus[MPUxx50_HIGH]) MPU6050addr = MPUxx50_HIGH;
    else if (deviceStatus[MPUxx50_LOW]) MPU6050addr = MPUxx50_LOW;
    else MPU6050ok = false;
    if (MPU6050ok) {
      // 设置满量程
      I2CDATA[0] = 0x00; 
      MPU6050ok = sendI2Cdata(MPU6050addr, CONFIG, 1);
      // 唤醒传感器
      if (MPU6050ok) sleepMPU6050(false);
    } 
    if (!MPU6050ok) LOG_WRN("MPU6050 6 axis not available");
  }
  return MPU6050ok;
}

static void getMPU6050() {
  // 从 MPU6050 获取数据并以数组返回
  if (MPU6050ok) {
    if (getI2Cdata(MPU6050addr, ACCEL_XOUT_H, ACCEL_BYTES+2)) { 
      // 读取三轴加速度计与温度
      int16_t raw[4]; // X、Y、Z、温度
      float axes[4];
      for (int i=0; i<4; i++) raw[i] = I2CDATA[i*2] << 8 | I2CDATA[(i*2)+1]; 
      // 各轴 G 力值，静止竖直向下约为 1.0
      for (int i=0; i<3; i++) axes[i] = (float)raw[i] / SENS_2G;
      // 由三轴合成重力（不含线速度）
      float gXYZ = sqrt(pow(axes[0],2)+pow(axes[1],2)+pow(axes[2],2));
      LOG_VRB("gXYZ should be close to 1, is: %0.2f", gXYZ);
      // 俯仰角（度）- X 轴
      float ratio = axes[0] / gXYZ;
      mpuData[0] = (float)((ratio < 0.5) ? 90-fabs(asin(ratio)*RAD_TO_DEG) : fabs(acos(ratio)*RAD_TO_DEG));
      // 横滚角（度）- Y 轴
      ratio = axes[1] / gXYZ;
      mpuData[1] = (float)((ratio < 0.5) ? 90-fabs(asin(ratio)*RAD_TO_DEG) : fabs(acos(ratio)*RAD_TO_DEG));
      // 偏航角（度）- Z 轴（因重力恒定而不准确）
      ratio = axes[2] / gXYZ;
      mpuData[2] = (float)((ratio < 0.5) ? 90-fabs(asin(ratio)*RAD_TO_DEG) : fabs(acos(ratio)*RAD_TO_DEG));
      // 温度（摄氏度）
      mpuData[3] = ((float)raw[3] / 340.0) + 36.53; 
    }
  }
}
#endif

/*------------------------------------------------------------------*/

/*
GY-91 上的 MPU9250
VIN: 电源输入引脚
3V3: 3.3V 稳压输出
GND: 0V 电源
SCL: I2C 时钟
SDA: I2C 数据
SDO/SAO: MPU9250 I2C 地址选择
NCS: 不适用
CSB: BMP280 I2C 地址选择
*/
#if USE_MPU9250
#include "MPU9250.h" // https://github.com/hideakitai/MPU9250
// GY-91 上 MPU9250 的加速度轴方向：
// - X：短边（俯仰）
// - Y：长边（横滚）
// - Z：向上（相对真北的偏航）
// 注意内部 AK8963 磁力计地址为 0x0C
#define LOCAL_MAG_DECLINATION (4 + 56/60)  // 本地值见 https://www.magnetic-declination.com/

static MPU9250 mpu9250;
static uint8_t MPU9250addr;
static bool MPU9250ok = false;

static bool setupMPU9250() {
  if (!MPU9250ok) {
    MPU9250ok = true;
    if (deviceStatus[MPUxx50_HIGH]) MPU9250addr = MPUxx50_HIGH;
    else if (deviceStatus[MPUxx50_LOW]) MPU9250addr = MPUxx50_LOW;
    else MPU9250ok = false;
    if (MPU9250ok) {
      if (mpu9250.setup(MPU9250addr)) {
        mpu9250.setMagneticDeclination(LOCAL_MAG_DECLINATION);
        mpu9250.selectFilter(QuatFilterSel::MADGWICK);
        mpu9250.setFilterIterations(15);
        LOG_INF("MPU9250 calibrating, leave still");
        mpu9250.calibrateAccelGyro();
  //    LOG_INF("以 8 字形移动 MPU9250 直至完成");
  //    delay(2000);
  //    mpu9250.calibrateMag();
      } else MPU9250ok = false;
    } 
    if (!MPU9250ok) LOG_WRN("MPU9250 9 axis not available");
  }
  return MPU9250ok;
}

static void getMPU9250() {
  // 从 MPU9250 获取数据并以数组返回
  // 仅获取部分功能
  if (MPU9250ok) {
    uint32_t mpuWait = millis();
    while (!mpu9250.update() && millis() - mpuWait < SENSOR_TIMEOUT) delay(10);
    if (mpu9250.update()) {
      // 读取三轴加速度计与温度
      // 角度（度），采用飞机坐标系
      mpuData[0] = mpu9250.getPitch(); // X
      mpuData[1] = mpu9250.getRoll();  // Y
      mpuData[2] = mpu9250.getYaw();   // Z
      mpuData[3] = mpu9250.getTemperature(); // 摄氏度
    }
  }
}
#endif

float* getMPUdata() {
#if USE_MPU6050
  getMPU6050();
#endif
#if USE_MPU9250
  getMPU9250();
#endif
  return mpuData;
}

/********************************* DS3231 RTC ************************************/

#define DS3231_RTC 0x68 // 实时时钟（地址可能与 MPU6050 冲突）
#if USE_DS3231
#include "driver/rtc_io.h"
#include <RtcDS3231.h> // https://github.com/Makuna/Rtc/wiki
RtcDS3231<TwoWire> Rtc(Wire);

static bool DS3231ok = false;
static volatile bool RTCalarmFlag = false;

static void IRAM_ATTR RTCalarmISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  RTCalarmFlag = true;
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static bool setupRTC() {
  // 接线：
  // DS3231 SDA --> SDA
  // DS3231 SCL --> SCL
  // DS3231 VCC --> 3.3v 或 5v
  // DS3231 GND --> GND
  // DS3231 SQW --> 闹钟中断引脚 - 需上拉

  // 将中断引脚设为带上拉的输入模式
  static bool SQWpin = -1; // 需作为配置项
  if (!DS3231ok) {
    if (deviceStatus[DS3231_RTC]) {
      pinMode(SQWpin, INPUT_PULLUP);

      Rtc.Begin();
      RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__); // 编译时间
      if (!Rtc.IsDateTimeValid()) {
        LOG_WRN("RTC lost confidence in the DateTime");
        Rtc.SetDateTime(compiled);
      }

      if (!Rtc.GetIsRunning()) {
        LOG_WRN("RTC was not actively running, starting now");
        Rtc.SetIsRunning(true);
      }

      RtcDateTime now = Rtc.GetDateTime();
      if (now < compiled) {
        LOG_WRN("RTC is older than compile time, updating DateTime");
        Rtc.SetDateTime(compiled);
      }
      
      Rtc.Enable32kHzPin(false);
      Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeAlarmBoth); // 设为闹钟输出
      Rtc.LatchAlarmsTriggeredFlags();  // 清除旧的闹钟状态
      // 配置闹钟中断
      attachInterrupt(digitalPinToInterrupt(SQWpin), RTCalarmISR, FALLING);
      
      DS3231ok = true;
    } else DS3231ok = false;
  }
  if (!DS3231ok) LOG_WRN("DS3231 RTC not available");
  return DS3231ok;
}

int cycleRange(int currVal, int minVal, int maxVal) {
  // 在取值范围内循环
  if (currVal < minVal) return maxVal;
  if (currVal > maxVal) return minVal;
  return currVal;
}

void setRTCintervalAlarm(int alarmHour, int alarmMin) {
  // 闹钟 1 可每秒一次或在指定时间触发 - 秒级精度
  // 此处用于重复间隔（小时间隔）- 可多次设置
  // 在 30 秒处触发，避免与 setRTCrolloverAlarm() 冲突
  // 参数为相对当前时间的小时与分钟
  if (DS3231ok) {
    int nextHour = cycleRange(Rtc.GetDateTime().Hour()+alarmHour, 0, 23);
    int nextMin = cycleRange(Rtc.GetDateTime().Minute()+alarmMin, 0, 59);
    DS3231AlarmOne alarm1(0, nextHour, nextMin, 30, DS3231AlarmOneControl_HoursMinutesSecondsMatch);
    Rtc.SetAlarmOne(alarm1);
  }
}

void setRTCspecificAlarm(int alarmHour, int alarmMin) {
  // 闹钟 1 可每秒一次或在指定时间触发 - 秒级精度
  // 此处用于指定时刻（当天的小时与分钟）- 可多次设置
  // 在 30 秒处触发，避免与 setRTCrolloverAlarm() 冲突
  // 参数为当天指定的小时与分钟
  if (DS3231ok) {
    DS3231AlarmOne alarm1(0, alarmHour, alarmMin, 30, DS3231AlarmOneControl_HoursMinutesSecondsMatch);
    Rtc.SetAlarmOne(alarm1);
  }
}

void setRTCrolloverAlarm(int alarmHour, int alarmMin) {
  // 闹钟 2 可每分钟一次或在指定时间触发 - 分钟级精度
  // 此处用于每日翻转闹钟 - 仅设置一次
  if (DS3231ok) {
    DS3231AlarmTwo alarm2(0, alarmHour, alarmMin, DS3231AlarmTwoControl_HoursMinutesMatch);
    Rtc.SetAlarmTwo(alarm2);
  }
}

uint32_t getRTCtime() {
  // 获取当前 RTC 时间的 Unix 时间戳
  if (DS3231ok) {
    if (!Rtc.IsDateTimeValid()) LOG_WRN("RTC lost confidence in the DateTime!");
    return Rtc.GetDateTime().Unix32Time();
  }
  return 0;
}

int RTCalarmed() {
  // 检查 RTC 闹钟是否触发并返回闹钟编号
  int wasAlarmed = 0;
  if (DS3231ok) {
    if (RTCalarmFlag) { 
      RTCalarmFlag = false; // 复位标志
      DS3231AlarmFlag flag = Rtc.LatchAlarmsTriggeredFlags(); // 哪个闹钟触发并为下次复位
      if (flag & DS3231AlarmFlag_Alarm1) wasAlarmed = 1; 
      if (flag & DS3231AlarmFlag_Alarm2) wasAlarmed = 2;
    }
  }
  return wasAlarmed;
}

float RTCtemperature() {
  // DS3231 内部温度
  if (DS3231ok) {
    RtcTemperature temp = Rtc.GetTemperature();
    return temp.AsFloatDegC();
  }
  return 0;
}

void RTCdatetime(char* datestring, int datestringLen) {
  // 返回格式化的 RTC 日期时间字符串
  if (DS3231ok) {
    if (!Rtc.IsDateTimeValid()) LOG_WRN("RTC lost confidence in the DateTime!");
    RtcDateTime dt = Rtc.GetDateTime(); // 自 2000 年 1 月 1 日以来的秒数
    snprintf(datestring, datestringLen, "%02u/%02u/%04u %02u:%02u:%02u",
      dt.Day(), dt.Month(), dt.Year(), dt.Hour(), dt.Minute(), dt.Second());
  }
}
#endif


/**************************** LCD1602 ******************************/
// I2C LCD 显示屏：2 行 16 列
// 改编自 https://github.com/arduino-libraries/LiquidCrystal

#define LCD1602 0x27 // 16 字符 x 2 行 LCD
#if USE_LCD1602

// 命令
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_CURSORSHIFT 0x10
#define LCD_FUNCTIONSET 0x20
#define LCD_SETCGRAMADDR 0x40
#define LCD_SETDDRAMADDR 0x80

// 显示输入模式标志
#define LCD_ENTRYRIGHT 0x00
#define LCD_ENTRYLEFT 0x02
#define LCD_ENTRYSHIFTINCREMENT 0x01
#define LCD_ENTRYSHIFTDECREMENT 0x00

// 显示开/关控制标志
#define LCD_DISPLAYON 0x04
#define LCD_DISPLAYOFF 0x00
#define LCD_CURSORON 0x02
#define LCD_CURSOROFF 0x00
#define LCD_BLINKON 0x01
#define LCD_BLINKOFF 0x00

// 显示/光标移位标志
#define LCD_DISPLAYMOVE 0x08
#define LCD_CURSORMOVE 0x00
#define LCD_MOVERIGHT 0x04
#define LCD_MOVELEFT 0x00

// 功能设置标志
#define LCD_8BITMODE 0x10
#define LCD_4BITMODE 0x00
#define LCD_2LINE 0x08
#define LCD_1LINE 0x00
#define LCD_5x10DOTS 0x04
#define LCD_5x8DOTS 0x00

// 背光控制标志
#define LCD_BACKLIGHT 0x08
#define LCD_NOBACKLIGHT 0x00

#define En 0b00000100  // 使能位
#define Rw 0b00000010  // 读/写位
#define Rs 0b00000001  // 寄存器选择位

#define NUM_ROWS 2
#define NUM_COLS 16

static bool LCD1602ok = false;
static uint8_t displaycontrol;
static uint8_t displaymode;
static uint8_t backlightval;

static void lcdWrite(uint8_t data) {
  if (LCD1602ok) {
    I2CDATA[0] = data | backlightval;
    sendI2Cdata(LCD1602, 0, 1); 
  }
}

static void writeNibble(uint8_t value) {
	lcdWrite(value);
	lcdWrite(value | En);	  // En 拉高
	delayMicroseconds(1);		// 脉冲
	lcdWrite(value & ~En);	// En 拉低
	delayMicroseconds(50);	// 命令需 > 37us 稳定
}

static void lcdSend(uint8_t value, uint8_t mode = 0) {
  // 写入命令（mode = 0）或数据，各为两个 4 位值
  if (LCD1602ok) {
    writeNibble((value & 0xf0) | mode);
    writeNibble(((value << 4 ) & 0xf0) | mode); 
  }
}

void lcdBacklight(bool lightOn) {
  // 打开/关闭背光
  backlightval = (lightOn) ? LCD_BACKLIGHT : LCD_NOBACKLIGHT;
  lcdWrite(backlightval);
}

void lcdClear() {
  // 清屏并将光标归零
  lcdSend(LCD_CLEARDISPLAY);
  delayMicroseconds(2000);  
}

void lcdHome() {
  // 将光标位置归零
  lcdSend(LCD_RETURNHOME);  
  delayMicroseconds(2000); 
}

void lcdDisplay(bool setDisplay) {
  // 打开/关闭显示（不含背光）
  if (setDisplay) displaycontrol |= LCD_DISPLAYON;
  else displaycontrol &= ~LCD_DISPLAYON;
  lcdSend(LCD_DISPLAYCONTROL | displaycontrol);
}

static bool setupLCD1602() {  
  if (!LCD1602ok) {
    if (deviceStatus[LCD1602]) {
      LCD1602ok = true;
      delay(50); 
      lcdBacklight(false);
      delay(1000);
  
      // PCF8574 引脚不足，HD44780 只能使用 4 位模式，无法 8 位。
      // 使用特定序列进行设置
      writeNibble(0x03 << 4);
      delayMicroseconds(4500); // 至少等待 4.1ms
      writeNibble(0x03 << 4);
      delayMicroseconds(4500); // 至少等待 4.1ms
      writeNibble(0x03 << 4);
      delayMicroseconds(150);
      writeNibble(0x02 << 4);
    
       // 设置初始显示格式
      lcdSend(LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS);  
      
      // 打开显示并清屏
      displaycontrol = LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF; 
      lcdDisplay(true);
      lcdClear();
      
      // 设置输入模式并将光标置于左上角
      displaymode = LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT;
      lcdSend(LCD_ENTRYMODESET | displaymode); 
      lcdHome();
      lcdBacklight(true); 
    } else LCD1602ok = false;
    if (!LCD1602ok) LOG_WRN("LCD1602 display not available");
  }
  return LCD1602ok;
}

void lcdPrint(const char* str) {
  // 向 LCD 写入字符串
	for (int i=0; i<strlen(str); i++) lcdSend((uint8_t)str[i], Rs);
}

void lcdSetCursorPos(uint8_t row, uint8_t col) {
  // 设置光标行列位置
	int row_offsets[] = {0x00, 0x40, 0x14, 0x54}; 
  if (row > NUM_ROWS) row = NUM_ROWS - 1;
  if (col > NUM_COLS) col = NUM_COLS - 1;
	lcdSend(LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

void lcdLineCursor(bool showLine) {
  // 打开/关闭下划线光标
  if (showLine) displaycontrol |= LCD_CURSORON;
  else displaycontrol &= ~LCD_CURSORON;
	lcdSend(LCD_DISPLAYCONTROL | displaycontrol);
}

void lcdBlinkCursor(bool showBlink) {
  // 打开/关闭闪烁光标
  if (showBlink) displaycontrol |= LCD_BLINKON;
  else displaycontrol &= ~LCD_BLINKON;
	lcdSend(LCD_DISPLAYCONTROL | displaycontrol);
}

void lcdScrollText(bool scrollLeft) {
  // 将当前显示左移或右移一位（不环绕）
  uint8_t moveDir = (scrollLeft) ? LCD_MOVELEFT : LCD_MOVERIGHT;
  lcdSend(LCD_CURSORSHIFT | LCD_DISPLAYMOVE | moveDir);
}

void lcdTextDirection(bool scrollLeft) {
  // 从光标处向前或向后写入文本
  if (scrollLeft) displaymode &= ~LCD_ENTRYLEFT;
  else displaymode |= LCD_ENTRYLEFT;
	lcdSend(LCD_ENTRYMODESET | displaymode);
}

void lcdAutoScroll(bool autoScroll) {
  // 在光标处每输入一个字符，先前文本左移
	if (autoScroll) displaymode |= LCD_ENTRYSHIFTINCREMENT; 
  else displaymode &= ~LCD_ENTRYSHIFTINCREMENT; 
	lcdSend(LCD_ENTRYMODESET | displaymode);
}

void lcdLoadCustom(uint8_t charLoc, uint8_t charmap[]) {
  // 加载自定义字符
  // 创建方法见 https://maxpromer.github.io/LCD-Character-Creator/
  // 8 行 x 5 位数组，位表示像素开/关
  // 例如定义并加载自定义字符（摄氏度符号）
  // uint8_t celsius[] = {B01000, B10100, B01011, B00100, B00100, B00100, B00011, B00000};
  // enum customChar {CELSIUS, CC1, CC2, CC3, CC4, CC5, CC6, CC7};
  // lcdLoadCustom(CELSIUS, celsius);
  // lcdWriteCustom(CELSIUS);
  if (charLoc > 7) LOG_WRN("custom char number %u out of range", charLoc);
  else {
  	charLoc &= 0x7; // CGRAM 加载位置 0 - 7
  	lcdSend(LCD_SETCGRAMADDR | (charLoc << 3));
  	for (int i=0; i<8; i++)	lcdSend(charmap[i], Rs);
  }
}

void lcdWriteCustom(uint8_t charLoc) {
  // 写入 8 个自定义字符之一
  if (charLoc > 7) LOG_WRN("custom char number %u out of range", charLoc);
  else lcdSend(charLoc, Rs);
}
#endif

/**************************** 初始化 ******************************/

bool checkI2Cdevice(const char* devName) {
  // 获取当前设备状态
  if (!strcmp(devName, "SSD1306")) return deviceStatus[SSD1306_BIaddr] || deviceStatus[SSD1306_Extaddr] ? true : false;
  if (!strcmp(devName, "PCF8591")) return deviceStatus[PCF8591addr];
  if (!strcmp(devName, "BMx280")) return deviceStatus[BMx280_Def] || deviceStatus[BMx280_Alt] ? true : false;
  if (!strcmp(devName, "MPU6050")) return deviceStatus[MPUxx50_HIGH] || deviceStatus[MPUxx50_LOW]  ? true : false;
  if (!strcmp(devName, "MPU9250")) return deviceStatus[MPUxx50_HIGH] || deviceStatus[MPUxx50_LOW]  ? true : false;
  if (!strcmp(devName, "DS3231")) return deviceStatus[DS3231_RTC];
  if (!strcmp(devName, "LCD1602")) return deviceStatus[LCD1602];
  LOG_WRN("Device name %s not recognised", devName);
  return false;
}

static bool prepI2Cdevices() {
  // 初始化可用的 I2C 设备
  if (I2Cdevices == 0) LOG_WRN("No I2C devices connected");
  else {
#if USE_SSD1306
    setupOled();
#endif
#if USE_BMx280
    setupBMx();
#endif
#if USE_MPU6050
    setupMPU6050();
#endif
#if USE_MPU9250
    setupMPU9250();
#endif
#ifdef ISCAM
    if (accelUse) startPollTask();
#endif
#if USE_DS3231
    setupRTC();
#endif
#if USE_LCD1602
    setupLCD1602();
#endif
    return true;
  }
  return false;
}

/************** 轮询加速度计检测运动 ***************/

// 供 MJPEG2SD 应用使用
#ifdef ISCAM

static TaskHandle_t sensorPollHandle = NULL;
bool accelUse = false;
int accelDeg = 2;
static bool haveMovement = false;

static void getAccelMove() {
  // 判断加速度计是否发生足够大的运动
  haveMovement = false;
  static int posData[2][3];
  int currMove = 0;
  for (int i = 0; i < 3; i++) {
    posData[0][i] = (int)mpuData[i];
    currMove += abs(posData[0][i] - posData[1][i]);
    posData[1][i] = posData[0][i];
  }
  if (currMove > accelDeg) haveMovement = true;
}

bool checkAccelMove() {
  return haveMovement;
}

static void sensorPollTask(void* p) {
  while (true) {
    getMPUdata();
    getAccelMove();
    delay(1000);
  }
}

static void startPollTask() {
  // 轮询输入传感器获取数据
  if (sensorPollHandle == NULL) xTaskCreateWithCaps(sensorPollTask, "sensorPollTask", SENSOR_STACK_SIZE, NULL, SENSOR_PRI, &sensorPollHandle, HEAP_MEM); 
}

#endif // ISCAM 结束

#endif // INCLUDE_I2C 结束
