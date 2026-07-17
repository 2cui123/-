// 通用 SD 卡和 Flash 存储工具
//
// 卡可使用 1 位数据线或 4 位数据线访问（若板卡支持）
// 4 位模式在 ESP32S3 上可能更快（取决于卡规格）
// 但需要额外 3 个引脚
/* 以下 #define 须在 camera_pins.h 中对应摄像头条目下声明
   1 位       4 位        
   SD_MMC_CMD  SD_MMC_CMD  
   SD_MMC_CLK  SD_MMC_CLK   
   SD_MMC_D0   SD_MMC_D0    
               SD_MMC_D1     
               SD_MMC_D2    
               SD_MMC_D3    
*/

#include "appGlobals.h"
#include "ff.h"
#include "vfs_fat_internal.h"

// 存储设置
int sdMinCardFreeSpace = 100; // 启用 sdFreeSpaceMode 操作前卡上最少剩余空间（MB）
int sdFreeSpaceMode = 1; // 0 - 不检查, 1 - 删除最旧目录, 2 - 将最旧目录上传到 FTP/HFS 后从 SD 删除 
bool formatIfMountFailed = true; // 挂载失败时自动格式化文件系统。设为 false 则不自动格式化。
static bool use1bitMode = true;
#if (!CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32S2)
static int sdmmcFreq = BOARD_MAX_SDMMC_FREQ; // 板卡特定的默认 SD_MMC 速度
#endif

enum fsInd {SDMMC, LITTLEFS, SPIFFSS, TBD};
static fsInd thisFS = TBD; 
static const char* fsTypes[] = {"SD_MMC", "LittleFS", "SPIFFS"};
static const char* fsPaths[] = {"/sdcard", "/littlefs", "/spiffs"};

// 保存按时间从新到旧排序的文件名/文件夹名列表
static std::vector<std::string> fileVec;
static auto currentDir = "/~current";
static auto previousDir = "/~previous";


static void infoSD() {
#if (!CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32S2)
  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) LOG_WRN("No SD card attached");
  else {
    char typeStr[8] = "UNKNOWN";
    if (cardType == CARD_MMC) strcpy(typeStr, "MMC");
    else if (cardType == CARD_SD) strcpy(typeStr, "SDSC");
    else if (cardType == CARD_SDHC) strcpy(typeStr, "SDHC");
    LOG_INF("SD card type %s, Size: %s, using %d bit mode @ %uMHz", typeStr, fmtSize(SD_MMC.cardSize()), use1bitMode ? 1 : 4, sdmmcFreq / 1000);
  }
#endif
}

static bool prepSD_MMC() {
  bool res = false;
#if (!CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32S2)
  if (psramFound()) heap_caps_malloc_extmem_enable(MIN_RAM); // 小阈值以强制向量分配至 PSRAM
  fileVec.reserve(1000);
  if (psramFound()) heap_caps_malloc_extmem_enable(MAX_RAM);
#if CONFIG_IDF_TARGET_ESP32S3
#if !defined(SD_MMC_CLK)
  LOG_WRN("SD card pins not defined");
  return false;
#else
 #if defined(SD_MMC_D1)
  // 假定 4 位模式
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0, SD_MMC_D1, SD_MMC_D2, SD_MMC_D3);
  use1bitMode = false;
 #else
  // 假定 1 位模式
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
 #endif
#endif
#endif
  
  res = SD_MMC.begin("/sdcard", use1bitMode, formatIfMountFailed, sdmmcFreq);
#if defined(CAMERA_MODEL_AI_THINKER)
  pinMode(4, OUTPUT);
  digitalWrite(4, 0); // 1 线模式下 sd_mmc 库仍会初始化引脚 4，将灯引脚完全关闭
#endif 
  if (res) {
    STORAGE.mkdir(DATA_DIR);
    infoSD();
  } else LOG_WRN("SD card mount failed");
#endif
  return res;
}

static void fileModifiedDate(const char* fileName, char* timebuf, size_t buflen) {
  // 返回文件最后修改日期
  timebuf[0] = 0;
  struct stat st;
  if (stat(fileName, &st) == 0) {
    struct tm tm;
    gmtime_r(&st.st_mtime, &tm);
    strftime(timebuf, buflen, "%d %b %Y %H:%M:%S", &tm);
  } else LOG_WRN("%s not found\n", fileName);
}

static void listFolder(const char* rootDir) { 
  // 列出文件夹内容
  LOG_INF("Sketch size %s", fmtSize(ESP.getSketchSize()));    
  File root = STORAGE.open(rootDir);
  File file = root.openNextFile();
  char fPath[FILE_NAME_LEN];
  char timebuf[30] = {0};
  while (file) {
    sprintf(fPath, "%s%s", fsPaths[thisFS], file.path());
    fileModifiedDate(fPath, timebuf, sizeof(timebuf));
    LOG_INF("File: %s, size: %s, Date: %s", file.path(), fmtSize(file.size()), timebuf);
    file = root.openNextFile();
  }
  char totalBytes[20];
  strcpy(totalBytes, fmtSize(STORAGE.totalBytes()));
  LOG_INF("%s: %s used of %s", fsTypes[thisFS], fmtSize(STORAGE.usedBytes()), totalBytes);
}

bool startStorage() {
  // 启动所需存储设备（SD 卡或 Flash 文件系统）
  bool res = false;
#if (!CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32S2)
  if ((fs::SDMMCFS*)&STORAGE == &SD_MMC) {
    thisFS = SDMMC;
    res = prepSD_MMC();
    if (res) listFolder(DATA_DIR);
    else snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Check SD card inserted");
    debugMemory("startStorage");
    return res; 
  }
#endif
  // SPIFFS 或 LittleFS 之一
  if (thisFS == TBD) {
#ifdef _SPIFFS_H_
    if ((fs::SPIFFSFS*)&STORAGE == &SPIFFS) {
      thisFS = SPIFFSS;
      res = SPIFFS.begin(formatIfMountFailed);
    }
#endif
#ifdef _LITTLEFS_H_
    if ((fs::LittleFSFS*)&STORAGE == &LittleFS) {
      thisFS = LITTLEFS;
      res = LittleFS.begin(formatIfMountFailed);
      // 若不存在则创建 data 目录
      if (res) LittleFS.mkdir(DATA_DIR);
    }
#endif
    if (res) {  
      // 列出文件系统上的文件详情
      const char* rootDir = thisFS == LITTLEFS ? DATA_DIR : "/";
      listFolder(rootDir);
    }
  } else {
    snprintf(startupFailure, SF_LEN, STARTUP_FAIL "Failed to mount %s", fsTypes[thisFS]);  
    dataFilesChecked = true; // 无文件系统时禁用 setupAssist 向导
  }
  debugMemory("startStorage");
  return res;
}

static void getOldestDir(char* oldestDir) {
  // 按日期名称获取最旧文件夹
  File root = STORAGE.open("/");
  File file = root.openNextFile();
  if (file) strcpy(oldestDir, file.path()); // 初始化最旧目录名
  while (file) {
    if (file.isDirectory() && strstr(file.name(), "System") == NULL // 忽略系统卷信息
        && strstr(DATA_DIR, file.name()) == NULL) { // 忽略 data 目录
      if (strcmp(oldestDir, file.path()) > 0) strcpy(oldestDir, file.path()); 
    }
    file = root.openNextFile();
  }
}

void inline getFileDate(File& file, char* fileDate) {
  // 获取文件最后写入日期字符串
  time_t writeTime = file.getLastWrite();
  struct tm lt;
  localtime_r(&writeTime, &lt);
  strftime(fileDate, sizeof(fileDate), "%Y-%m-%d %H:%M:%S", &lt);
}

bool checkFreeStorage() { 
  // 检查存储空间是否充足
  bool res = false;
  size_t freeSize = (size_t)((STORAGE.totalBytes() - STORAGE.usedBytes()) / ONEMEG);
  if (!sdFreeSpaceMode && freeSize < sdMinCardFreeSpace) 
    LOG_WRN("Space left %uMB is less than minimum %uMB", freeSize, sdMinCardFreeSpace);
  else {
    // 删除以腾出空间
    while (freeSize < sdMinCardFreeSpace) {
      char oldestDir[FILE_NAME_LEN];
      getOldestDir(oldestDir);
      LOG_WRN("Deleting oldest folder: %s %s", oldestDir, sdFreeSpaceMode == 2 ? "after uploading" : "");
#if INCLUDE_FTP_HFS
      if (sdFreeSpaceMode == 2) fsStartTransfer(oldestDir); // 传输后删除最旧文件夹
#endif
      deleteFolderOrFile(oldestDir);
      freeSize = (size_t)((STORAGE.totalBytes() - STORAGE.usedBytes()) / ONEMEG);
    }
    LOG_INF("Storage free space: %s", fmtSize(STORAGE.totalBytes() - STORAGE.usedBytes()));
    res = true;
  }
  return res;
} 

void setFolderName(const char* fname, char* fileName) {
  // 设置当前或上一文件夹 
  char partName[FILE_NAME_LEN];
  if (strchr(fname, '~') != NULL) {
    if (!strcmp(fname, currentDir)) {
      dateFormat(partName, sizeof(partName), true);
      strcpy(fileName, partName);
      LOG_INF("Current directory set to %s", fileName);
    }
    else if (!strcmp(fname, previousDir)) {
      struct timeval tv;
      gettimeofday(&tv, NULL);
      struct tm* tm = localtime(&tv.tv_sec);
      tm->tm_mday -= 1;
      time_t prev = mktime(tm);
      strftime(partName, sizeof(partName), "/%Y%m%d", localtime(&prev));
      strcpy(fileName, partName);
      LOG_INF("Previous directory set to %s", fileName);
    } else strcpy(fileName, ""); 
  } else strcpy(fileName, fname);
}

bool listDir(const char* fname, char* jsonBuff, size_t jsonBuffLen, const char* extension) {
  // 列出根目录下的日期文件夹，或某日期文件夹内的文件
  bool hasExtension = false;
  char partJson[200]; // 用于构建 SD 页面 JSON 缓冲区
  bool noEntries = true;
  char fileName[FILE_NAME_LEN];
  setFolderName(fname, fileName);

  // 检查是文件夹还是文件
  if (strstr(fileName, extension) != NULL) {
    // 已选择所需文件类型
    hasExtension = true;
    noEntries = true; 
    strcpy(jsonBuff, "{}");     
  } else {
    // 若非唯一字符则忽略前导 '/'
    bool returnDirs = strlen(fileName) > 1 ? (strchr(fileName+1, '/') == NULL ? false : true) : true; 
    // 打开相应文件夹以列出内容
    File root = STORAGE.open(fileName);
    if (strlen(fileName)) {
      if (!root) LOG_WRN("Failed to open directory %s", fileName);
      else if (!root.isDirectory()) LOG_WRN("Not a directory %s", fileName);
      LOG_VRB("Retrieving %s in %s", returnDirs ? "folders" : "files", fileName);
    }
    
    // 构建相应选项列表
    strcpy(jsonBuff, returnDirs ? "{" : "{\"/\":\".. [ Up ]\",");            
    File file = root.openNextFile();
    if (psramFound()) heap_caps_malloc_extmem_enable(MIN_RAM); // 小阈值以强制向量分配至 PSRAM
    while (file) {
      if (returnDirs && file.isDirectory() && strstr(DATA_DIR, file.name()) == NULL) {  
        // 构建文件夹列表，忽略 data 目录
        sprintf(partJson, "\"%s\":\"%s\",", file.path(), file.name());
        fileVec.push_back(std::string(partJson));
        noEntries = false;
      }
      if (!returnDirs && !file.isDirectory()) {
        // 构建文件列表
        if (strstr(file.name(), extension) != NULL) {
          sprintf(partJson, "\"%s\":\"%s %s\",", file.path(), file.name(), fmtSize(file.size()));
          fileVec.push_back(std::string(partJson));
          noEntries = false;
        }
      }
      file = root.openNextFile();
    }
    if (psramFound()) heap_caps_malloc_extmem_enable(MAX_RAM);
  }
  
  if (noEntries && !hasExtension) sprintf(jsonBuff, "{\"/\":\"List folders\",\"%s\":\"Go to current (today)\",\"%s\":\"Go to previous (yesterday)\"}", currentDir, previousDir);
  else {
    // 构建 JSON 字符串内容
    sort(fileVec.begin(), fileVec.end(), std::greater<std::string>());
    for (auto fileInfo : fileVec) {
      if (strlen(jsonBuff) + strlen(fileInfo.c_str()) < jsonBuffLen) strcat(jsonBuff, fileInfo.c_str());
      else {
        LOG_WRN("Too many folders/files to list %u+%u in %u bytes", strlen(jsonBuff), strlen(partJson), jsonBuffLen);
        break;
      }
    }
    jsonBuff[strlen(jsonBuff)-1] = '}'; // 去掉末尾逗号 
  }
  fileVec.clear();
  return hasExtension;
}

static void deleteOthers(const char* baseFile) {
#ifdef ISCAM
  // 若存在则删除对应的 CSV 和 SRT 文件
  char otherDeleteName[FILE_NAME_LEN];
  strcpy(otherDeleteName, baseFile);
  changeExtension(otherDeleteName, CSV_EXT);
  if (STORAGE.remove(otherDeleteName)) LOG_INF("File %s deleted", otherDeleteName);
  changeExtension(otherDeleteName, SRT_EXT);
  if (STORAGE.remove(otherDeleteName)) LOG_INF("File %s deleted", otherDeleteName);
#endif  
}

void deleteFolderOrFile(const char* deleteThis) {
  // 删除指定文件或文件夹，除非是保留文件夹
  char fileName[FILE_NAME_LEN];
  setFolderName(deleteThis, fileName);
  File df = STORAGE.open(fileName);
  if (!df) {
    LOG_WRN("Failed to open %s", fileName);
    return;
  }
  if (df.isDirectory() && (strstr(fileName, "System") != NULL 
      || strstr("/", fileName) != NULL)) {
    df.close();   
    LOG_WRN("Deletion of %s not permitted", fileName);
    delay(1000); // 同一错误时减少反复重试
    return;
  }  
  LOG_INF("Deleting : %s", fileName);
  // 先清空指定文件夹
  if (df.isDirectory() || (thisFS == SPIFFSS && strstr("/", fileName) != NULL)) {
    LOG_INF("Folder %s contents", fileName);
    File file = df.openNextFile();
    while (file) {
      char filepath[FILE_NAME_LEN];
      strcpy(filepath, file.path()); 
      if (file.isDirectory()) LOG_INF("  DIR : %s", filepath);
      else {
        size_t fSize = file.size();
        file.close();
        LOG_INF("  FILE : %s Size : %s %sdeleted", filepath, fmtSize(fSize), STORAGE.remove(filepath) ? "" : "not ");
        deleteOthers(filepath);
      }
      file = df.openNextFile();
    }
    // 删除文件夹
    if (df.isDirectory()) LOG_ALT("Folder %s %sdeleted", fileName, STORAGE.rmdir(fileName) ? "" : "not ");
    else df.close();
  } else {
    // 删除单个文件
    df.close();
    LOG_ALT("File %s %sdeleted", deleteThis, STORAGE.remove(deleteThis) ? "" : "not ");  // 删除文件
    deleteOthers(deleteThis);
  }
}

/************** 未压缩 tar 归档 **************/

#define BLOCKSIZE 512

static esp_err_t writeHeader(File& inFile, httpd_req_t* req) {  
  char tarHeader[BLOCKSIZE] = {0}; // 512 字节 tar 文件头
  strncpy(tarHeader, inFile.name(), 99); // 文件名
  sprintf(tarHeader + 100, "0000666"); // 文件权限，ASCII 八进制
  sprintf(tarHeader + 124, "%011o", inFile.size()); // 文件长度（字节），6 位 ASCII 八进制
  memcpy(tarHeader + 148, "        ", 8); // 初始化为 8 个空格以计算校验和
  tarHeader[156] = '0'; // 条目类型 - 0 表示普通文件
  strcpy(tarHeader + 257, "ustar"); // 魔数
  memcpy(tarHeader + 263, "00", 2); // 版本，两个 0 数字

  // 计算并设置校验和
  uint32_t checksum = 0;
  for (const auto& ch : tarHeader) checksum += ch;
  sprintf(tarHeader + 148, "%06lo", checksum); // 六位八进制数，前导零，后跟空字符和空格。
  return httpd_resp_send_chunk(req, tarHeader, BLOCKSIZE);
}

esp_err_t downloadFile(File& df, httpd_req_t* req) {
  // 以附件形式下载文件，所需文件名在 inFileName 中
  // 设置下载响应头，按需创建 zip 文件并下载
  esp_err_t res = ESP_OK;
  bool needZip = false;
  char downloadName[FILE_NAME_LEN];
  strcpy(downloadName, df.name());
  size_t downloadSize = df.size();
  char fsSavePath[FILE_NAME_LEN];
  strcpy(fsSavePath, inFileName);
#ifdef ISCAM
  changeExtension(fsSavePath, CSV_EXT);
  
  // 检查是否存在附属文件
  needZip = STORAGE.exists(fsSavePath);
  const char* extensions[3] = {AVI_EXT, CSV_EXT, SRT_EXT};
  if (needZip) {
    // 有附属文件，计算 HTTP 响应头的总大小
    downloadSize = 0;
    for (const auto& ext : extensions) {
      changeExtension(fsSavePath, ext);
      File inFile = STORAGE.open(fsSavePath, FILE_READ);
      if (inFile) {
        // 将文件大小向上取整到 512 字节边界并加上文件头大小
        downloadSize += (((inFile.size() + BLOCKSIZE - 1) / BLOCKSIZE) * BLOCKSIZE) + BLOCKSIZE;
        strcpy(downloadName, inFile.name());
        inFile.close();
      }
    }
    downloadSize += BLOCKSIZE * 2; // TAR 归档结束标记
    changeExtension(downloadName, "zip"); 
  } 
#endif 

  // 创建 HTTP 响应头
  LOG_INF("Download file: %s, size: %s", downloadName, fmtSize(downloadSize));
  httpd_resp_set_type(req, "application/octet-stream");
  // 响应头字段值在首次发送前必须保持有效
  char contentDisp[IN_FILE_NAME_LEN + 50];
  snprintf(contentDisp, sizeof(contentDisp) - 1, "attachment; filename=%s", downloadName);
  httpd_resp_set_hdr(req, "Content-Disposition", contentDisp);
  char contentLength[10];
  snprintf(contentLength, sizeof(contentLength) - 1, "%i", downloadSize);
  httpd_resp_set_hdr(req, "Content-Length", contentLength);

  if (needZip) {
#ifdef ISCAM
    // 将 AVI 文件和附属文件打包为未压缩 TAR 归档
    for (const auto& ext : extensions) {
      changeExtension(fsSavePath, ext);
      File inFile = STORAGE.open(fsSavePath, FILE_READ);
      if (inFile) {
        res = writeHeader(inFile, req);
        if (res == ESP_OK) res = sendChunks(inFile, req, false);
        if (res == ESP_OK) {
          // 写入文件结束填充
          size_t remainingBytes = inFile.size() % BLOCKSIZE;
          if (remainingBytes) {
            char zeroBlock[BLOCKSIZE - remainingBytes] = {};
            res = httpd_resp_send_chunk(req, zeroBlock, sizeof(zeroBlock));
          }
          inFile.close();
        }
      }
    }

    // 写入两个全零块标记归档结束
    char zeroBlock[BLOCKSIZE] = {};
    res = httpd_resp_send_chunk(req, zeroBlock, BLOCKSIZE);
    res = httpd_resp_send_chunk(req, zeroBlock, BLOCKSIZE);
    res = httpd_resp_sendstr_chunk(req, NULL);
#endif
  } else res = sendChunks(df, req); // 发送 AVI
  return res;
}

bool formatSDcard() {
  // 格式化 SD 卡，擦除现有内容
  // 可能需要一些时间完成
  // 通过 URL 调用：<cam ip>/control?formatSD=1
  LOG_INF("Format the SD card, wait ...");
  char drv[3] = {'0', ':', 0};
  const size_t workbuf_size = 4096;
  void* workbuf = NULL;
  size_t allocation_unit_size = 4 * 1024;
  int sector_size_default = 512;

  workbuf = ff_memalloc(workbuf_size);
  if (workbuf == NULL) {
    LOG_ERR("workbuf memory not allocated");
    return false;
  }

  size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(
      sector_size_default, allocation_unit_size);
  const MKFS_PARM opt = {(BYTE)FM_ANY, 0, 0, 0, alloc_unit_size};
  FRESULT res = f_mkfs(drv, &opt, workbuf, workbuf_size);
  ff_memfree(workbuf);
  if (res != FR_OK) LOG_ERR("SD card format failed");
  else LOG_INF("SD card formatted with alloc unit size %d", alloc_unit_size);
  return res != FR_OK ? false : true;
}
