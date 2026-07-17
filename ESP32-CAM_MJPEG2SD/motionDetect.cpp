
/* 
 使用背景差分法检测连续图像中的运动。
 
 使用极小的（96x96）位图，既平滑图像以减少误报运动变化，
 又便于快速处理。
 位图可以是彩色或灰度。彩色需要三倍于灰度的内存和处理量。

 图像间变化量取决于帧率。
 帧率越高，需要的灵敏度越高。

 当帧尺寸改变时，OV2640 在切换过程中会输出若干异常帧。
 这些帧可能被误判为虚假运动。

 可结合机器学习进一步区分运动目标类型，
 例如人、动物、车辆等是否为感兴趣对象。 
*/

#include "appGlobals.h"

#if INCLUDE_TINYML
#include TINY_ML_LIB
#endif

#define RESIZE_DIM 96  // 缩放后运动位图的尺寸
#define RESIZE_DIM_SQ (RESIZE_DIM * RESIZE_DIM) // 位图像素数
#define INACTIVE_COLOR 96 // 非活动运动像素的颜色
#define JPEG_QUAL 80 // 生成运动检测 jpeg 的质量（%）
  
// 运动录制参数
bool dbgMotion = false;
int detectMotionFrames = 5; // 确认运动所需的最少连续变化帧数
int detectNightFrames = 10; // 连续暗帧数，用于避免昼夜切换误触发
// 定义感兴趣区域，即按需排除图像上下部分不参与运动检测
// 将图像分为 detectNumBands 个水平带，定义起始和结束带，1 = 顶部
int detectNumBands = 10;
int detectStartBand = 3;
int detectEndBand = 8; // 含结束带
int detectChangeThreshold = 15; // 像素比较表示变化的最小差值
uint8_t colorDepth; // 由 depthColor 配置设置
static size_t stride;
bool mlUse = false; // 是否用 ML 做运动检测，需 INCLUDE_TINYML 为 true
float mlProbability = 0.8; // 正类判定的最小概率（0.0 - 1.0）

uint8_t lightLevel; // 当前环境光照等级
uint8_t nightSwitch = 20; // 昼夜切换的初始白电平（%）
float motionVal = 8.0; // 初始运动灵敏度
uint8_t* motionJpeg = NULL;
size_t motionJpegLen = 0;
static uint8_t* currBuff = NULL;

#ifndef AUXILIARY

#if INCLUDE_NEW_JPG
// 使用 esp_new_jpeg 库替代内置实现
#include <esp_jpeg_dec.h>
#include <esp_jpeg_enc.h>

struct esp_jpeg_stream {
    jpeg_dec_handle_t       jpeg_dec;
    jpeg_dec_io_t*          jpeg_io;
    jpeg_dec_header_info_t* out_info;
    jpeg_pixel_format_t     output_type;
};
typedef struct esp_jpeg_stream* esp_jpeg_stream_handle_t;

static void jpgReduce(int inWidth, int inHeight, uint8_t downsize, int* outWidth, int* outHeight);
static bool jpg2rgbOpen(esp_jpeg_stream_handle_t jpegHandle, uint16_t width, uint16_t height);
static bool jpg2rgb(esp_jpeg_stream_handle_t jpegHandle, uint8_t* inputBuf, int inputLen, uint8_t* outputBuf);
static bool jpg2rgbClose(esp_jpeg_stream_handle_t jpegHandle);
static size_t rgb2jpg(uint8_t* rgb888, int width, int height, int qual, uint8_t* outputBuf);
#else
// 内置实现
static bool jpg2rgb(const uint8_t* src, size_t src_len, uint8_t* out, uint8_t scale);
#endif

/**********************************************************************************/


bool isNight(uint8_t nightSwitch) {
  // 检查是否为夜间（用于暂停录制）
  // 或用于启用时切换继电器
  static bool nightTime = false;
  static uint16_t nightCnt = 0;
  if (nightTime) {
    if (lightLevel > nightSwitch) {
      // 较亮图像
      nightCnt--;
      // 连续若干亮帧后判定为白天
      if (nightCnt == 0) {
        nightTime = false;
        LOG_INF("Day time");
      }
    }
  } else {
    if (lightLevel < nightSwitch) {
      // 较暗图像
      nightCnt++;
      // 连续若干暗帧后判定为夜间
      if (nightCnt > detectNightFrames) {
        nightTime = true;     
        LOG_INF("Night time"); 
      }
    }
  } 
  return nightTime;
}

static void rescaleImage(const uint8_t* input, int inputWidth, int inputHeight, uint8_t* output, int outputWidth, int outputHeight) {
  // 使用双线性插值缩放图像
  float xRatio = (float)inputWidth / (float)outputWidth;
  float yRatio = (float)inputHeight / (float)outputHeight;

  for (int i = 0; i < outputHeight; ++i) {
    for (int j = 0; j < outputWidth; ++j) {
      int xL = (int)floor(xRatio * j);
      int yL = (int)floor(yRatio * i);
      int xH = (int)ceil(xRatio * j);
      int yH = (int)ceil(yRatio * i);
      float xWeight = xRatio * j - xL;
      float yWeight = yRatio * i - yL;
      for (int channel = 0; channel < colorDepth; ++channel) {
        uint8_t a = input[(yL * inputWidth + xL) * colorDepth + channel];
        uint8_t b = input[(yL * inputWidth + xH) * colorDepth + channel];
        uint8_t c = input[(yH * inputWidth + xL) * colorDepth + channel];
        uint8_t d = input[(yH * inputWidth + xH) * colorDepth + channel];

        float pixel = a * (1 - xWeight) * (1 - yWeight) + b * xWeight * (1 - yWeight)
                    + c * yWeight * (1 - xWeight) + d * xWeight * yWeight;
        output[(i * outputWidth + j) * colorDepth + channel] = (uint8_t)pixel;
      }
    }
  }
}

static void rgbToGray(uint8_t* buffer, int width, int height) {
  // 将 rgb 缓冲区就地转为灰度
  for (int i = 0; i < width * height; ++i) {
    int index = i * 3;
    // 使用亮度公式计算灰度值
    buffer[i] = (uint8_t)(((77 * buffer[index]) + (150 * buffer[index + 1]) + (29 * buffer[index + 2])) >> 8);
  }
}

#if INCLUDE_TINYML

static int getImageData(size_t offset, size_t length, float *out_ptr) {
  // 复制为灰度或 RGB 特征
  size_t pixelPtr = offset * colorDepth;
  size_t out_ptr_idx = 0;
  while (out_ptr_idx < length) {
    out_ptr[out_ptr_idx++] = (colorDepth == RGB888_BYTES)  
      ? (float)((currBuff[pixelPtr] << 16) + (currBuff[pixelPtr + 1] << 8) + currBuff[pixelPtr + 2])
      : (float)((currBuff[pixelPtr] << 16) + (currBuff[pixelPtr] << 8) + currBuff[pixelPtr]);  
    pixelPtr += colorDepth;
  } 
  return 0;
}

static bool tinyMLclassify() {
  // 将输入数据转换为合适格式
  bool out = false;
  uint32_t dTime = millis(); 
  // 将位图缩放到分类器所需尺寸，并复制为灰度或 RGB 特征
  if (RESIZE_DIM != EI_CLASSIFIER_INPUT_WIDTH) {
    uint8_t* tempBuff = (uint8_t*)ps_malloc(EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * colorDepth);
    rescaleImage(currBuff, RESIZE_DIM, RESIZE_DIM, tempBuff, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);
    memcpy(currBuff, tempBuff, EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * colorDepth);
    free(tempBuff);
  }
  signal_t features_signal;
  features_signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  features_signal.get_data = &getImageData;

  // 运行分类器
  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR res = run_classifier(&features_signal, &result, false);
  if (res == EI_IMPULSE_OK) {
    if (result.classification[0].value > mlProbability) {
      out = true; // 分类匹配足够，保留运动检测
      if (dbgVerbose) {
        LOG_VRB("Prob: %0.2f, Timing: DSP %d ms, inference %d ms, anomaly %d ms", 
        result.classification[0].value, result.timing.dsp, result.timing.classification, result.timing.anomaly);
        char outcome[200] = {0};
        for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++)
          sprintf(outcome + strlen(outcome), "%s: %.2f, ", ei_classifier_inferencing_categories[i], result.classification[i].value);
        LOG_VRB("Predictions - %s in %ums", outcome, millis() - dTime);
      } 
    } 
  } else LOG_WRN("Failed to run classifier (%d)", res);
  return out;
}
#endif

bool checkMotion(camera_fb_t* fb, bool motionStatus, bool lightLevelOnly) {
  // 比较当前帧与上一帧的差异（背景差分）
  // 将 JPEG 图像转为缩小的 RGB888 或 8 位灰度位图
  if (fsizePtr > FRAMESIZE_SXGA) return false;
  uint32_t dTime = millis();
  uint32_t lux = 0;
  static uint32_t motionCnt = 0;
  static uint8_t fsizePtrPrev = 255; // 初始为无效值
  static uint8_t scaling, downsize;
  static uint16_t reducer;
  static int sampleWidth = 0, sampleHeight = 0;
  static uint8_t* rgbBuf = (uint8_t*)heap_caps_aligned_calloc(16, 1, frameData[FRAMESIZE_SXGA].frameWidth * frameData[FRAMESIZE_SXGA].frameHeight * RGB888_BYTES / 8, MALLOC_CAP_SPIRAM); // 必须 16 字节对齐，最大尺寸，无需释放
 #if INCLUDE_NEW_JPG
  static struct esp_jpeg_stream jpegHandle = {0};
  static uint8_t* jpgBuf = (uint8_t*)ps_malloc(RESIZE_DIM_SQ * RGB888_BYTES);
#endif  

  // 分辨率变化时计算采样尺寸参数
  if (fsizePtr != fsizePtrPrev) {
    fsizePtrPrev = fsizePtr;
    scaling = frameData[fsizePtr].scaleFactor; 
    reducer = frameData[fsizePtr].sampleRate;
    downsize = pow(2, scaling) * reducer;
    stride = (colorDepth == RGB888_BYTES) ? GRAYSCALE_BYTES : RGB888_BYTES; // stride 与 colorDepth 相反
    sampleWidth = frameData[fsizePtr].frameWidth / downsize;
    sampleHeight = frameData[fsizePtr].frameHeight / downsize;
#if INCLUDE_NEW_JPG
    jpg2rgbClose(&jpegHandle);
    jpgReduce(fb->width, fb->height, downsize, &sampleWidth, &sampleHeight);
    if (!jpg2rgbOpen(&jpegHandle, sampleWidth, sampleHeight)) return motionStatus;
#endif
  }
#if INCLUDE_NEW_JPG
  if (!jpg2rgb(&jpegHandle, fb->buf, fb->len, rgbBuf)) return motionStatus;
#else
  if (!jpg2rgb((uint8_t*)fb->buf, fb->len, rgbBuf, scaling)) return motionStatus;
#endif

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
  if (colorDepth == GRAYSCALE_BYTES) rgbToGray(rgbBuf, sampleWidth, sampleHeight);
#endif
  LOG_VRB("JPEG to rescaled %s bitmap conversion %u bytes in %lums", colorDepth == RGB888_BYTES ? "color" : "grayscale", sampleWidth * sampleHeight * colorDepth, millis() - dTime);
  
  // 在堆上分配缓冲区
  size_t resizeDimLen = RESIZE_DIM_SQ * colorDepth; // 位图字节大小
  if (motionJpeg == NULL) motionJpeg = (uint8_t*)ps_malloc(32 * 1024);
  if (currBuff == NULL) currBuff = (uint8_t*)ps_malloc(RESIZE_DIM_SQ * RGB888_BYTES);
  static uint8_t* prevBuff = (uint8_t*)ps_malloc(RESIZE_DIM_SQ * RGB888_BYTES);
  static uint8_t* changeMap = (uint8_t*)ps_malloc(RESIZE_DIM_SQ * RGB888_BYTES);
  
  dTime = millis();
  rescaleImage(rgbBuf, sampleWidth, sampleHeight, currBuff, RESIZE_DIM, RESIZE_DIM);
  LOG_VRB("Bitmap rescale to %u bytes in %lums", resizeDimLen, millis() - dTime);
  // 将当前帧每个像素与上一帧比较
  dTime = millis();
  int changeCount = 0;
  // 设置图像中的水平感兴趣区域
  uint16_t startPixel = (RESIZE_DIM*(detectStartBand-1)/detectNumBands) * RESIZE_DIM * colorDepth;
  uint16_t endPixel = (RESIZE_DIM*(detectEndBand)/detectNumBands) * RESIZE_DIM * colorDepth;
  int moveThreshold = ((endPixel-startPixel)/colorDepth) * (11-motionVal)/100; // 构成运动的像素变化数量
  for (int i = 0; i < resizeDimLen; i += colorDepth) {
    uint16_t currPix = 0, prevPix = 0;
    for (int j = 0; j < colorDepth; j++) {
      currPix += currBuff[i + j];
      prevPix += prevBuff[i + j];
    }
    currPix /= colorDepth;
    prevPix /= colorDepth;
    lux += currPix; // 用于计算光照等级
    uint8_t pixVal = 255; // 在 changeMap 图像中将活动变化像素显示为亮红色
    // 为运动跟踪调试设置显示图像
    if (dbgMotion) for (int j = 0; j < RGB888_BYTES; j++) changeMap[(i * stride) + j] = currPix; // 灰度
    // 判断像素变化状态
    if (abs((int)currPix - (int)prevPix) > detectChangeThreshold) {
      if (i > startPixel && i < endPixel) changeCount++; // 变化像素数量
      else pixVal = 80; // 在 changeMap 图像中将非活动变化像素显示为暗红色
      if (dbgMotion) {
        changeMap[(i * stride) + 2] = pixVal;
        for (int j = 0; j < RGB888_BYTES - 1; j++) changeMap[(i * stride) + j] = 0;
      }
    }
  }
  lightLevel = (lux*100)/(RESIZE_DIM_SQ*255); // 光照值（%）
  nightTime = isNight(nightSwitch);
  memcpy(prevBuff, currBuff, resizeDimLen); // 保存图像供下次比较
  LOG_VRB("Detected %u changes, threshold %u, light level %u, in %lums", changeCount, moveThreshold, lightLevel, millis() - dTime);
  if (lightLevelOnly) return false; // 不做运动检测，仅计算光照等级
  if (dbgMotion) {
    // 流式传输时显示运动检测以便调参
    if (!motionJpegLen) {
      // 准备下一次运动映射以供流式传输
      dTime = millis();
      // 为调试流式传输构建 changeMap 的 jpeg
#if INCLUDE_NEW_JPG
      motionJpegLen = rgb2jpg(changeMap, RESIZE_DIM, RESIZE_DIM, JPEG_QUAL, jpgBuf);
      if (motionJpegLen == 0) LOG_WRN("motionDetect: encode() failed"); 
      memcpy(motionJpeg, jpgBuf, motionJpegLen); 
#else
      uint8_t* jpg_buf = NULL;
      if (!fmt2jpg(changeMap, resizeDimLen, RESIZE_DIM, RESIZE_DIM, PIXFORMAT_RGB888, JPEG_QUAL, &jpg_buf, &motionJpegLen))
        LOG_WRN("motionDetect: fmt2jpg() failed"); 
      memcpy(motionJpeg, jpg_buf, motionJpegLen); 
      free(jpg_buf);
      jpg_buf = NULL;
#endif
      xSemaphoreGive(motionSemaphore);
      LOG_VRB("Created changeMap JPEG %d bytes in %lums", motionJpegLen, millis() - dTime);
    }
  } else {
    // 正常运动检测
    dTime = millis();
    if (!nightTime && changeCount > moveThreshold) {
      LOG_VRB("### Change detected");
      motionCnt++; // 连续变化次数
      // 需要最少连续变化序列才判定为有效运动
      if (!motionStatus && motionCnt >= detectMotionFrames) {
        LOG_VRB("***** Motion - START");
        motionStatus = true; // 运动开始
#if INCLUDE_TINYML
        // 将图像交给 TinyML 分类
        if (mlUse) if (!tinyMLclassify()) motionCnt = 0; // 未通过分类则取消运动
#endif
        dTime = millis();
#if INCLUDE_MQTT
        if (mqtt_active && motionCnt) {
          sprintf(jsonBuff, "{\"MOTION\":\"ON\",\"TIME\":\"%s\"}", esp_log_system_timestamp());
          mqttPublish(jsonBuff);
          mqttPublishPath("motion", "on");
#if INCLUDE_HASIO
          mqttPublishPath("cmd", "still");
#endif
        }
#endif
      } 
    } else motionCnt = 0;
  
    if (motionStatus && !motionCnt) {
      // 变化不足或未通过运动分类
      LOG_VRB("***** Motion - STOP");
      motionStatus = false; // 运动停止
#if INCLUDE_MQTT
      if (mqtt_active) {
        sprintf(jsonBuff, "{\"MOTION\":\"OFF\",\"TIME\":\"%s\"}", esp_log_system_timestamp());
        mqttPublish(jsonBuff);
        mqttPublishPath("motion", "off");
      }
#endif
    } 
    if (motionStatus) LOG_VRB("*** Motion - ongoing %u frames", motionCnt);
  }
  
  if (dbgVerbose) checkMemory();  
  LOG_VRB("============================");
  // motionStatus 表示此前是否已有运动进行中
  return nightTime ? false : motionStatus;
}

/*****************************************************************************************************/

#if INCLUDE_NEW_JPG

// 需已安装 espressif__esp_new_jpeg 库

static void jpgReduce(int inWidth, int inHeight, uint8_t downsize, int* outWidth, int* outHeight) {
  // 先缩小，再将宽高向上取整到 8 的倍数，同时保持宽高比
  uint8_t roundTo8 = 8; // 新宽高必须是 8 的倍数
  // 计算原始宽高比
  inWidth /= downsize;
  inHeight /= downsize;
  float aspectRatio = (float)(inWidth) / inHeight;

  auto roundUpToMultiple = [](int n, int m) {
    // 将 n 向上取整到 m 的倍数
    return ((n + m - 1) / m) * m;
  };

  // 确定较大维度
  int newLarger = inWidth;
  int newSmaller = inHeight;   
  if (inWidth < inHeight) {
    newLarger = inHeight;
    newSmaller = inWidth;
  }

  // 将较大维度向上取整到 8 的倍数
  newLarger = roundUpToMultiple(inWidth, roundTo8);
  
  // 根据新较大维度和原始宽高比计算新较小维度，再向上取整
  newSmaller = (int)(ceil((float)newLarger / aspectRatio));
  newSmaller = roundUpToMultiple(newSmaller, roundTo8);

  // 更新返回值
  *outWidth = newLarger;
  *outHeight = newSmaller;
  if (inWidth < inHeight) {
    *outWidth = newSmaller;
    *outHeight = newLarger;
  }
}

static bool jpg2rgbOpen(esp_jpeg_stream_handle_t jpegHandle, uint16_t width, uint16_t height) {
  // 配置 jpeg 处理器
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB888;
  config.rotate = JPEG_ROTATE_0D;
  config.scale.width = width;
  config.scale.height = height;
  jpegHandle->output_type = JPEG_PIXEL_FORMAT_RGB888;

  // 创建 jpeg_dec 句柄
  jpeg_error_t ret = jpeg_dec_open(&config, &jpegHandle->jpeg_dec);
  if (ret != JPEG_ERR_OK) {
    LOG_ERR("Unable to create jpeg decoder handle: %d", ret);
    return false;
  }

  // 创建 io_callback 句柄
  jpegHandle->jpeg_io = (jpeg_dec_io_t*)calloc(1, sizeof(jpeg_dec_io_t));
  if (jpegHandle->jpeg_io == NULL) {
    LOG_ERR("Insufficient memory to create input handle");
    jpg2rgbClose(jpegHandle);
    return false;
  }

  // 创建 out_info 句柄
  jpegHandle->out_info = (jpeg_dec_header_info_t*)calloc(1, sizeof(jpeg_dec_header_info_t));
  if (jpegHandle->out_info == NULL) {
    LOG_ERR("Insufficient memory to create output handle");
    jpg2rgbClose(jpegHandle);
    return false;
  }
  return true;
}

static bool jpg2rgb(esp_jpeg_stream_handle_t jpegHandle, uint8_t* inputBuf, int inputLen, uint8_t* outputBuf) {
  // 将 jpeg 解码为 rgb888
  // 设置 io_callback 的输入缓冲区和长度
  jpegHandle->jpeg_io->inbuf = inputBuf;
  jpegHandle->jpeg_io->inbuf_len = inputLen;

  // 解析 jpeg 文件头供解码器使用
  jpeg_error_t ret = jpeg_dec_parse_header(jpegHandle->jpeg_dec, jpegHandle->jpeg_io, jpegHandle->out_info);
  if (ret != JPEG_ERR_OK) {
    LOG_ERR("Failed to parse jpeg header: %d", ret);
    return false;
  }

  // 将 jpeg 解码到 outputBuf
  jpegHandle->jpeg_io->outbuf = outputBuf;
  ret = jpeg_dec_process(jpegHandle->jpeg_dec, jpegHandle->jpeg_io);
  if (ret != JPEG_ERR_OK) {
    LOG_ERR("Failed to decode jpeg: %d", ret);
    return false;
  }
  return true;
}

static bool jpg2rgbClose(esp_jpeg_stream_handle_t jpegHandle) {
   // 分辨率变化时移除旧的流句柄
  jpeg_error_t ret = jpeg_dec_close(jpegHandle->jpeg_dec);
  if (jpegHandle->jpeg_io) free(jpegHandle->jpeg_io);
  if (jpegHandle->out_info) free(jpegHandle->out_info);
  return ret == JPEG_ERR_OK ? true : false;
}

size_t rgb2jpg(uint8_t* rgb888, int width, int height, int qual, uint8_t* outputBuf) {
  // 将 rgb888 编码为 jpeg
  static bool firstCall = true;
  static jpeg_enc_handle_t jpeg_enc = NULL;
  static int bufLen = width * height * RGB888_BYTES;
  jpeg_error_t ret = JPEG_ERR_OK;

  if (firstCall) {
    firstCall = false;
    // 配置编码器
    jpeg_enc_config_t jpeg_enc_cfg = DEFAULT_JPEG_ENC_CONFIG();
    jpeg_enc_cfg.width = width;
    jpeg_enc_cfg.height = height;
    jpeg_enc_cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    jpeg_enc_cfg.subsampling = JPEG_SUBSAMPLE_420;
    jpeg_enc_cfg.quality = qual;
    jpeg_enc_cfg.rotate = JPEG_ROTATE_0D;
    jpeg_enc_cfg.task_enable = false;
    jpeg_enc_cfg.hfm_task_priority = 13;
    jpeg_enc_cfg.hfm_task_core = 1;

    // 打开编码器
    ret = jpeg_enc_open(&jpeg_enc_cfg, &jpeg_enc);
    if (ret != JPEG_ERR_OK) {
      LOG_ERR("Failed to open decoder: %d");
      return 0;
    }
  }

  // 编码
  int jpgLen = 0;
  ret = jpeg_enc_process(jpeg_enc, rgb888, bufLen, outputBuf, bufLen, &jpgLen);
  if (ret != JPEG_ERR_OK) LOG_ERR("Failed to encode: %d", ret);

  //jpeg_enc_close(jpeg_enc); // 保持打开
  return (size_t)jpgLen;
}

#else

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)

// 基于 esp32-camera/to_bmp.c 中的 jpg2rgb888()，以便使用缩放功能

static uint8_t work[3100]; // JPEG 解码器默认大小为 3.1kB

static bool jpg2rgb(const uint8_t* src, size_t src_len, uint8_t* out, uint8_t scale) {
  esp_jpeg_image_cfg_t jpeg_cfg = {
      .indata = (uint8_t *)src,
      .indata_size = src_len,
      .outbuf = out,
      .outbuf_size = UINT32_MAX, // 原文如此，待办：假设非常激进，暂保持以免破坏现有代码
      .out_format = JPEG_IMAGE_FORMAT_RGB888,
      .out_scale = (esp_jpeg_image_scale_t)scale,
      .flags = {.swap_color_bytes = 0},
      .advanced = {
        .working_buffer = work,
        .working_buffer_size = sizeof(work)
      }
  };
  esp_jpeg_image_output_t output_img = {};
  esp_err_t res = esp_jpeg_decode(&jpeg_cfg, &output_img);
  if (res != ESP_OK) LOG_WRN("jpg2rgb failure: %s", espErrMsg(res)); 
  return (res == ESP_OK) ? true : false;
}

#else

// 适用于 arduino-esp32 3.2.1 及更早版本

/************* 自 esp32-camera/to_bmp.c 复制并修改，以访问 jpg_scale_t *****************/

typedef struct {
  uint16_t width;
  uint16_t height;
  uint16_t data_offset;
  const uint8_t *input;
  uint8_t *output;
} rgb_jpg_decoder;

static bool _rgb_write(void * arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t *data) {
  // mpjpeg2sd：修改为生成 24 位 RGB 或 8 位灰度
  rgb_jpg_decoder * jpeg = (rgb_jpg_decoder *)arg;
  if (!data){
    if (x == 0 && y == 0) {
      // 写入开始
      jpeg->width = w;
      jpeg->height = h;
    } 
    return true;
  }

  size_t jw = jpeg->width*RGB888_BYTES;
  size_t t = y * jw;
  size_t b = t + (h * jw);
  size_t l = x * RGB888_BYTES;
  uint8_t *out = jpeg->output+jpeg->data_offset;
  uint8_t *o = out;
  size_t iy, ix;
  w *= RGB888_BYTES;

  for (iy=t; iy<b; iy+=jw) {
    o = out+(iy+l)/stride;
    for (ix=0; ix<w; ix+=RGB888_BYTES) {
      if (colorDepth == RGB888_BYTES) {
        o[ix] = data[ix+2];
        o[ix+1] = data[ix+1];
        o[ix+2] = data[ix];
      } else {
        uint16_t grayscale = (data[ix+2]+data[ix+1]+data[ix])/RGB888_BYTES;
        o[ix/RGB888_BYTES] = (uint8_t)grayscale;
      }
    }
    data+=w;
  }
  return true;
}

static unsigned int _jpg_read(void * arg, size_t index, uint8_t *buf, size_t len) {
  rgb_jpg_decoder * jpeg = (rgb_jpg_decoder *)arg;
  if (buf) memcpy(buf, jpeg->input + index, len);
  return len;
}

static bool jpg2rgb(const uint8_t* src, size_t src_len, uint8_t* out, uint8_t scale) {
  rgb_jpg_decoder jpeg;
  jpeg.width = 0;
  jpeg.height = 0;
  jpeg.input = src;
  jpeg.output = out;
  jpeg.data_offset = 0;
  esp_err_t res = esp_jpg_decode(src_len, (jpg_scale_t)scale, _jpg_read, _rgb_write, (void*)&jpeg);
  if (res != ESP_OK) LOG_WRN("jpg2rgb failure: %s", espErrMsg(res)); 
  return (res == ESP_OK) ? true : false;
}

#endif // ESP_ARDUINO_VERSION

#endif // INCLUDE_NEW_JPG

#else 
// 占位函数
bool isNight(uint8_t nightSwitch) {return false;}

#endif // AUXILIARY
