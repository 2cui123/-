
// 使用 UART 接口在客户端 ESP 与辅助 ESP 之间通信
// 以支持客户端无法直接托管的外设
//
// 将辅助端 UART_TXD_PIN 引脚连接到客户端 UART_RXD_PIN 引脚
// 将辅助端 UART_RXD_PIN 引脚连接到客户端 UART_TXD_PIN 引脚
// 同时连接公共 GND
// 使用的 UART 编号和引脚通过网页配置
//
// 交换的数据共 8 字节：
// - 2 字节固定帧头
// - 1 字节命令字符
// - 4 字节数据，可为任意不超过 32 位的类型
// - 1 字节校验和
//
// 回调函数：
// - setOutputPeripheral()：在辅助端，将 UART 读取的 uint32_t 数据转换为对应外设数据类型并写入外设
// - getInputPeripheral()：在辅助端，读取输入外设并将数据类型转换为 uint32_t 经 UART 发送
// - setInputPeripheral()：在客户端，将 UART 读取的 uint32_t 转换为输入状态数据类型

#include "appGlobals.h"

#if INCLUDE_UART
#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 1, 0)
#error uart.cpp must be compiled with arduino-esp32 core v3.1.0 or higher
#endif
#include "driver/uart.h"

// UART 引脚
#define UART_RTS UART_PIN_NO_CHANGE
#define UART_CTS UART_PIN_NO_CHANGE

#define UART_BAUD_RATE 115200
#define BUFF_LEN (UART_HW_FIFO_LEN(0) * 2) // 128 * 2
#define MSG_LEN 8 

// 辅助端 UART 连接
int uartTxdPin;
int uartRxdPin;

TaskHandle_t uartRxHandle = NULL;
static QueueHandle_t uartQueue = NULL;
static SemaphoreHandle_t responseMutex = NULL;
static SemaphoreHandle_t writeMutex = NULL;
static uart_event_t uartEvent;
static byte uartBuffTx[BUFF_LEN];
static byte uartBuffRx[BUFF_LEN];
static const char* uartErr[] = {"FRAME_ERR", "PARITY_ERR", "UART_BREAK", "DATA_BREAK",
  "BUFFER_FULL", "FIFO_OVF", "UART_DATA", "PATTERN_DET", "EVENT_MAX"};
static const uint16_t header = 0x55aa; 
static uart_port_t uartId;

static bool readUart() {
  // 当 UART 有数据可读时读取
  // 等待事件发生
  if (xQueueReceive(uartQueue, (void*)&uartEvent, (TickType_t)portMAX_DELAY)) { 
    if (uartEvent.type != UART_DATA) {
      xQueueReset(uartQueue);
      uart_flush_input(uartId);
      LOG_WRN("Unexpected uart event type: %s", uartErr[uartEvent.type]);
      delay(1000);
      return false;
    } else {
      // UART 接收数据可用，等待完整消息
      int msgLen = 0;
      while (msgLen < MSG_LEN) {
        uart_get_buffered_data_len(uartId, (size_t*)&msgLen);
        delay(10);
      }
      heartBeatDone = true; // 隐含心跳
      msgLen = uart_read_bytes(uartId, uartBuffRx, msgLen, pdMS_TO_TICKS(20));
      uint16_t* rxPtr = (uint16_t*)uartBuffRx;
      if (rxPtr[0] != header) {
        // 使用 UART0 时忽略客户端重启期间收到的数据
        return false;
      } 
      // 有效消息帧头，检查内容是否正确
      byte checkSum = 0; // 校验和为数据内容求和取模 256
      for (int i = 0; i < MSG_LEN - 1; i++) checkSum += uartBuffRx[i];
      if (checkSum != uartBuffRx[MSG_LEN - 1]) {
        LOG_WRN("Invalid message ignored, got checksum %02x, expected %02x", uartBuffRx[MSG_LEN - 1], checkSum);
        return false;
      }
    }
  }
  return true;
}

bool writeUart(uint8_t cmd, uint32_t outputData) {
  // 准备并写入 UART 请求
  xSemaphoreTake(writeMutex, portMAX_DELAY);
  // 将待发送的外设数据加载到 UART 发送缓冲区
  memcpy(uartBuffTx, &header, 2);
  uartBuffTx[2] = cmd;
  memcpy(uartBuffTx + 3, &outputData, 4);
  uartBuffTx[MSG_LEN - 1] = 0; // 校验和为数据内容求和取模 256
  for (int i = 0; i < MSG_LEN - 1; i++) uartBuffTx[MSG_LEN - 1] += uartBuffTx[i];  
  bool res = uart_write_bytes(uartId, uartBuffTx, MSG_LEN) > 0 ? true : false;
  xSemaphoreGive(writeMutex);
  return res;
}

static bool configureUart() { 
  // 配置 UART 驱动参数
  uart_config_t uart_config = {
    .baud_rate = UART_BAUD_RATE,
    .data_bits = UART_DATA_8_BITS,
    .parity    = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .rx_flow_ctrl_thresh = 122,
#if CONFIG_IDF_TARGET_ESP32
    .source_clk = UART_SCLK_REF_TICK,
#endif
  };
  
  // 安装驱动并配置引脚
#if CONFIG_IDF_TARGET_ESP32C3 
  uartId = UART_NUM_1;
#else // ESP32、ESP32S3
  uartId = UART_NUM_2;
#endif
  esp_err_t res = uart_driver_install(uartId, BUFF_LEN, BUFF_LEN, 20, &uartQueue, 0);
  if (res == ESP_OK) res = uart_param_config(uartId, &uart_config);
  if (res == ESP_OK) res = uart_set_pin(uartId, uartTxdPin, uartRxdPin, UART_RTS, UART_CTS);
  if (res != ESP_OK) LOG_WRN("UART config failed: %s", espErrMsg(res));
  return (res == ESP_OK) ? true : false;
}

static void uartRxTask(void *arg) {
  // 辅助端用于从 UART 接收数据
  while (true) {
    // 等待前一次请求的响应处理完毕
    xSemaphoreTake(responseMutex, portMAX_DELAY); 
    if (readUart()) {
      // 更新对应外设状态
      uint32_t receivedData;
      memcpy(&receivedData, uartBuffRx + 3, 4); // 响应数据（如适用）
#ifdef AUXILIARY
      // 尝试输出请求
      if (!setOutputPeripheral(uartBuffRx[2], receivedData)) {
        // 尝试输入请求
        int receivedData = getInputPeripheral(uartBuffRx[2]); // 命令
        // 向客户端写回响应
        if (receivedData >= 0) writeUart(uartBuffRx[2], (uint32_t)receivedData); // 命令、数据
      }
#else
      // 客户端，处理接收到的输入
      setInputPeripheral(uartBuffRx[2], receivedData);
#endif
    }
    xSemaphoreGive(responseMutex);
  }
}

void prepUart() {
  // 若使用辅助端则初始化 UART
  if (useUart) {
    if (uartTxdPin && uartRxdPin) {
      LOG_INF("Prepare UART on pins Tx %d, Rx %d", uartTxdPin, uartRxdPin);
      responseMutex = xSemaphoreCreateMutex();
      writeMutex = xSemaphoreCreateMutex();
      if (configureUart()) {
#ifdef USE_UARTTASK
        xSemaphoreTake(responseMutex, portMAX_DELAY);
        xTaskCreateWithCaps(uartRxTask, "uartRxTask", UART_STACK_SIZE, NULL, UART_PRI, &uartRxHandle, HEAP_MEM);
#endif
        xSemaphoreGive(responseMutex);
        xSemaphoreGive(writeMutex);
      }
    } else LOG_WRN("At least one uart pin not defined");
  } 
}

#endif
