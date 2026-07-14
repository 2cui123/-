
// 可选外设支持：
// - 引脚传感器，如 PIR / 雷达
// - 舵机，如云台平移 / 俯仰 / 转向
// - DS18B20 温度传感器
// - 电池电压测量
// - 照明 LED 驱动（PWM 或 WS2812 / SK6812）
// - 三轴摇杆
// - MY9221 驱动的 LED 灯条，如 10 段 Grove LED Bar
// - 5 线 28BYJ-48 单极步进电机 + ULN2003 驱动
// - 4 线双极步进电机 + MX1508 H 桥驱动
//
// 外设可直接挂在主控 ESP 上，也可挂在独立的 IO 扩展 ESP 上
// （当主控可用引脚较少时，如 ESP-CAM 模块）
// 外部外设应具有较低数据速率且不要求快速响应，
// 因此中断驱动的输入引脚应由主控内部轮询监测。
// 需要时钟数据流的外设（如麦克风）不适用。
//
// 引脚编号必须 > 0。
//
// s60sc 2022 - 2025
//

#include "appGlobals.h"

#if INCLUDE_PERIPH
#include "driver/ledc.h"

// 已启用的外设
bool pirUse; // true 表示使用 PIR 进行运动检测
bool ledBarUse; // true 表示使用 LED 灯条
uint8_t lampLevel; // 板载照明 LED 亮度
bool lampAuto = false; // 为 true 时，配合 pirUse 与 accelUse，夜间 PIR 或加速度计触发时自动开灯
bool lampNight; // true 表示仅在夜间开灯（未使用）
int lampType; // 照明工作模式
bool voltUse; // true 表示通过 ADC 引脚监测电压（如电池）
bool stickUse; // true 表示使用摇杆
bool buzzerUse; // true 表示使用蜂鸣器
bool stepperUse; // true 表示使用步进电机
bool SVactive; // true 表示使用舵机
TaskHandle_t heartBeatHandle = NULL;
bool RCactive = false;

// 外设所用引脚

// 传感器
int pirPin; // pirUse 为 true 时使用
int lampPin;
int buzzerPin; // buzzerUse 为 true 时使用

// 摄像头舵机
int servoPanPin;
int servoTiltPin;

// 环境 / 模块温度读取
int ds18b20Pin; // INCLUDE_DS18B20 为 true 时使用

// 电池监测
// ESP32-CAM 模块仅可使用引脚 33，因其为唯一可用模拟引脚
int voltPin; 

// 外设附加配置
// 按具体舵机型号配置，如 SG90
int servoMinAngle; // 最小角度（度）
int servoMaxAngle;
int servoMinPulseWidth; // 最小脉宽（微秒）
int servoMaxPulseWidth;
int servoDelay; // 通过延时控制舵机角度变化速率
int servoCenter = 90; // 舵机居中角度（度）

// 电池监测配置
int voltDivider; // 分压电阻除数，用于将输入电压换算为实际电压
                 // 例：100k / 100k 分压时除数为 2
float voltLow; // 低于该电压时发送邮件告警
int voltInterval; // 电池电压检测间隔（分钟）

// 蜂鸣器持续时间
int buzzerDuration; // 蜂鸣时长（秒）

// 遥控引脚与控制参数
int servoSteerPin;
int lightsRCpin;
int heartbeatRC;
int maxSteerAngle;
int maxDutyCycle;
int minDutyCycle;
int maxTurnSpeed;
bool allowReverse;
bool autoControl;
int waitTime; 
int stickzPushPin; // 摇杆按键开关所接数字引脚
int stickXpin; // X 轴模拟引脚
int stickYpin; // Y 轴模拟引脚
int relayPin;
bool relayMode;

// MY9221 LED 灯条引脚
int ledBarClock;
int ledBarData;

// 步进电机驱动引脚
#define stepperPins 4 
uint8_t stepINpins[stepperPins];

static void doStep();
void setStickTimer(bool restartTimer, uint32_t interval);
void setLamp(uint8_t lampVal);


// 各引脚传感器 / 控制器功能

bool getPIRval() {
  // 读取 PIR 或雷达传感器状态
  return digitalRead(pirPin); 
}

void buzzerAlert(bool buzzerOn) {
  // 控制有源蜂鸣器
  if (buzzerUse) {
    if (buzzerOn) {
      // 打开蜂鸣器
      pinMode(buzzerPin, OUTPUT);
      digitalWrite(buzzerPin, HIGH); 
    } else digitalWrite(buzzerPin, LOW); // 关闭蜂鸣器
  }
}

// 使用两个舵机控制云台，或控制遥控舵机
// 仅在 SG90 类舵机上测试过
// 典型接线：
// - 橙线：信号
// - 红线：5V
// - 棕线：GND
//
#define PWM_FREQ 50 // 赫兹
#define DUTY_BIT_DEPTH 12 // ESP32-C3 最大为 14 位

TaskHandle_t servoHandle = NULL;
static int newTiltVal, newPanVal, newSteerVal;
static int oldPanVal, oldTiltVal, oldSteerVal; 

static int dutyCycle (int angle) {
  // 根据角度计算占空比
  angle = constrain(angle, servoMinAngle, servoMaxAngle);
  int pulseWidth = map(angle, servoMinAngle, servoMaxAngle, servoMinPulseWidth, servoMaxPulseWidth);
  return pow(2, DUTY_BIT_DEPTH) * pulseWidth * PWM_FREQ / USECS;
}

static int changeAngle(uint8_t servoPin, int newVal, int oldVal, bool useDelay = true) {
  // 改变指定舵机角度
  int incr = newVal - oldVal > 0 ? 1 : -1;
  for (int angle = oldVal; angle != newVal + incr; angle += incr) {
    ledcWrite(servoPin, dutyCycle(angle));
    if (useDelay) delay(servoDelay); // 控制变化速率
  }
  return newVal;
}

static void servoTask(void* pvParameters) {
  // 根据用户输入更新舵机位置
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (newSteerVal != oldSteerVal) oldSteerVal = changeAngle(servoSteerPin, newSteerVal, oldSteerVal, false);
    if (newPanVal != oldPanVal) oldPanVal = changeAngle(servoPanPin, newPanVal, oldPanVal);
    if (newTiltVal != oldTiltVal) oldTiltVal = changeAngle(servoTiltPin, newTiltVal, oldTiltVal);
  }
}

void setCamPan(int panVal) {
  // 设置摄像头平移角度
  newPanVal = panVal;
  if (servoPanPin && servoHandle != NULL) xTaskNotifyGive(servoHandle);
}

void setCamTilt(int tiltVal) {
  // 设置摄像头俯仰角度
  newTiltVal = tiltVal;
  if (servoTiltPin && servoHandle != NULL) xTaskNotifyGive(servoHandle);
}

void setSteering(int steerVal) {
  // 设置转向角度
  newSteerVal = steerVal;
  if (servoSteerPin && servoHandle != NULL) xTaskNotifyGive(servoHandle);
}

static void prepServos() {
  if (SVactive) {
    if (servoPanPin) ledcAttach(servoPanPin, PWM_FREQ, DUTY_BIT_DEPTH); 
    else LOG_WRN("No servo pan pin defined");
    if (servoTiltPin) ledcAttach(servoTiltPin, PWM_FREQ, DUTY_BIT_DEPTH);
    else LOG_WRN("No servo tilt pin defined");
  }
  if (RCactive && servoSteerPin) ledcAttach(servoSteerPin, PWM_FREQ, DUTY_BIT_DEPTH);
  oldPanVal = oldTiltVal = oldSteerVal = servoCenter + 1;

  if (SVactive || (RCactive && servoSteerPin)) {
    xTaskCreateWithCaps(&servoTask, "servoTask", SERVO_STACK_SIZE, NULL, SERVO_PRI, &servoHandle, HEAP_MEM); 
    // 初始角度
    if (servoPanPin) setCamPan(servoCenter);
    if (servoTiltPin) setCamTilt(servoCenter);
    if (servoSteerPin) setSteering(servoCenter);
  }
}


/* 从上述引脚连接的 DS18B20 读取温度
    使用 Arduino 库管理器安装 OneWire 和 DallasTemperature
    DS18B20 为单总线数字温度传感器
    正面从左到右引脚：GND、数据、3V3
    需在 3V3 与数据线之间接 4.7k 上拉电阻
    因读取温度需约 750ms 延时，在独立任务中运行

    若无 DS18B20，则使用 ESP 片内温度传感器
*/

#if INCLUDE_DS18B20
#if __has_include("../libraries/DallasTemperature/DallasTemperature.h") 
#include <OneWire.h> // https://github.com/PaulStoffregen/OneWire
#include <DallasTemperature.h> // https://github.com/milesburton/Arduino-Temperature-Control-Library
#else
#error "Need to install DallasTemperature and OneWire libraries"
#endif
#endif

// 配置
static float dsTemp = NULL_TEMP;
TaskHandle_t DS18B20handle = NULL;
static bool haveDS18B20 = false;

static void DS18B20task(void* pvParameters) {
#if INCLUDE_DS18B20
  // 从 DS18B20 获取当前温度
  OneWire oneWire(ds18b20Pin);
  DallasTemperature sensors(&oneWire);
  while (true) {
    dsTemp = NULL_TEMP;
    sensors.begin();
    uint8_t deviceAddress[8];
    sensors.getAddress(deviceAddress, 0);
    if (deviceAddress[0] == 0x28) {
      uint8_t tryCnt = 10;
      while (tryCnt) {
        sensors.requestTemperatures(); 
        dsTemp = sensors.getTempCByIndex(0);
        // 忽略偶发的错误读数
        if (dsTemp > NULL_TEMP) tryCnt = 10;
        else tryCnt--;
        delay(1000);
      }   
    } 
    // 重新尝试初始化 ds18b20
    delay(10000);
  }
#endif
}

void prepTemperature() {
#if INCLUDE_DS18B20
  if (ds18b20Pin) {
    xTaskCreateWithCaps(&DS18B20task, "DS18B20task", DS18B20_STACK_SIZE, NULL, DS18B20_PRI, &DS18B20handle, HEAP_MEM); 
    haveDS18B20 = true;
    LOG_INF("Using DS18B20 sensor");
  } else LOG_WRN("No DS18B20 pin defined, using chip sensor if present");
#endif
}

float readTemperature(bool isCelsius, bool onlyDS18) {
  // 返回最新温度；isCelsius 为 true 返回摄氏度，否则华氏度；读数失败时返回错误值
  if (onlyDS18) return dsTemp;
  if (!haveDS18B20) dsTemp = readInternalTemp();
  return (dsTemp > NULL_TEMP) ? (isCelsius ? dsTemp : (dsTemp * 1.8) + 32.0) : dsTemp;
}

float getNTCcelsius (uint16_t resistance, float oldTemp) {
  // 将 NTC 热敏电阻阻值转换为摄氏度
  double Temp = log(resistance);
  Temp = 1 / (0.001129148 + (0.000234125 + (0.0000000876741 * Temp * Temp )) * Temp);
  Temp = (Temp == 0) ? oldTemp : Temp - 273.15; // 读数为 0 时沿用上次温度
  return (float) Temp;
}

/************ 电池电压监测 ************/

// 从 ADC 引脚读取电池电压
// 若电池电压超过 3.3V，需通过分压电阻降至 3.3V 以下
static float currentVoltage = -1.0; // -1 表示未启用监测
TaskHandle_t battHandle = NULL;

float readVoltage()  {
  return currentVoltage;
}

static void battTask(void* parameter) {
    if (voltInterval < 1) voltInterval = 1;

    // 本地状态：闪烁与告警
    static bool wasLow = false;
    static bool blinkOn = false;
    static bool sentExtAlert = false;
    static unsigned long lastBlinkMs = 0;
    static unsigned long lastSampleMs = 0;

    const uint16_t BLINK_MS = 500;        // 闪烁周期
    const float    HYST     = 0.15f;      // 恢复低电量状态的电压回差

    // 将分钟设置转换为毫秒，采用非阻塞定时采样
    const unsigned long SAMPLE_MS = (unsigned long)voltInterval * 60UL * 1000UL;

    for (;;) {
        unsigned long now = millis();

        // 1) 按设定间隔采样电池电压（非阻塞）
        if (now - lastSampleMs >= SAMPLE_MS || lastSampleMs == 0) {
            lastSampleMs = now;

            // analogReadMilliVolts() 不可用，沿用现有换算方式
            currentVoltage = (float)(smoothAnalog(voltPin)) * 3.3f * voltDivider / MAX_ADC;

            // 更新低电量锁存与告警
            if (currentVoltage < voltLow) {
                if (!wasLow && !sentExtAlert) {
                    sentExtAlert = true; // 每次会话仅告警一次
                    char battMsg[20];
                    sprintf(battMsg, "Voltage is %.2fV", currentVoltage);
                    externalAlert("Low battery", battMsg);
                }
                wasLow = true;
            } else if (wasLow && currentVoltage >= voltLow + HYST) {
                // 电压明显恢复，退出低电量状态
                wasLow = false;
                blinkOn = false;

                // 若照明开启则恢复常亮，否则关闭
                // 若在其他处保存了目标亮度（如 lampLevel 0..15），可在此使用
                if (lightsRCpin > 0) setLightsRC(/* 目标常亮状态 */ false);

                // 不强制改写照明，交由正常控制逻辑恢复
            }
        }

        // 2) 低电量期间持续闪烁（覆盖其他控制）
        if (wasLow) {
            if (now - lastBlinkMs >= BLINK_MS) {
                lastBlinkMs = now;
                blinkOn = !blinkOn;
            }

            // 每轮循环写入，避免被其他代码覆盖
            setLamp(blinkOn ? 15 : 0);                  // 最大亮度闪烁，可按需调整
            if (lightsRCpin > 0) setLightsRC(blinkOn);   // 同步遥控照明引脚
        }

        // 3) 短暂让出 CPU（保持闪烁平滑且不阻塞）
        vTaskDelay(pdMS_TO_TICKS(20)); // 约 50Hz 服务频率
    }

    vTaskDelete(NULL);
}

static void setupBatt() {
  if (voltUse) {
  	if (voltPin) {
      xTaskCreateWithCaps(&battTask, "battTask", BATT_STACK_SIZE, NULL, BATT_PRI, &battHandle, HEAP_MEM);
      LOG_INF("Monitor batt voltage");
      debugMemory("setupBatt");
    } else LOG_WRN("No voltage pin defined");
  }
}

/********************* 照明 LED 驱动 **********************/

#define RGB_BITS 24  // WS2812 / SK6812 为 24 位 RGB 颜色
static bool lampInit = false;
#if defined(USE_WS2812)
static rmt_data_t ledData[RGB_BITS];
#endif

static void setupLamp() {
  // 按板型配置照明 LED
  // 假定 LED 为高电平有效（ESP32 引脚 4 的板载灯为高有效，引脚 33 的信号灯为低有效）
  lampInit = false;
#if defined(LED_GPIO_NUM)
  if (lampPin <= 0) {
    lampPin = LED_GPIO_NUM;
    char lampPinStr[3];
    sprintf(lampPinStr, "%d", lampPin);
    updateStatus("lampPin", lampPinStr);
  }
#endif

  if (lampPin) {
    lampInit = true;
#if defined(USE_WS2812)
    // WS2812 RGB 高亮 LED
    if (rmtInit(lampPin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000)) 
      LOG_INF("Setup WS2812 Lamp Led on pin %d", lampPin);
    else {
      LOG_WRN("Failed to setup WS2812 on pin %u", lampPin);
      lampInit = false;
    }
#else
    // 假定 PWM LED
    ledcAttach(lampPin, 5000, DUTY_BIT_DEPTH); // 频率、分辨率
    setLamp(0);
    LOG_INF("Setup PWM Lamp Led on pin %d", lampPin);
#endif
  }
  if (lightsRCpin > 1) pinMode(lightsRCpin, OUTPUT);
}

void setLamp(uint8_t lampVal) {
  // 控制照明状态
  if (lampPin) {
    if (!lampInit) setupLamp();
    if (lampInit) {
#if defined(USE_WS2812)
      // WS2812 LED：白色，lampVal 控制亮度（0 关，15 最亮）
      uint8_t RGB[3]; // 每色 8 位
      lampVal = lampVal == 15 ? 255 : lampVal * 16;
      for (uint8_t i = 0; i < 3; i++) {
        RGB[i] = lampVal;
        // 按 WS2812 协议编码每一位的时序
        for (uint8_t j = 0; j < 8; j++) { 
          int bit = (i * 8) + j;
          if ((RGB[i] << j) & 0x80) { // 从最高位开始
            // 位 = 1
            ledData[bit].level0 = 1;
            ledData[bit].duration0 = 8;
            ledData[bit].level1 = 0;
            ledData[bit].duration1 = 4;
          } else {
            // 位 = 0
            ledData[bit].level0 = 1;
            ledData[bit].duration0 = 4;
            ledData[bit].level1 = 0;
            ledData[bit].duration1 = 8;
          }
        }
      }
      rmtWrite(lampPin, ledData, RGB_BITS, RMT_WAIT_FOR_EVER);
#else
      // 假定 PWM LED，用 PWM 设置亮度（0 关，15 最亮）
      uint8_t valueMax = 15;
      uint32_t duty = (pow(2, DUTY_BIT_DEPTH) / valueMax) * min(lampVal, valueMax);
      ledcWrite(lampPin, duty);
#endif
    }
  }
}

void twinkleLed(uint8_t ledPin, uint16_t interval, uint8_t blinks) {
  // LED 闪烁，blinks 为闪烁次数，interval 为间隔（毫秒）
  bool ledState = true;
  for (int i=0; i<blinks*2; i++) {
    digitalWrite(ledPin, ledState);
    delay(interval);
    ledState = !ledState;
  }
}

void setLightsRC(bool lightsOn) {
  // 遥控照明开关
  if (lightsRCpin > 0) digitalWrite(lightsRCpin, lightsOn);
}

static void prepPIR() {
  if (pirUse) {
    if (pirPin) pinMode(pirPin, INPUT_PULLDOWN); // 触发时为高电平
    else {
      pirUse = false;
      LOG_WRN("No PIR pin defined");
    }
  }
  if (relayPin) pinMode(relayPin, OUTPUT);
}

/********************************* 摇杆 *************************************/

// HW-504 摇杆
// X 轴用于转向，Y 轴用于电机，按键用于照明开关
// 需要 2 个模拟引脚和 1 个数字引脚，供电宜为 3.3V
// X 轴对应板子长边方向

static const int sRate = 1; // 每次模拟读数的采样次数
static int xOffset = 0; // X 轴零点偏移
static int yOffset = 0; // Y 轴零点偏移
static bool lightsChanged = false;
TaskHandle_t stickHandle = NULL;

static void IRAM_ATTR buttonISR() {
  // 摇杆按键按下，切换照明状态
  lightsChanged = !lightsChanged;
}

static void IRAM_ATTR stickISR() {
  // 定时器速率中断
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (stickHandle) {
    vTaskNotifyGiveFromISR(stickHandle, &xHigherPriorityTaskWoken); 
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

void setStickTimer(bool restartTimer, uint32_t interval) {
  // 设置摇杆轮询间隔或步进电机速度
  static hw_timer_t* stickTimer = NULL;
  // 若定时器在运行则先停止
  if (stickTimer) {
    timerDetachInterrupt(stickTimer); 
    timerEnd(stickTimer);
    stickTimer = NULL;
  }
  if (restartTimer) {
    // 按所需间隔（重新）启动定时器中断
    stickTimer = timerBegin(OneMHz); // 1 MHz
    timerAttachInterrupt(stickTimer, &stickISR);
    timerAlarm(stickTimer, interval, true, 0); // 单位：微秒
  }
}

static void stickTask (void *pvParameter) {
  static bool lightsStatus = false;
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (stickUse) {
      // 读取摇杆位置并补偿零点偏移
      int xPos = smoothAnalog(stickXpin, sRate);
      int steerAngle = (xPos > CENTER_ADC + xOffset) ? map(xPos, CENTER_ADC + xOffset, MAX_ADC, servoCenter, servoCenter + maxSteerAngle)
        : map(xPos, 0, CENTER_ADC + xOffset, servoCenter - maxSteerAngle, servoCenter); 
      setSteering(steerAngle);
      
      int yPos = smoothAnalog(stickYpin, sRate);
      // Y 轴反向，使向上为前进
      int motorCycle = (yPos > CENTER_ADC + yOffset) ? map(yPos, CENTER_ADC + yOffset, MAX_ADC, 0, 0 - maxDutyCycle)
        : map(yPos, 0, CENTER_ADC + yOffset, maxDutyCycle, 0); 
      if (abs(motorCycle) < minDutyCycle) motorCycle = 0; // 死区
#if INCLUDE_MCPWM
      motorSpeed(motorCycle);
#endif
      if (lightsChanged != lightsStatus) setLightsRC(lightsChanged);
      lightsStatus = lightsChanged;
      LOG_VRB("Xpos %d, Ypos %d, button %d", xPos, yPos, lightsStatus);
    }
    if (stepperUse) doStep();
  }
} 

static void prepJoystick() {
  if (stickUse) {
    if (stickXpin > 0 && stickYpin > 0) {
      // 在摇杆静止位置测定偏移
      xOffset = smoothAnalog(stickXpin, 8) - CENTER_ADC;
      yOffset = smoothAnalog(stickYpin, 8) - CENTER_ADC;
      LOG_VRB("X-offset: %d, Y-offset: %d", xOffset, yOffset);
      if (stickzPushPin > 0) {
        pinMode(stickzPushPin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(stickzPushPin), buttonISR, FALLING); 
      }
      if (stickHandle == NULL) xTaskCreateWithCaps(&stickTask, "stickTask", STICK_STACK_SIZE , NULL, STICK_PRI, &stickHandle, HEAP_MEM);
      setStickTimer(true, waitTime * 1000);
      LOG_INF("Joystick available");
    } else {
      stickUse = false;
      LOG_WRN("Joystick pins not defined");
    }
  }
}

/****************************** 步进电机 *************************************/

// 单极 28BYJ-48 减速步进电机 + ULN2003 驱动
// 双极通用电机 + MX1508 H 桥驱动
// 适用于四线步进驱动器的全步模式
// 使用 stickTask 与 stickTimer

#define stepPhases 4
#define modelTypes 2 // 须与 appGlobals.h 中 stepperModel 枚举项数量一致
static bool clockwise = false;
static const uint16_t stepsPerRevolution[modelTypes] = {32 * 64, 20}; // 28BYJ-48 减速圈数；8mm 双极圈数
static uint32_t stepsToDo = 0; // 待执行总步数
static uint8_t modelIndex = 0;
static uint8_t stepPhase = 0;
static const uint8_t pinSequence[stepPhases * modelTypes][stepperPins] = {
  // 28BYJ-48 单极全步相序
  {1, 1, 0, 0}, 
  {0, 1, 1, 0}, 
  {0, 0, 1, 1}, 
  {1, 0, 0, 1},
  // 8mm 双极半步相序
  {1, 0, 1, 0}, 
  {0, 1, 1, 0}, 
  {0, 1, 0, 1}, 
  {1, 0, 0, 1}, 
};


void setStepperPin(uint8_t pinNum, uint8_t pinPos) {
  // 引脚顺序为 IN1、IN2、IN3、IN4，以保证正确全步
  // 28BYJ-48 线色（从驱动板看）：蓝、粉、黄、橙、红
  // 双极线序：A+、A-、B+、B-
  stepINpins[pinPos] = pinNum;
}

static void prepStepper() {
  if (stepperUse) {
    if (stepINpins[0] > 0 && stepINpins[1] > 0) {
      stepPhase = 0;
      // 配置电机驱动引脚
      for (int i = 0; i < stepperPins; i++) {
        pinMode(stepINpins[i], OUTPUT);
        digitalWrite(stepINpins[i], LOW);
      }
      // stickTask 提供速度控制定时器
      if (stickHandle == NULL) xTaskCreateWithCaps(&stickTask, "stickTask", STICK_STACK_SIZE , NULL, STICK_PRI, &stickHandle, HEAP_MEM);   
      LOG_INF("Stepper motor on pins: %d, %d, %d, %d", stepINpins[0], stepINpins[1], stepINpins[2], stepINpins[3]);
      // 注意：上电后第一步可能不动作或方向相反
    } else {
      stepperUse = false;
      LOG_WRN("Stepper pins not defined");
    }
  }
}

static void nextPhase(bool changeDir = false) {
  // 确定下一相
  if (changeDir) clockwise = !clockwise; // 换向时调整下一相
  if (clockwise) {
    if (stepPhase-- <= 0) stepPhase = stepPhases - 1;
  } else {
    if (++stepPhase >= stepPhases) stepPhase = 0;
  }
}

void stepperRun(float RPM, float revFraction, bool _clockwise, stepperModel thisStepper) {
  // RPM：转速；revFraction：相对整圈的运动比例；thisStepper：电机型号
  stepsToDo = revFraction * stepsPerRevolution[thisStepper];
  modelIndex = stepPhases * thisStepper;
  if (clockwise != _clockwise) {
    // 换向：调整相序为反向
    nextPhase(true);  
    nextPhase();
  }
  uint32_t stepDelay = 60 * USECS / RPM; // 一圈时长（微秒）
  stepDelay /= stepsPerRevolution[thisStepper]; // 单步时长

  // 启动 stickTimer，由任务执行步进序列
  setStickTimer(false, 0); // 停止旧定时器
  setStickTimer(true, stepDelay);
}

static void doStep() {
  // 由 stickTask 调用，执行单步
  if (stepsToDo--) {
    for (int i = 0; i < stepperPins; i++) digitalWrite(stepINpins[i], pinSequence[modelIndex + stepPhase][i]);
    nextPhase();
  } else {
    // 步进序列完成
    setStickTimer(false, 0);  // 停止任务定时器
    for (int i = 0; i < stepperPins; i++) digitalWrite(stepINpins[i], LOW); // 断电节能
#if (INCLUDE_PGRAM && INCLUDE_PERIPH)
    stepperDone();
#endif
  }
}

/******************* MY9221 LED 灯条 ***************************/
/*
 带 MY9221 驱动的 LED 段条，如 Grove LED Bar
 接线：
    黑  GND
    红  3V3
    白  DCKI 时钟引脚
    黄  D1   数据引脚
    
 可作指示条，如显示音量电平
 */

#define MY9221_COUNT 12 // MY9221 最多可寻址 LED 数
#define LEDBAR_COUNT 10 // 灯条实际 LED 数量
#define LED_OFF 0x00
#define LED_FULL 0xFF

static bool reverse = true; // 点亮方向，true 为 Grove 灯条绿→红
static uint8_t ledLevel[LEDBAR_COUNT];

static void ledBarLatch() {
  // 触发内部锁存，将寄存器内容输出到 LED
  digitalWrite(ledBarClock, LOW); 
  delayMicroseconds(250); // 最小 220us
  // 内部锁存时序
  bool dataVal = false;
  for (uint8_t i = 0; i < 8; i++, dataVal = !dataVal) {
    digitalWrite(ledBarData, dataVal ? HIGH : LOW);
    delayMicroseconds(1); // 大于最小脉宽 230ns
  }
}

static void ledBarSend(uint16_t bits) {
  // 以 16 位时钟方式输出（8 位灰度时仅低 8 位有效）
  bool clockVal = false;
  for (int i = 15; i >= 0; i--, clockVal = !clockVal) {
    digitalWrite(ledBarData, (bits >> i) & 1 ? HIGH : LOW);
    digitalWrite(ledBarClock, clockVal ? HIGH : LOW);
  }
}

void ledBarClear() {
  for (uint8_t i = 0; i < LEDBAR_COUNT; i++) ledLevel[i] = LED_OFF;
}

void ledBrightness(uint8_t whichLed, float brightness) {
  // brightness 为 0.0~1.0，映射为 8 级亮度或关闭
  ledLevel[whichLed] |= (1 << (uint8_t)(8 * brightness)) - 1;
}

void ledBarUpdate() {
  // 将所需数值写入 MY9221 的 208 位寄存器
  if (ledBarUse) {
    ledBarSend(0); // 初始 16 位命令：8 位灰度模式及默认值
    // 12 路 × 16 位 LED 灰度 PWM
    for (uint8_t i = 0; i < LEDBAR_COUNT; i++) // 10 × 16 位
      ledBarSend(reverse ? ledLevel[LEDBAR_COUNT - 1 - i] : ledLevel[i]);
    // 未用通道填零
    for (uint8_t i = 0; i < MY9221_COUNT - LEDBAR_COUNT; i++) ledBarSend(LED_OFF);
    ledBarLatch();
  }
}
       
void ledBarGauge(float level) {
  // 按 0.0~1.0 的 level 设置点亮 LED 数量及亮度
  // 低位 LED 全亮，最高位 LED 按比例亮度
  level = abs(level);
  if (ledBarUse) {
    ledBarClear();
    uint8_t fullLedCnt = (uint8_t)(level * LEDBAR_COUNT);
    for (uint8_t i = 0; i < fullLedCnt; i++) ledLevel[i] = LED_FULL;
    // 设置最高位已点亮 LED 的亮度
    ledBrightness(fullLedCnt, (LEDBAR_COUNT * level) - fullLedCnt); 
    ledBarUpdate();
  }
}

static void prepLedBar() {
  // 初始化 LED 状态并配置引脚
  if (ledBarUse && ledBarClock && ledBarData) {
    pinMode(ledBarClock, OUTPUT);
    pinMode(ledBarData, OUTPUT);
    ledBarClear();
    ledBarUpdate();
    LOG_INF("Setup %d Led Bar with pins %d, %d", LEDBAR_COUNT, ledBarClock, ledBarData);
  } else ledBarUse = false;
}

/**********************************************/

void prepPeripherals() {
  // 初始化客户端或扩展板上的各外设
  setupADC();
  setupBatt();
  setupLamp();
  prepPIR();
  prepTemperature();
  prepServos();  
  prepJoystick();
  prepStepper();
  prepLedBar();
  debugMemory("prepPeripherals");
}

#endif
