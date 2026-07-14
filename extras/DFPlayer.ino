/**
 * ESP32 AI Thinker CAM 上的 DFPlayer Mini
 *
 * 接线（Serial2）：（实际引脚取决于所用 CAM 板）
 *   DFPlayer TX  -> GPIO 15
 *   DFPlayer RX  -> GPIO 14  [经 1kΩ 电阻]
 *   DFPlayer GND -> GND（需共地）
 *
 * 库：通过 Arduino 库管理器安装 DFRobotDFPlayerMini
 */

#include <Arduino.h>
#include "DFRobotDFPlayerMini.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ---- 引脚 ----
#define DFPLAYER_RX_PIN  15
#define DFPLAYER_TX_PIN  14

// ---- 命令类型 ----
typedef enum : uint8_t {
  CMD_NONE        = 0,
  CMD_PLAY        = 1,   // param = 曲目编号
  CMD_STOP        = 2,
  CMD_NEXT        = 3,
  CMD_PREV        = 4,
  CMD_VOLUME_UP   = 5,
  CMD_VOLUME_DOWN = 6,
  CMD_SET_VOLUME  = 7,   // param = 0-30
  CMD_LOOP        = 8,   // param = 曲目编号
  CMD_RANDOM      = 9,
} DFPlayerCmdType;

// ---- 共享命令结构 — 作为 pvParameters 传递 ----
typedef struct {
  DFPlayerCmdType type;
  int             param;
  SemaphoreHandle_t mutex;  // 保护 type 与 param
} DFPlayerSharedCmd;

// ---- 供 xTaskNotifyGive() 使用的任务句柄 ----
static TaskHandle_t       dfTaskHandle = nullptr;
static DFPlayerSharedCmd  sharedCmd;

bool sendCommand(DFPlayerCmdType type, int param = 0) {
  if (!dfTaskHandle) return false;

  if (xSemaphoreTake(sharedCmd.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println(F("[CMD] Mutex timeout — command dropped"));
    return false;
  }

  sharedCmd.type  = type;
  sharedCmd.param = param;

  xSemaphoreGive(sharedCmd.mutex);

  // 唤醒 DFPlayer 任务 — 增加其通知计数
  xTaskNotifyGive(dfTaskHandle);
  return true;
}

// 公共辅助函数 — 按需从 ESP32-CAM_MJPEG2SD 应用调用
bool playTrack(int trackNum) { return sendCommand(CMD_PLAY, trackNum);       }
bool setVolume(int vol)      { return sendCommand(CMD_SET_VOLUME, vol);      }
bool stopPlayback()          { return sendCommand(CMD_STOP);                 }


void dfPlayerTask(void *pvParameters) {
  // 将 pvParameters 转回共享命令结构
  DFPlayerSharedCmd *cmd = static_cast<DFPlayerSharedCmd *>(pvParameters);

  HardwareSerial dfSerial(2);
  DFRobotDFPlayerMini dfPlayer;

  dfSerial.begin(115200, SERIAL_8N1, DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
  vTaskDelay(pdMS_TO_TICKS(1000));

  if (!dfPlayer.begin(dfSerial, true, true)) {
    Serial.println(F("[DFTask] Init failed! Check wiring/SD card."));
    vTaskDelete(nullptr);
    return;
  }

  dfPlayer.setTimeOut(500);
  dfPlayer.volume(20);
  dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
  dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);

  Serial.print(F("[DFTask] Ready. Files on SD: "));
  Serial.println(dfPlayer.readFileCounts());

  while (true) {
    // 阻塞直到收到通知（或 50ms 超时用于事件轮询）。
    // pdTRUE = 退出时清除通知计数（充当二进制信号量）。
    uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

    if (notified > 0) {
      // 获取互斥锁，本地复制命令，立即释放
      DFPlayerCmdType type  = CMD_NONE;
      int             param = 0;

      if (xSemaphoreTake(cmd->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        type  = cmd->type;
        param = cmd->param;
        cmd->type = CMD_NONE;   // 消费命令
        xSemaphoreGive(cmd->mutex);
      }

      switch (type) {
        case CMD_PLAY:
          Serial.printf("[DFTask] Play track %d\n", param);
          dfPlayer.play(param);
          break;
        case CMD_STOP:
          Serial.println(F("[DFTask] Stop"));
          dfPlayer.stop();
          break;
        case CMD_NEXT:
          Serial.println(F("[DFTask] Next"));
          dfPlayer.next();
          break;
        case CMD_PREV:
          Serial.println(F("[DFTask] Prev"));
          dfPlayer.previous();
          break;
        case CMD_VOLUME_UP:
          dfPlayer.volumeUp();
          Serial.printf("[DFTask] Volume: %d\n", dfPlayer.readVolume());
          break;
        case CMD_VOLUME_DOWN:
          dfPlayer.volumeDown();
          Serial.printf("[DFTask] Volume: %d\n", dfPlayer.readVolume());
          break;
        case CMD_SET_VOLUME:
          dfPlayer.volume(param);
          Serial.printf("[DFTask] Volume set: %d\n", param);
          break;
        case CMD_LOOP:
          Serial.printf("[DFTask] Loop track %d\n", param);
          dfPlayer.loop(param);
          break;
        case CMD_RANDOM:
          Serial.println(F("[DFTask] Random"));
          dfPlayer.randomAll();
          break;
        case CMD_NONE:
        default:
          break;
      }
    }

    // ---- 轮询 DFPlayer 异步事件（超时或命令后执行） ----
    if (dfPlayer.available()) {
      uint8_t evType = dfPlayer.readType();
      int     value  = dfPlayer.read();
      switch (evType) {
        case DFPlayerPlayFinished:
          Serial.printf("[DFTask] Track %d finished\n", value);
          break;
        case DFPlayerCardInserted:
          Serial.println(F("[DFTask] SD inserted"));
          break;
        case DFPlayerCardRemoved:
          Serial.println(F("[DFTask] SD removed"));
          break;
        case DFPlayerError:
          Serial.printf("[DFTask] Error: %d\n", value);
          break;
        default:
          break;
      }
    }
  }
}

void setupDFPlayer() {
  Serial.println(F("DFPlayer ESP32 AI Thinker CAM"));

  // 任务启动前初始化共享命令结构
  sharedCmd.type  = CMD_NONE;
  sharedCmd.param = 0;
  sharedCmd.mutex = xSemaphoreCreateMutex();
  if (!sharedCmd.mutex) {
    Serial.println(F("Failed to create mutex!"));
    while (true) delay(1000);
  }

  xTaskCreate( dfPlayerTask, "DFPlayerTask", 4096, &sharedCmd, 2, &dfTaskHandle);

  Serial.println(F("Ready. Commands: p<n>=play, s=stop, n=next, b=prev, +/-=vol, v<0-30>=set vol, r=random"));
}

/********** 以下仅用于概念验证 *************/

// -------------------------------------------------------
// 串口命令处理 — 仅用于独立测试
// 通过 Arduino 监视器输入命令
// -------------------------------------------------------

void handleSerialCommands() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();
  if (input.length() == 0) return;

  char c = input.charAt(0);
  switch (c) {
    case 'p': case 'P': {
      int n = input.substring(1).toInt();
      if (n > 0) sendCommand(CMD_PLAY, n);
      else Serial.println(F("Usage: p<number>  e.g. p2"));
      break;
    }
    case 's': case 'S': sendCommand(CMD_STOP);        break;
    case 'n': case 'N': sendCommand(CMD_NEXT);        break;
    case 'b': case 'B': sendCommand(CMD_PREV);        break;
    case '+':           sendCommand(CMD_VOLUME_UP);   break;
    case '-':           sendCommand(CMD_VOLUME_DOWN); break;
    case 'r': case 'R': sendCommand(CMD_RANDOM);      break;
    case 'v': case 'V': {
      int vol = input.substring(1).toInt();
      if (vol >= 0 && vol <= 30) sendCommand(CMD_SET_VOLUME, vol);
      else Serial.println(F("Usage: v<0-30>  e.g. v15"));
      break;
    }
    default:
      Serial.println(F("Unknown command"));
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  setupDFPlayer();
}

void loop() {
  // 仅用于独立测试
  handleSerialCommands();
  vTaskDelay(pdMS_TO_TICKS(20));
}

