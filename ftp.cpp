// 使用 FTP 或 HTTPS 将 SD 卡或 SPIFFS 内容上传到远程服务器
// 
// s60sc 2022, 2023

#include "appGlobals.h"

#if INCLUDE_FTP_HFS
#if (!INCLUDE_CERTS)
const char* hfs_rootCACertificate = "";
const char* ftps_rootCACertificate = "";
#endif

// 文件服务器参数（FTP 或 HTTPS），通过网页配置
char fsServer[MAX_HOST_LEN];
uint16_t fsPort = 21;
char FS_Pass[MAX_PWD_LEN]; // FTP 密码或 HTTPS 通行码
char fsWd[FILE_NAME_LEN]; 

static bool uploadInProgress = false;
uint8_t percentLoaded = 0;
TaskHandle_t fsHandle = NULL;
static char storedPathName[FILE_NAME_LEN];
static char folderPath[FILE_NAME_LEN];
static byte* fsChunk;
bool deleteAfter = false; // 上传后自动删除
bool autoUpload = false;  // 自动将每个新建文件上传到远程文件服务器
bool fsUse = false; // false 为 FTP，true 为 HTTPS


/******************** HTTPS 协议 ********************/

// 将本地存储中的单个文件或文件夹上传到远程 HTTPS 文件服务器
// 由于 TLS，需要较多堆内存。
// 每个文件的 POST 格式如下，以下值来自网页配置：
//   Host: 文件服务器
//   port: 文件服务器端口
//   passcode: 文件服务器密码
//   pathname: FS 根目录 + 所选日期文件夹/文件
/*
POST /upload HTTP/1.1
Host: 192.168.1.135
Content-Length: 2412358
Content-Type: multipart/form-data; boundary=123456789000000000000987654321

--123456789000000000000987654321
Content-disposition: form-data; name="json"
Content-Type: "application/json"

{"pathname":"/FS/root/dir/20231119/20231119_140513_SVGA_20_6_120.avi","passcode":"abcd1234"}
--123456789000000000000987654321
Content-disposition: form-data; name="file"; filename="20231119_140513_SVGA_20_6_120.avi"
Content-Type: "application/octet-stream"

<文件内容>
--123456789000000000000987654321
*/

#define CONTENT_TYPE "Content-Type: \"%s\"\r\n\r\n"
#define POST_HDR "POST /%s HTTP/1.1\r\nHost: %s\r\nContent-Length: %u\r\n" CONTENT_TYPE
#define MULTI_TYPE "multipart/form-data; boundary=" BOUNDARY_VAL
#define JSON_TYPE "application/json"
#define BIN_TYPE "application/octet-stream"
#define FORM_DATA "--" BOUNDARY_VAL "\r\nContent-disposition: form-data; name=\"%s%s\"\r\n" CONTENT_TYPE 
#define END_BOUNDARY "\r\n--" BOUNDARY_VAL "--\r\n"
#define FILE_NAME "file\"; filename=\""
#define JSON_DATA "{\"pathname\":\"%s%s/%s\",\"passcode\":\"%s\"}"
#define FORM_OFFSET 256 // fsBuff 中用于准备表单数据的偏移量

NetworkClientSecure hclient;
char* fsBuff;

static void postHeader(const char* tmethod, const char* contentType, bool isFile, 
  size_t fileSize, const char* fileName) {
  // 创建 HTTP POST 头
  char* p = fsBuff + FORM_OFFSET; // 为 HTTP 请求数据预留空间
  if (isFile) {
    p += sprintf(p, FORM_DATA, "json", "", JSON_TYPE);
    // fsBuff 初始包含文件夹名称
    p += sprintf(p, JSON_DATA, fsWd, folderPath, fileName, FS_Pass);
    p += sprintf(p, "\r\n" FORM_DATA, FILE_NAME, fileName, BIN_TYPE); 
  } // 否则 JSON 数据已由 hfsCreateFolder() 加载
  size_t formLen = strlen(fsBuff + FORM_OFFSET);
  // 创建 HTTP 请求头
  p = fsBuff;
  if (isFile) fileSize += formLen + strlen(END_BOUNDARY);
  p += sprintf(p, POST_HDR, tmethod, fsServer, fileSize, isFile ? MULTI_TYPE : JSON_TYPE);
  size_t reqLen = strlen(fsBuff);
  // 拼接请求与表单数据
  if (formLen) {
    memmove(fsChunk + reqLen, fsChunk + FORM_OFFSET, formLen);
    fsChunk[reqLen + formLen] = 0;
  }
  hclient.print(fsBuff); // HTTP 头
}

static bool hfsStoreFile(File &fh) {
  // 将单个文件上传到 HTTPS 服务器
  // 若为文件夹或无效文件类型则拒绝
#ifdef ISCAM
  if (!strstr(fh.name(), AVI_EXT) && !strstr(fh.name(), CSV_EXT) && !strstr(fh.name(), SRT_EXT)) return false; 
#else
  if (!strstr(fh.name(), FILE_EXT)) return false; 
#endif
  LOG_INF("Upload file: %s, size: %s", fh.name(), fmtSize(fh.size()));    

  // 准备 POST 头并将文件发送到 HTTPS 服务器
  postHeader("upload", BIN_TYPE, true, fh.size(), fh.name());
  // 分块上传文件内容
  uint8_t percentLoaded = 0;
  size_t chunksize = 0, totalSent = 0;
  while ((chunksize = fh.read((uint8_t*)fsChunk, CHUNKSIZE))) {
    hclient.write((uint8_t*)fsChunk, chunksize);
    totalSent += chunksize;
    if (calcProgress(totalSent, fh.size(), 5, percentLoaded)) LOG_INF("Uploaded %u%%", percentLoaded); 
  }
  percentLoaded = 100;
  hclient.println(END_BOUNDARY);
  return true;
}

/******************** FTP ********************/

// FTP 控制
bool useFtps = false;
char ftpUser[MAX_HOST_LEN];
static char rspBuf[256]; // FTP 响应缓冲区
static char respCodeRx[4]; // FTP 响应码
#define NO_CHECK "999"

// WiFi 客户端
NetworkClient rclient;
NetworkClient dclient;

static bool sendFtpCommand(const char* cmd, const char* param, const char* respCode, const char* respCode2 = NO_CHECK) {
  // 构建并发送 FTP 命令
  if (strlen(cmd)) {
    rclient.print(cmd);
    rclient.println(param);
  }
  LOG_VRB("Sent cmd: %s%s", cmd, param);
  
  // 等待 FTP 服务器响应
  uint32_t start = millis();
  while (!rclient.available() && millis() < start + (responseTimeoutSecs * 1000)) delay(1);
  if (!rclient.available()) {
    LOG_WRN("FTP server response timeout");
    return false;
  }
  // 读取响应码和消息
  rclient.read((uint8_t*)respCodeRx, 3); 
  respCodeRx[3] = 0; // 终止符
  int readLen = rclient.read((uint8_t*)rspBuf, 255);
  rspBuf[readLen] = 0;
  while (rclient.available()) rclient.read(); // 丢弃剩余响应

  // 检查响应码是否符合预期
  LOG_VRB("Rx code: %s, resp: %s", respCodeRx, rspBuf);
  if (strcmp(respCode, NO_CHECK) == 0) return true; // 不检查响应码
  if (strcmp(respCodeRx, respCode) != 0) {
    if (strcmp(respCodeRx, respCode2) != 0) {
      // 响应码不正确
      LOG_ERR("Command %s got wrong response: %s %s", cmd, respCodeRx, rspBuf);
      return false;
    }
  }
  return true;
}

static bool ftpConnect() {
  // 连接 FTP 或 FTPS
  if (rclient.connect(fsServer, fsPort)) {LOG_VRB("FTP connected at %s:%u", fsServer, fsPort);}
  else {
    LOG_WRN("Error opening ftp connection to %s:%u", fsServer, fsPort);
    return false;
  }
  if (!sendFtpCommand("", "", "220")) return false;
  if (useFtps) {
    if (sendFtpCommand("AUTH ", "TLS", "234")) {
      /* 未实现 */
    } else LOG_WRN("FTPS not available");
  }
  if (!sendFtpCommand("USER ", ftpUser, "331")) return false;
  if (!sendFtpCommand("PASS ", FS_Pass, "230")) return false;
  // 切换到指定文件夹
  if (!sendFtpCommand("CWD ", fsWd, "250")) return false;
  if (!sendFtpCommand("Type I", "", "200")) return false;
  return true;
}

static void ftpDisconnect() {
  // 断开 FTP 服务器连接
  rclient.println("QUIT");
  dclient.stop();
  rclient.stop();
}

static bool ftpCreateFolder(const char* folderName) {
  // 若文件夹不存在则创建，然后进入该文件夹
  LOG_VRB("Check for folder %s", folderName);
  sendFtpCommand("CWD ", folderName, NO_CHECK); 
  if (strcmp(respCodeRx, "550") == 0) {
    // 文件夹不存在，创建它
    if (!sendFtpCommand("MKD ", folderName, "257")) return false;
    //sendFtpCommand("SITE CHMOD 755 ", folderName, "200", "550"); // 仅 Unix
    if (!sendFtpCommand("CWD ", folderName, "250")) return false;         
  }
  return true;
}

static bool openDataPort() {
  // 设置数据传输端口
  if (!sendFtpCommand("PASV", "", "227")) return false;
  // 解析数据端口号
  char* p = strchr(rspBuf, '('); // 跳过开头文本
  int p1, p2;   
  int items = sscanf(p, "(%*d,%*d,%*d,%*d,%d,%d)", &p1, &p2);
  if (items != 2) {
    LOG_ERR("Failed to parse data port");
    return false;
  }
  int dataPort = (p1 << 8) + p2;
  
  // 连接数据端口
  LOG_VRB("Data port: %i", dataPort);
  if (!dclient.connect(fsServer, dataPort)) {
    LOG_WRN("Data connection failed");   
    return false;
  }
  return true;
}

static bool ftpStoreFile(File &fh) {
  // 将单个文件上传到当前文件夹，覆盖已有文件
  // 若为文件夹或无效文件类型则拒绝
#ifdef ISCAM
  if (!strstr(fh.name(), AVI_EXT) && !strstr(fh.name(), CSV_EXT) && !strstr(fh.name(), SRT_EXT)) return false; 
#else
  if (!strstr(fh.name(), FILE_EXT)) return false; 
#endif
  char ftpSaveName[FILE_NAME_LEN];
  strcpy(ftpSaveName, fh.name());
  size_t fileSize = fh.size();
  LOG_INF("Upload file: %s, size: %s", ftpSaveName, fmtSize(fileSize));    

  // 打开数据连接
  openDataPort();
  uint32_t writeBytes = 0; 
  uint32_t uploadStart = millis();
  size_t readLen, writeLen;
  if (!sendFtpCommand("STOR ", ftpSaveName, "150", "125")) return false;
  do {
    // 分块上传文件
    readLen = fh.read(fsChunk, CHUNKSIZE);  
    if (readLen) {
      writeLen = dclient.write((const uint8_t*)fsChunk, readLen);
      writeBytes += writeLen;
      if (writeLen == 0) {
        LOG_WRN("Upload file to ftp failed");
        return false;
      }
      if (calcProgress(writeBytes, fileSize, 5, percentLoaded)) LOG_INF("Uploaded %u%%", percentLoaded); 
    }
  } while (readLen > 0);
  dclient.stop();
  percentLoaded = 100;
  bool res = sendFtpCommand("", "", "226");
  if (res) {
    LOG_ALT("Uploaded %s in %u sec", fmtSize(writeBytes), (millis() - uploadStart) / 1000);
    //sendFtpCommand("SITE CHMOD 644 ", ftpSaveName, "200", "550"); // 仅 Unix
  } else LOG_WRN("File transfer not successful");
  return res;
}


/******************** 通用 ********************/

static bool getFolderName(const char* folderName) {
  // 从路径名提取文件夹名称
  strcpy(folderPath, folderName); 
  int pos = 1; // 跳过第一个 '/'
  // 依次获取每个文件夹名称
  bool res = true;
  for (char* p = strchr(folderPath, '/'); (p = strchr(++p, '/')) != NULL; pos = p + 1 - folderPath) {
    *p = 0; // 终止符
    if (!fsUse) res = ftpCreateFolder(folderPath + pos);
  }
  return res;
}

static bool uploadFolderOrFileFs(const char* fileOrFolder) {
  // 使用 FTP 或 HTTPS 服务器上传单个文件或整个文件夹
  // 文件夹逐文件上传
  fsBuff = (char*)fsChunk;
  bool res = fsUse ? remoteServerConnect(hclient, fsServer, fsPort, hfs_rootCACertificate, FSFTP) : ftpConnect();

  if (!res) {
    LOG_WRN("Unable to connect to %s server", fsUse ? "HTTPS" : "FTP");
    return false;
  }
  res = false;
  const int saveRefreshVal = refreshVal;
  refreshVal = 1;
  File root = STORAGE.open(fileOrFolder);
  if (!root.isDirectory()) {
    // 上传单个文件
    char fsSaveName[FILE_NAME_LEN];
    strcpy(fsSaveName, root.path());
    if (getFolderName(root.path())) res = fsUse ? hfsStoreFile(root) : ftpStoreFile(root); 
#ifdef ISCAM
    // 若存在则上传对应的 csv 和 srt 文件
    if (res) {
      changeExtension(fsSaveName, CSV_EXT);
      if (STORAGE.exists(fsSaveName)) {
        File csv = STORAGE.open(fsSaveName);
        res = fsUse ? hfsStoreFile(csv) : ftpStoreFile(csv);
        csv.close();
      }
      changeExtension(fsSaveName, SRT_EXT);
      if (STORAGE.exists(fsSaveName)) {
        File srt = STORAGE.open(fsSaveName);
        res = fsUse ? hfsStoreFile(srt) : ftpStoreFile(srt);
        srt.close();
      }
    }
    if (!res) LOG_WRN("Failed to upload: %s", fsSaveName);
#endif
  } else {  
    // 上传整个文件夹，逐文件上传
    LOG_INF("Uploading folder: ", root.name()); 
    strncpy(folderPath, root.name(), FILE_NAME_LEN - 1);
    res = fsUse ? true : ftpCreateFolder(root.name());
    if (!res) {
      refreshVal = saveRefreshVal;
      return false;
    }
    File fh = root.openNextFile();
    while (fh) {
      res = fsUse ? hfsStoreFile(fh) : ftpStoreFile(fh);
      if (!res) break; // 放弃剩余文件
      fh.close();
      fh = root.openNextFile();
    }
    if (fh) fh.close();
  }
  refreshVal = saveRefreshVal;
  root.close();
  fsUse ? remoteServerClose(hclient) : ftpDisconnect(); 
  return res;
}

static void fileServerTask(void* parameter) {
  // 处理 FTP 或 HTTPS 请求
#ifdef ISCAM
  doPlayback = false; // 关闭当前回放
#endif
  fsChunk = psramFound() ? (byte*)ps_malloc(CHUNKSIZE) : (byte*)malloc(CHUNKSIZE); 
  if (strlen(storedPathName) >= 2) {
    File root = STORAGE.open(storedPathName);
    if (!root) LOG_WRN("Failed to open: %s", storedPathName);
    else { 
      bool res = uploadFolderOrFileFs(storedPathName);
      if (res && deleteAfter) deleteFolderOrFile(storedPathName);
    }
  } else LOG_VRB("Root or null is not allowed %s", storedPathName);  
  uploadInProgress = false;
  free(fsChunk);
  fsHandle = NULL;
  vTaskDelete(NULL);
}

bool fsStartTransfer(const char* fileFolder) {
  // 由其他函数调用，开始将文件或文件夹传输到文件服务器
  setFolderName(fileFolder, storedPathName);
  if (!uploadInProgress) {
    uploadInProgress = true;
    if (fsHandle == NULL) xTaskCreateWithCaps(&fileServerTask, "fileServerTask", FS_STACK_SIZE, NULL, FTP_PRI, &fsHandle, HEAP_MEM);    
    debugMemory("fsStartTransfer");
    return true;
  } else LOG_WRN("Unable to transfer %s as another transfer in progress", storedPathName);
  return false;
}

void prepUpload() {
  LOG_INF("File uploads will use %s server", fsUse ? "HTTPS" : "FTP");
}
#endif
