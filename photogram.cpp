// 
// 摄影测量使用从不同角度拍摄的照片收集 3D 物体数据，
// 可由软件转换以创建 3D 图像，例如用于 3D 打印复制品。
// 为从不同角度拍摄照片，可在静态相机前以间隔旋转放置物体的转台。
//
// ESP 可用于控制转台（步进电机），并使用内置相机拍摄照片，
// 或远程控制 DSLR 相机快门。

// 转台可 3D 打印，由 28BYJ-48 步进电机配合 ULN2003 电机驱动。
// 
// 3D 打印转台示例：www.thingiverse.com/thing:4817279
// 连接 RS-60E3 远程开关控制 DSLR 的电路示例：github.com/ch3p4ll3/ESP-Intervallometer#how-to-make-your-intervallometer
// 使用 Meshroom 软件创建 3D 图像：alicevision.org/#meshroom
// 使用 Blender 软件转换并修改图像用于 3D 打印：www.blender.org

// 使用网页界面指定下方列出的参数和引脚。转台将完整旋转一圈，按所需照片数量
// 在固定间隔停止拍照。若使用 ESP 相机，照片以 JPEG 格式存储在 SD 卡上，
// 文件夹名称为按下 Start 按钮时的日期时间。
// 若启用 ESP 灯 LED，将用作闪光灯。

// s60sc 2024

#include "appGlobals.h"

#if INCLUDE_PGRAM 
#if !INCLUDE_PERIPH
#error "Need INCLUDE_PERIPH true"
#endif

// 使用网页界面指定以下参数
uint8_t numberOfPhotos; // 转台旋转一圈要拍摄的照片数量
float tRPM; // 所需转台 RPM
bool clockWise; // 转台旋转方向
uint8_t timeForFocus; // DSLR 自动对焦时间（秒）
// timeForPhoto 为拍照总允许时间（秒），需包括：
// - 等待转台稳定
// - 若需要则等待 ESP 灯 LED 点亮
// - 若需要则允许自动对焦的时间
// - 快门曝光时间
uint8_t timeForPhoto; 
int pinShutter; // RS-60E3 快门控制引脚
int pinFocus; // RS-60E3 对焦控制引脚
uint8_t photosDone = 0; // 只读：目前已拍摄照片数量
float gearing = 1; // 转台转一圈所需步进电机齿轮旋转圈数
bool extCam = false; // 是否使用外接 DSLR（true）或内置 ESP 相机（false）
bool PGactive = false; 

static float mRPM; // 由 tRPM 和 gearing 推导的步进 RPM
static TaskHandle_t pgramHandle = NULL;
static char pFolder[20];

#define MAX_RPM 15.0 // 允许的最大步进电机 RPM
#define shutterTime 100 // DSLR 快门开合允许时间（毫秒）

static void prepPgram() {
  if (extCam) {
    pinMode(pinShutter, OUTPUT); 
    if (pinFocus) pinMode(pinFocus, OUTPUT); 
    LOG_INF("External cam, shutter pin %d", pinShutter);
#ifndef AUXILIARY
  } else {
     // 使用内置相机
     lampAuto = true;
     useMotion = doRecording = doPlayback = timeLapseOn = false;   
     setLamp(0);
     // 创建文件夹
     time_t currEpoch = getEpoch();
     strftime(pFolder, sizeof(pFolder), "/%Y%m%d_%H%M%S", localtime(&currEpoch));
     STORAGE.mkdir(pFolder);
     LOG_INF("Built in cam, created photogrammetry folder %s", pFolder);
#endif
  }
}

static void getPhoto() {
#ifdef AUXILIARY
  LOG_WRN("Internal camera not available on auxiliary board");
  photosDone = numberOfPhotos;
  stepperDone();
#else
  // 使用内置 ESP 相机
  setLamp(lampLevel); // 若需要则打开灯 LED 作为闪光灯
  if (timeForPhoto * 1000 > MAX_FRAME_WAIT) delay((timeForPhoto * 1000) - MAX_FRAME_WAIT); // 等待转台稳定
  uint32_t startTime = millis();
  doKeepFrame = true;
  while (doKeepFrame && (millis() - startTime < MAX_FRAME_WAIT)) delay(100);
  if (!doKeepFrame && alertBufferSize) {
    // 创建文件名
    char pName[FILE_NAME_LEN];
    strcpy(pName, pFolder);
    time_t currEpoch = getEpoch();
    strftime(pName + strlen(pFolder), sizeof(pName), "/%Y%m%d_%H%M%S", localtime(&currEpoch));
    strcat(pName, JPG_EXT);
    File pFile = STORAGE.open(pName, FILE_WRITE);
    // 保存文件到 SD 卡
    pFile.write((uint8_t*)alertBuffer, alertBufferSize);
    pFile.close();
    LOG_INF("Photo %u of % u saved in %s", photosDone + 1, numberOfPhotos, pName);
    alertBufferSize = 0;
  } else LOG_WRN("Failed to get photo");
  setLamp(0);
#endif
}

static void takePhoto() {
  // 控制外接相机
  if (timeForFocus * 1000 > timeForPhoto * 1000 - shutterTime) timeForFocus = timeForPhoto - 1;
  uint32_t waitTime = (timeForPhoto - timeForFocus) * 1000 - shutterTime;
  delay(waitTime); // 等待转台稳定
  if (pinFocus) {
    // 若使用自动对焦
    digitalWrite(pinFocus, HIGH);
    delay(timeForFocus * 1000); // 等待自动对焦
  }
  digitalWrite(pinShutter, HIGH);
  delay(shutterTime);
  digitalWrite(pinShutter, LOW);
  if (pinFocus) digitalWrite(pinFocus, LOW);
  if (photosDone < numberOfPhotos) LOG_INF("Photo %u of %u taken", photosDone + 1, numberOfPhotos);
}

static void pgramTask (void *pvParameter) {
  // 在转台一圈内拍摄一系列照片
  // 转台旋转需要 gearing 次快门电机旋转
  if (numberOfPhotos == 0) {
#ifdef DEV_ONLY
    laserLevel();
#else
    LOG_WRN("Number of photos needs to be non zero");
#endif
  } else {
    float revFraction = 1.0 / (float)numberOfPhotos; // 即一圈的角分数
    photosDone = 0;
    prepPgram();
    LOG_INF("Start taking %u photos each %0.1f deg at %0.1f RPM", numberOfPhotos, revFraction * 360, tRPM);
    do {
      extCam ? takePhoto() : getPhoto();
      // 假设转台与电机同向旋转（电机齿轮在内侧）
      stepperRun(mRPM, revFraction * gearing, clockWise, BYJ_48); 
      // 等待步进任务完成
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    } while (++photosDone < numberOfPhotos);
    LOG_INF("Completed taking photos");
    if (extCam) {
      pinMode(pinShutter, INPUT); // 避免不必要的耗电
      if (pinFocus) pinMode(pinFocus, INPUT); // 避免不必要的耗电
    }
  }
  pgramHandle = NULL;
  vTaskDelete(NULL);
} 

void takePhotos(bool startPhotos) { 
  // 启动任务
  if (stepperUse) {
    if (startPhotos) {
      mRPM = tRPM * gearing;
      if (mRPM > MAX_RPM) LOG_WRN("Requested stepper RPM %0.1f is too high", mRPM);
      else {
        if (pgramHandle == NULL) xTaskCreateWithCaps(&pgramTask, "pgramTask", STICK_STACK_SIZE , NULL, STICK_PRI, &pgramHandle, HEAP_MEM);
        else LOG_WRN("pgramTask still running");
      }
    } else {
      LOG_INF("User aborted taking photos");
      photosDone = numberOfPhotos;
      stepperDone();
    }
  }
}

void stepperDone() {
  // 通知摄影测量任务进入下一步
  if (pgramHandle) xTaskNotifyGive(pgramHandle);
}

#endif
