// 不同开发板的摄像头引脚定义

#if defined(CAMERA_MODEL_WROVER_KIT)
#define CAM_BOARD "CAMERA_MODEL_WROVER_KIT"
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    21
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27

#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      19
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM       5
#define Y2_GPIO_NUM       4
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

#elif defined(CAMERA_MODEL_ESP_EYE)
#define CAM_BOARD "CAMERA_MODEL_ESP_EYE"
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    4
#define SIOD_GPIO_NUM    18
#define SIOC_GPIO_NUM    23

#define Y9_GPIO_NUM      36
#define Y8_GPIO_NUM      37
#define Y7_GPIO_NUM      38
#define Y6_GPIO_NUM      39
#define Y5_GPIO_NUM      35
#define Y4_GPIO_NUM      14
#define Y3_GPIO_NUM      13
#define Y2_GPIO_NUM      34
#define VSYNC_GPIO_NUM   5
#define HREF_GPIO_NUM    27
#define PCLK_GPIO_NUM    25

#define LED_GPIO_NUM     22

#elif defined(CAMERA_MODEL_M5STACK_PSRAM)
#define CAM_BOARD "CAMERA_MODEL_M5STACK_PSRAM"
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    15
#define XCLK_GPIO_NUM     27
#define SIOD_GPIO_NUM     25
#define SIOC_GPIO_NUM     23

#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       39
#define Y5_GPIO_NUM        5
#define Y4_GPIO_NUM       34
#define Y3_GPIO_NUM       35
#define Y2_GPIO_NUM       32
#define VSYNC_GPIO_NUM    22
#define HREF_GPIO_NUM     26
#define PCLK_GPIO_NUM     21

#elif defined(CAMERA_MODEL_M5STACK_V2_PSRAM)
#define CAM_BOARD "CAMERA_MODEL_M5STACK_V2_PSRAM"
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    15
#define XCLK_GPIO_NUM     27
#define SIOD_GPIO_NUM     22
#define SIOC_GPIO_NUM     23

#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       39
#define Y5_GPIO_NUM        5
#define Y4_GPIO_NUM       34
#define Y3_GPIO_NUM       35
#define Y2_GPIO_NUM       32
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     26
#define PCLK_GPIO_NUM     21

#elif defined(CAMERA_MODEL_M5STACK_WIDE)
#define CAM_BOARD "CAMERA_MODEL_M5STACK_WIDE"
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    15
#define XCLK_GPIO_NUM     27
#define SIOD_GPIO_NUM     22
#define SIOC_GPIO_NUM     23

#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       39
#define Y5_GPIO_NUM        5
#define Y4_GPIO_NUM       34
#define Y3_GPIO_NUM       35
#define Y2_GPIO_NUM       32
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     26
#define PCLK_GPIO_NUM     21

#define LED_GPIO_NUM       2

#elif defined(CAMERA_MODEL_M5STACK_ESP32CAM)
#define CAM_BOARD "CAMERA_MODEL_M5STACK_ESP32CAM"
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    15
#define XCLK_GPIO_NUM     27
#define SIOD_GPIO_NUM     25
#define SIOC_GPIO_NUM     23

#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       39
#define Y5_GPIO_NUM        5
#define Y4_GPIO_NUM       34
#define Y3_GPIO_NUM       35
#define Y2_GPIO_NUM       17
#define VSYNC_GPIO_NUM    22
#define HREF_GPIO_NUM     26
#define PCLK_GPIO_NUM     21

#elif defined(CAMERA_MODEL_M5STACK_UNITCAM)
#define CAM_BOARD "CAMERA_MODEL_M5STACK_UNITCAM"
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    15
#define XCLK_GPIO_NUM     27
#define SIOD_GPIO_NUM     25
#define SIOC_GPIO_NUM     23

#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       39
#define Y5_GPIO_NUM        5
#define Y4_GPIO_NUM       34
#define Y3_GPIO_NUM       35
#define Y2_GPIO_NUM       32
#define VSYNC_GPIO_NUM    22
#define HREF_GPIO_NUM     26
#define PCLK_GPIO_NUM     21

#elif defined(CAMERA_MODEL_M5STACK_CAMS3_UNIT)
// 随附 PY260 摄像头（MEGA_CCM_PID）
#define USE_PY260 // 若未使用 PY260 请注释掉
#define CAM_BOARD "CAMERA_MODEL_M5STACK_CAMS3_UNIT"
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM 21
#define XCLK_GPIO_NUM  11
#define SIOD_GPIO_NUM  17
#define SIOC_GPIO_NUM  41

#define Y9_GPIO_NUM    13
#define Y8_GPIO_NUM    4
#define Y7_GPIO_NUM    10
#define Y6_GPIO_NUM    5
#define Y5_GPIO_NUM    7
#define Y4_GPIO_NUM    16
#define Y3_GPIO_NUM    15
#define Y2_GPIO_NUM    6
#define VSYNC_GPIO_NUM 42
#define HREF_GPIO_NUM  18
#define PCLK_GPIO_NUM  12

#define LED_GPIO_NUM 14

// SD 卡引脚
#define SD_MMC_CLK 39
#define SD_MMC_CMD 38
#define SD_MMC_D0 40
// 片选引脚为 GPIO9，SD_MMC 模式下不需要
// 麦克风引脚
#define I2S_SD 48 // PDM 麦克风
#define I2S_WS 47
#define I2S_SCK -1 


#elif defined(CAMERA_MODEL_AI_THINKER) || defined(SIDE_ALARM)
#define CAM_BOARD "CAMERA_MODEL_AI_THINKER"
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// 4 为闪光灯 LED，33 为信号 LED
#define LED_GPIO_NUM      4

#elif defined(CAMERA_MODEL_TTGO_T_JOURNAL)
#define CAM_BOARD "CAMERA_MODEL_TTGO_T_JOURNAL"
#define PWDN_GPIO_NUM      0
#define RESET_GPIO_NUM    15
#define XCLK_GPIO_NUM     27
#define SIOD_GPIO_NUM     25
#define SIOC_GPIO_NUM     23

#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       39
#define Y5_GPIO_NUM        5
#define Y4_GPIO_NUM       34
#define Y3_GPIO_NUM       35
#define Y2_GPIO_NUM       17
#define VSYNC_GPIO_NUM    22
#define HREF_GPIO_NUM     26
#define PCLK_GPIO_NUM     21

#elif defined(CAMERA_MODEL_XIAO_ESP32S3)
#define CAM_BOARD "CAMERA_MODEL_XIAO_ESP32S3"
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

#define LED_GPIO_NUM 43
// 定义 SD 卡引脚
#define SD_MMC_CLK 7 
#define SD_MMC_CMD 9
#define SD_MMC_D0 8
// 定义麦克风引脚
#define I2S_SD 41 // PDM 麦克风
#define I2S_WS 42
#define I2S_SCK -1 

#elif defined(CAMERA_MODEL_ESP32_CAM_BOARD)
#define CAM_BOARD "CAMERA_MODEL_ESP32_CAM_BOARD"
// 板载 18 针排针上 Y5 与 Y3 对调
#define USE_BOARD_HEADER 0 
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   33
#define XCLK_GPIO_NUM     4
#define SIOD_GPIO_NUM    18
#define SIOC_GPIO_NUM    23

#define Y9_GPIO_NUM      36
#define Y8_GPIO_NUM      19
#define Y7_GPIO_NUM      21
#define Y6_GPIO_NUM      39
#if USE_BOARD_HEADER
#define Y5_GPIO_NUM      13
#else
#define Y5_GPIO_NUM      35
#endif
#define Y4_GPIO_NUM      14
#if USE_BOARD_HEADER
#define Y3_GPIO_NUM      35
#else
#define Y3_GPIO_NUM      13
#endif
#define Y2_GPIO_NUM      34
#define VSYNC_GPIO_NUM    5
#define HREF_GPIO_NUM    27
#define PCLK_GPIO_NUM    25

#elif defined(CAMERA_MODEL_ESP32S3_CAM_LCD)
#define CAM_BOARD "CAMERA_MODEL_ESP32S3_CAM_LCD"
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     40
#define SIOD_GPIO_NUM     17
#define SIOC_GPIO_NUM     18

#define Y9_GPIO_NUM       39
#define Y8_GPIO_NUM       41
#define Y7_GPIO_NUM       42
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       3
#define Y4_GPIO_NUM       14
#define Y3_GPIO_NUM       47
#define Y2_GPIO_NUM       13
#define VSYNC_GPIO_NUM    21
#define HREF_GPIO_NUM     38
#define PCLK_GPIO_NUM     11

#elif defined(CAMERA_MODEL_ESP32S2_CAM_BOARD)
// 不支持 ESP32S2
#define CAM_BOARD "CAMERA_MODEL_ESP32S2_CAM_BOARD unsupported"
// 板载 18 针排针上 Y5 与 Y3 对调
#define USE_BOARD_HEADER 0
#define PWDN_GPIO_NUM     1
#define RESET_GPIO_NUM    2
#define XCLK_GPIO_NUM     42
#define SIOD_GPIO_NUM     41
#define SIOC_GPIO_NUM     18

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       39
#define Y7_GPIO_NUM       40
#define Y6_GPIO_NUM       15
#if USE_BOARD_HEADER
#define Y5_GPIO_NUM       12
#else
#define Y5_GPIO_NUM       13
#endif
#define Y4_GPIO_NUM       5
#if USE_BOARD_HEADER
#define Y3_GPIO_NUM       13
#else
#define Y3_GPIO_NUM       12
#endif
#define Y2_GPIO_NUM       14
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     4
#define PCLK_GPIO_NUM     3

#elif defined(CAMERA_MODEL_ESP32S3_EYE) || defined(CAMERA_MODEL_FREENOVE_ESP32S3_CAM) || defined(CAMERA_MODEL_ESP32_S3_CAM)
#if defined(CAMERA_MODEL_ESP32S3_EYE)
#define CAM_BOARD "CAMERA_MODEL_ESP32S3_EYE"
#elif defined(CAMERA_MODEL_FREENOVE_ESP32S3_CAM)
#define CAM_BOARD "CAMERA_MODEL_FREENOVE_ESP32S3_CAM"
#elif defined(CAMERA_MODEL_ESP32_S3_CAM)
#define CAM_BOARD "CAMERA_MODEL_ESP32_S3_CAM" // AI_THINKER 风格板
#endif

#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5

#define Y2_GPIO_NUM 11
#define Y3_GPIO_NUM 9
#define Y4_GPIO_NUM 8
#define Y5_GPIO_NUM 10
#define Y6_GPIO_NUM 12
#define Y7_GPIO_NUM 18
#define Y8_GPIO_NUM 17
#define Y9_GPIO_NUM 16

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

#if defined(CAMERA_MODEL_FREENOVE_ESP32S3_CAM) || defined(CAMERA_MODEL_ESP32_S3_CAM)
#define USE_WS2812 // 使用 WS2812 RGB LED
#endif
#ifdef USE_WS2812
#define LED_GPIO_NUM 48 // WS2812 RGB LED
#else
#define LED_GPIO_NUM 2 // 蓝色信号 LED
#endif

// 定义 SD 卡引脚
#define SD_MMC_CLK 39 
#define SD_MMC_CMD 38
#define SD_MMC_D0 40
#if defined(CAMERA_MODEL_ESP32_S3_CAM)
// 取消注释以下引脚以使用 SD MMC 4 位模式
//#define SD_MMC_D1 41
//#define SD_MMC_D2 14
//#define SD_MMC_D3 47
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
// 定义麦克风引脚
#define I2S_SD 2  // I2S 麦克风
#define I2S_WS 42
#define I2S_SCK 41
#endif


#elif defined(CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3) || defined(CAMERA_MODEL_DFRobot_Romeo_ESP32S3)
#define CAM_BOARD "CAMERA_MODEL_DFRobot_ESP32S3"
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     45
#define SIOD_GPIO_NUM     1
#define SIOC_GPIO_NUM     2

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       46
#define Y7_GPIO_NUM       8
#define Y6_GPIO_NUM       7
#define Y5_GPIO_NUM       4
#define Y4_GPIO_NUM       41
#define Y3_GPIO_NUM       40
#define Y2_GPIO_NUM       39
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     42
#define PCLK_GPIO_NUM     5

#define LED_GPIO_NUM     21
#if defined(CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3)
#define SD_MMC_CLK -1
#define SD_MMC_CMD -1
#define SD_MMC_D0 -1
#if SD_MMC_CLK == -1
#define NO_SD  // 无 SD 卡
#endif
#endif

#elif defined(CAMERA_MODEL_TTGO_T_CAMERA_PLUS)
#define CAM_BOARD "CAMERA_MODEL_TTGO_T_CAMERA_PLUS"
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    4
#define SIOD_GPIO_NUM    18
#define SIOC_GPIO_NUM    23

#define Y9_GPIO_NUM      36
#define Y8_GPIO_NUM      37
#define Y7_GPIO_NUM      38
#define Y6_GPIO_NUM      39
#define Y5_GPIO_NUM      35
#define Y4_GPIO_NUM      26
#define Y3_GPIO_NUM      13
#define Y2_GPIO_NUM      34
#define VSYNC_GPIO_NUM   5
#define HREF_GPIO_NUM    27
#define PCLK_GPIO_NUM    25

#define LED_GPIO_NUM     -1
// 定义 SD 卡引脚
#define SD_MMC_CLK 21 // 串行时钟（SCLK）
#define SD_MMC_CMD 19 // 主出从入（MOSI）
#define SD_MMC_D0 22  // 主入从出（MISO）

#elif defined(CAMERA_MODEL_NEW_ESPS3_RE1_0)
// 速卖通标有 RE:1.0 的板子，使用较慢的 8MB QSPI PSRAM，仅 4MB 可寻址
#define CAM_BOARD "CAMERA_MODEL_NEW_ESPS3_RE1_0"
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 10
#define SIOD_GPIO_NUM 21
#define SIOC_GPIO_NUM 14

#define Y9_GPIO_NUM 11
#define Y8_GPIO_NUM 9
#define Y7_GPIO_NUM 8
#define Y6_GPIO_NUM 6
#define Y5_GPIO_NUM 4
#define Y4_GPIO_NUM 2
#define Y3_GPIO_NUM 3
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 13
#define HREF_GPIO_NUM 12
#define PCLK_GPIO_NUM 7

#define USE_WS2812 // 使用 SK6812 RGB LED
#ifdef USE_WS2812
#define LED_GPIO_NUM 33 // SK6812 RGB LED
#else
#define LED_GPIO_NUM 34 // 绿色信号 LED
#endif
// 定义 SD 卡引脚
#define SD_MMC_CLK 42
#define SD_MMC_CMD 39
#define SD_MMC_D0 41
// 定义麦克风引脚
#define I2S_SD 35 // I2S 麦克风
#define I2S_WS 37
#define I2S_SCK 36 

#elif defined(CAMERA_MODEL_XENOIONEX)
#define CAM_BOARD "CAMERA_MODEL_XENOIONEX"
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    1 // 可使用
#define SIOD_GPIO_NUM    8 // 可使用其他 I2C SDA 引脚，设为 -1 | 若不用 I2C 则设为 8 或 47
#define SIOC_GPIO_NUM    9 // 可使用其他 I2C SCL 引脚，设为 -1 | 若不用 I2C 则设为 9 或 21

#define Y9_GPIO_NUM      3  //D7
#define Y8_GPIO_NUM      18 //D6
#define Y7_GPIO_NUM      42 //D5
#define Y6_GPIO_NUM      16 //D4
#define Y5_GPIO_NUM      41 //D3
#define Y4_GPIO_NUM      17 //D2
#define Y3_GPIO_NUM      40 //D1
#define Y2_GPIO_NUM      39 //D0
#define VSYNC_GPIO_NUM   45
#define HREF_GPIO_NUM    38
#define PCLK_GPIO_NUM    2

#define SD_MMC_CLK       13
#define SD_MMC_CMD       12
#define SD_MMC_D0        14

// I2S 引脚
#define I2S_SCK          4  // 串行时钟（SCK）或位时钟（BCLK）
#define I2S_WS           5  // 字选择（WS）或左右时钟（LRCLK）
#define I2S_SDI          6  // 串行数据输入（麦克风）
#define I2S_SDO          7  // 串行数据输出（功放）
//#define I2S_BCK          3  // 位时钟（BCLK）!!! Core V3 起不再需要
//#define I2S_LRC          11  // 左右时钟（LRCLK）!!! Core V3 起不再需要

#define TRIGGER         15 // PIR 或雷达触发

#define USE_WS2812
#define LED_GPIO_NUM     48

#elif defined(CAMERA_MODEL_UICPAL_ESP32)
#define CAM_BOARD "CAMERA_MODEL_UICPAL_ESP32"

// 摄像头
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM    5
#define XCLK_GPIO_NUM    15
#define SIOD_GPIO_NUM    21
#define SIOC_GPIO_NUM    22

#define Y9_GPIO_NUM       2
#define Y8_GPIO_NUM      13
#define Y7_GPIO_NUM      12
#define Y6_GPIO_NUM      32
#define Y5_GPIO_NUM      25
#define Y4_GPIO_NUM      27
#define Y3_GPIO_NUM      26
#define Y2_GPIO_NUM      33
#define VSYNC_GPIO_NUM   17
#define HREF_GPIO_NUM    16
#define PCLK_GPIO_NUM    14

// SD 卡
#define SD_MMC_CLK       18
#define SD_MMC_CMD       19
#define SD_MMC_D0        23


#elif defined(CAMERA_MODEL_Waveshare_ESP32_S3_ETH)
// Waveshare ESP32-S3-ETH 原理图见 https://files.waveshare.com/wiki/ESP32-S3-ETH/ESP32-S3-ETH-Schematic.pdf
#define CAM_BOARD "CAMERA_MODEL_Waveshare_ESP32_S3_ETH"
#define PWDN_GPIO_NUM    8  // 驱动摄像头电源的 MOSFET
#define RESET_GPIO_NUM   -1 // 
#define XCLK_GPIO_NUM    3  // 时钟
#define SIOD_GPIO_NUM    48 // SIO_DAT
#define SIOC_GPIO_NUM    47 // SIO_CLK

#define Y9_GPIO_NUM      18 // D7
#define Y8_GPIO_NUM      15 // D6
#define Y7_GPIO_NUM      38 // D5
#define Y6_GPIO_NUM      40 // D4
#define Y5_GPIO_NUM      42 // D3
#define Y4_GPIO_NUM      46 // D2
#define Y3_GPIO_NUM      45 // D1
#define Y2_GPIO_NUM      41 // D0
#define VSYNC_GPIO_NUM   1  // 也可能用 GP16，但通常未连接
#define HREF_GPIO_NUM    2  //
#define PCLK_GPIO_NUM    39 //

#define USE_WS2812          // 此板有 WS2812 RGB LED，在此定义
#define LED_GPIO_NUM     21 // WS2812B RGB LED

// 定义 SD 卡引脚
#define SD_MMC_CLK 7        //
#define SD_MMC_CMD 6        // CMD/DI/MOSI
#define SD_MMC_D0 5         // DAT0/D0/MISO
// 片选引脚为 GPIO4，有 10k 上拉，但 SD_MMC 模式下不需要

// 定义麦克风引脚（无板载麦克风）
#define I2S_SD 34           // I2S 麦克风
#define I2S_WS 33
#define I2S_SCK 35          // 时钟

// 此板以太网 W5500（SPI）引脚
#define ETH_MOSI 11
#define ETH_MISO 12
#define ETH_SCLK 13
#define ETH_CS   14
#define ETH_RST  9
#define ETH_INT  10


#elif defined(CAMERA_MODEL_DFRobot_ESP32_S3_AI_CAM)
// https://wiki.dfrobot.com/SKU_DFR1154_ESP32_S3_AI_CAM
#define CAM_BOARD "CAMERA_MODEL_DFRobot_ESP32_S3_AI_CAM"
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    5
#define SIOD_GPIO_NUM    8
#define SIOC_GPIO_NUM    9

#define Y9_GPIO_NUM      4
#define Y8_GPIO_NUM      6
#define Y7_GPIO_NUM      7
#define Y6_GPIO_NUM      14
#define Y5_GPIO_NUM      17
#define Y4_GPIO_NUM      21
#define Y3_GPIO_NUM      18
#define Y2_GPIO_NUM      16
#define VSYNC_GPIO_NUM   1
#define HREF_GPIO_NUM    2
#define PCLK_GPIO_NUM    15

#define LED_GPIO_NUM 3 
// IR 引脚 47

// 定义 SD 卡引脚
#define SD_MMC_CLK 12      //
#define SD_MMC_CMD 13      // CMD/DI/MOSI
#define SD_MMC_D0  11      // DAT0/D0/MISO
// 片选引脚为 GPIO10，SD_MMC 模式下不需要

// 定义麦克风引脚
#define I2S_SD 39 // PDM 麦克风
#define I2S_WS 38
#define I2S_SCK -1

// 定义功放引脚
#define I2S_BCLK  45 // I2S 功放
#define I2S_LRCLK 46
#define I2S_DIN   42
// 增益引脚 41，模式引脚 40


#elif defined(AUXILIARY)
#define CAM_BOARD "AUXILIARY"
#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM -1
#define SIOD_GPIO_NUM -1
#define SIOC_GPIO_NUM -1

#define Y9_GPIO_NUM -1
#define Y8_GPIO_NUM -1
#define Y7_GPIO_NUM -1
#define Y6_GPIO_NUM -1
#define Y5_GPIO_NUM -1
#define Y4_GPIO_NUM -1
#define Y3_GPIO_NUM -1
#define Y2_GPIO_NUM -1
#define VSYNC_GPIO_NUM -1
#define HREF_GPIO_NUM -1
#define PCLK_GPIO_NUM -1

#define NO_SD

#else
#error "Camera model not selected"
#endif

 
