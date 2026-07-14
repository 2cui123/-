
/*
  使用 WebDAV 服务器：
    Windows 10：
    - 文件资源管理器地址栏输入：<ip_address>/webdav
    - 映射网络驱动器，连接到：\\<ip_address>\webdav
    Windows 11：
    - 映射网络驱动器：
      - 连接到：\\<ip_address>\webdav
      - 点击链接「连接到一个可用于存储文档和图片的网站」
      - 点击「下一步」，然后「选择自定义网络位置」
      - 重新输入 \\<ip_address>\webdav

    Android：
    - Solid Explorer，远程主机名输入 <ip_address>，路径输入 webdav

  未测试：
    MacOS：
    - Finder：command-K > http://<ip_address>/webdav（勿选「匿名」以保留写权限）
    - 命令行：mkdir -p /tmp/esp; mount_webdav -S -i -v esp32 <ip_address>/webdav /tmp/esp && echo OK
 
    linux：
    - mount -t davs2 http://<ip_address>/webdav /mnt/
    - gio/gvfs/nautilus/你的文件管理器 http://<ip_address>/webdav

  参考思路来自 https://github.com/d-a-v/ESPWebDAV
   
  s60sc 2024
*/

#include "appGlobals.h"

#if INCLUDE_WEBDAV
#define ALLOW "PROPPATCH,PROPFIND,OPTIONS,DELETE,MOVE,COPY,HEAD,POST,PUT,GET"
#define XML1 "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:multistatus xmlns:D=\"DAV:\">"
#define XML2 "<D:response xmlns:D=\"DAV:\"><D:href>"
#define XML3 "</D:href><D:propstat><D:status>HTTP/1.1 200 OK</D:status><D:prop>"
#define XML4 "</D:prop></D:propstat></D:response>"
#define XML5 "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock><D:locktoken><D:href>"
#define XML6 "</D:href></D:locktoken></D:activelock></D:lockdiscovery></D:prop>"

static char pathName[IN_FILE_NAME_LEN];
static httpd_req_t* req;
static char formattedTime[80];
static const char* extensions[] = {"dummy", ".htm", ".css", ".txt", ".js", ".json", ".png", ".gif", ".jpg", ".ico", ".svg", ".xml", ".pdf", ".zip", ".gz"};
static const char* mimeTypes[] = {"application/octet-stream", "text/html", "text/html", "text/css", "text/plain", "application/javascript", "application/json", "image/png", "image/gif", "image/jpeg", "image/x-icon", "image/svg+xml", "text/xml", "application/pdf", "application/zip", "application/x-gzip"};

static int getMimeType(const char* path) {
  // 根据文件扩展名确定 MIME 类型
  int mimePtr = 1;
  size_t len = strlen(path);
  for (const char* ext : extensions) {
    size_t slen = strlen(ext);
    if (!strncmp(path + len - slen, ext, slen)) return mimePtr;
    mimePtr++;
  }
  return 0; // 默认 MIME 类型
}
  
static void formatTime(time_t t) {
  // 格式化 XML 属性值的时间
  tm* timeinfo = gmtime(&t);
  strftime(formattedTime, sizeof(formattedTime), "%a, %d %b %Y %H:%M:%S GMT", timeinfo);
}

static bool haveResource(bool ignore = false) {
  // 检查文件或文件夹是否存在
  if (STORAGE.exists(pathName)) return true;
  else if (!ignore) httpd_resp_send_404(req); 
  return false;
} 

static bool isFolder() {
  // 判断资源是文件还是文件夹
  File root = STORAGE.open(pathName);
  bool res = root.isDirectory();
  root.close();
  return res;
}

static void sendContentProp(const char* prop, const char* value) {
  // 在响应中设置单个 XML 属性
  char propStr[strlen(prop) * 2 + strlen(value) + 15];
  sprintf(propStr, "<D:%s>%s</D:%s>", prop, value, prop);
  httpd_resp_sendstr_chunk(req, propStr);
  LOG_VRB("propStr %s", propStr);
}

static void sendPropResponse(File& file, const char* payload) {
  // 向 PC 发送 SD 卡属性详情
  size_t encodeLen = 3 + strlen(file.path()) * 2;
  size_t maxLen = strlen(XML2) + encodeLen + strlen(XML3);
  char resp[maxLen + 1];
  snprintf(resp, maxLen, "%s%s%s", XML2, file.path(), XML3);
  httpd_resp_sendstr_chunk(req, resp);
  LOG_VRB("resp xml: %s", resp);
  
  formatTime(file.getLastWrite());
  sendContentProp("getlastmodified", formattedTime);
  sendContentProp("creationdate", formattedTime);

  if (file.isDirectory()) sendContentProp("resourcetype", "<D:collection/>");
  else {
    char fsizeStr[15];
    sprintf(fsizeStr, "%u", file.size());
    sendContentProp("getcontentlength", fsizeStr);
    sendContentProp("getcontenttype", mimeTypes[getMimeType(file.path())]);
    httpd_resp_sendstr_chunk(req, "<resourcetype/>");
  }
  sendContentProp("displayname", file.name());
  
  if (strlen(payload)) {
    // 若请求则返回配额数据
    if (strstr(payload, "quota-available-bytes") != NULL || strstr(payload, "quota-used-bytes") != NULL) {
      char numberStr[15];
      sprintf(numberStr, "%llu", (uint64_t)STORAGE.totalBytes() - (uint64_t)STORAGE.usedBytes());
      sendContentProp("quota-available-bytes", numberStr);
      sprintf(numberStr, "%llu", (uint64_t)STORAGE.usedBytes());
      sendContentProp("quota-used-bytes", numberStr);
    }
  }
  httpd_resp_sendstr_chunk(req, XML4);
}

static bool getPayload(char* payload) {
  // 获取 PROPFIND 消息中的请求体
  int bytesRead = -1;
  size_t offset = 0;
  size_t psize = req->content_len;
  if (psize) {
    do {
      bytesRead = httpd_req_recv(req, payload + offset, psize - offset);
      if (bytesRead < 0) {  
        if (bytesRead == HTTPD_SOCK_ERR_TIMEOUT) {
          delay(10);
          continue;
        } else {
          LOG_WRN("Transfer request failed with status %i", bytesRead);
          psize = 0;
          break;
        }
      } else offset += bytesRead;
    } while (bytesRead > 0);
    payload[psize] = 0;  
    LOG_VRB("payload: %s\n", payload);
  }
  return bytesRead < 0 ? false : true;
}

static bool handleProp() {
  // 向 PC 提供 SD 卡内容详情
  if (!haveResource()) return false;
  // 获取 Depth 头
  bool depth = false;
  char value[10];
  if (extractHeaderVal(req, "Depth", value) == ESP_OK) depth = (!strcmp(value, "0")) ? false : true;

  // 若有请求体则获取其内容
  char payload[req->content_len + 1] = {0};
  if (req->content_len) getPayload(payload); 
  
  // 通用响应头
  httpd_resp_set_status(req, "207 Multi-Status");
  httpd_resp_set_type(req, "application/xml;charset=utf-8");
  httpd_resp_sendstr_chunk(req, XML1);
  
  // 返回所选文件夹的详情
  File root = STORAGE.open(pathName);
  sendPropResponse(root, payload);
  if (depth && root.isDirectory()) {
    // 若请求则返回文件夹中每个资源的详情
    File entry = root.openNextFile();
    while (entry) {
      sendPropResponse(entry, "");
      entry.close();
      entry = root.openNextFile();
    }
  }
  root.close();
  httpd_resp_sendstr_chunk(req, "</D:multistatus>");
  httpd_resp_sendstr_chunk(req, NULL);
  return true;
}

static bool handleOptions() {
  httpd_resp_sendstr(req, NULL); 
  return true;
}

static bool handleGet() {
  // 向 PC 传输文件
  if (!haveResource()) return false;
  if (isFolder()) {
    httpd_resp_send_404(req);
    return false;
  } else {
    httpd_resp_set_type(req, mimeTypes[getMimeType(pathName)]);
    strcpy(inFileName, pathName);
    esp_err_t res = fileHandler(req); // 文件内容
    return res == ESP_OK ? true : false;
  }
  return true;
}

static bool handleHead() {
  if (!haveResource()) return false;
  httpd_resp_sendstr(req, NULL);
  return true;
}

static bool handleLock() {
  // 文件打开时提供（虚拟）锁
  const char* lockToken = "0123456789012345";
  httpd_resp_set_hdr(req, "Lock-Token", lockToken);
  char resp[strlen(XML5) + strlen(lockToken) + strlen(XML6) + 1];
  sprintf(resp, "%s%s%s", XML5, lockToken, XML6);
  httpd_resp_set_type(req, "application/xml;charset=utf-8");
  httpd_resp_sendstr(req, resp);
  return true;
} 

static bool handleUnlock() {
  // 文件关闭时解锁
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_sendstr(req, NULL);
  return true;
}

static bool handlePut() {
  // 从 PC 传输文件
  if (isFolder()) return false;
  if (!haveResource(true) || !req->content_len) {
    // 若无内容，仅创建文件条目
    File file = STORAGE.open(pathName, FILE_WRITE);
    file.close();
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_sendstr(req, NULL);
  }
  if (req->content_len) {
    // 将文件内容传输至 SD 卡
    strcpy(inFileName, pathName);
    esp_err_t res = uploadHandler(req);
    return res == ESP_OK ? true : false;
  } 
  return true;
}

static bool handleDelete() {
  // 删除文件或文件夹
  if (!haveResource()) return false;
  // 本应用仅支持单级文件夹
  deleteFolderOrFile(pathName);
  httpd_resp_sendstr(req, NULL);
  return true;
}

static bool handleMkdir() {
  // 创建新文件夹
  if (haveResource(true)) return false; // 已存在
  bool res = STORAGE.mkdir(pathName);
  if (res) httpd_resp_set_status(req, "201 Created");
  else httpd_resp_set_status(req, "500 Internal Server Error");
  httpd_resp_sendstr(req, NULL);
  return res; 
}

static bool checkSamePath(const char *source_path, const char *dest_path) {
  // 比较路径（不含文件名）
  char source_dir[strlen(source_path) + 1];
  char dest_dir[strlen(dest_path) + 1];
  strncpy(source_dir, source_path, strrchr(source_path, '/') - source_path);
  source_dir[strrchr(source_path, '/') - source_path] = 0;
  strncpy(dest_dir, dest_path, strrchr(dest_path, '/') - dest_path);
  dest_dir[strrchr(dest_path, '/') - dest_path] = 0;
  return strcmp(source_dir, dest_dir) == 0;
}

static bool handleMove() {
  // 重命名文件或文件夹，或更改文件位置
  bool res = false;
  char dest[100];
  if (extractHeaderVal(req, "Destination", dest) == ESP_OK) {
    // 获取目标文件名
    res = true;
    urlDecode(dest);
    char* pos = strstr(dest, WEBDAV);
    memmove(dest, pos + strlen(WEBDAV), strlen(dest));
  
    // 仅允许在同一文件夹内重命名
    if (isFolder()) res = checkSamePath(pathName, dest);
    if (res) {
      res = STORAGE.rename(pathName, dest);
      if (res) httpd_resp_set_status(req, "201 Created");
      else httpd_resp_set_status(req, "500 Internal Server Error");
      httpd_resp_sendstr(req, NULL);
      return true;
    } 
  } 
  httpd_resp_send_404(req);
  return false;
}

static bool handleCopy() {
  // 复制文件夹 - 未实现
  // 文件可通过复制/粘贴操作完成
  httpd_resp_send_404(req);
  return false;
}

bool handleWebDav(httpd_req_t* rreq) {
  // 解析 HTTP 方法以确定 WebDAV 操作
  //showHttpHeaders(rreq);
  req = rreq;
  sprintf(pathName, "%s", req->uri + strlen(WEBDAV)); // 去掉 "/webdav" 前缀
  if (pathName[strlen(pathName) - 1] == '/') pathName[strlen(pathName) - 1] = 0; // 若末尾有 / 则移除
  if (!strlen(pathName)) strcpy(pathName, "/"); // 路径为空时使用 "/"
  urlDecode(pathName);
  // 通用响应头
  httpd_resp_set_hdr(req, "DAV", "1");
  httpd_resp_set_hdr(req, "Allow", ALLOW);

  switch(req->method) {
    case HTTP_PUT: return handlePut(); // 创建/上传文件
    case HTTP_PROPFIND: return handleProp(); // 获取文件或目录属性
    case HTTP_PROPPATCH: return handleProp(); // 设置文件或目录属性
    case HTTP_GET: return handleGet(); // 下载文件
    case HTTP_HEAD: return handleHead(); // 获取文件属性
    case HTTP_OPTIONS: return handleOptions(); // 支持的选项
    case HTTP_LOCK: return handleLock(); // 打开文件锁
    case HTTP_UNLOCK: return handleUnlock(); // 关闭文件锁
    case HTTP_MKCOL: return handleMkdir(); // 创建文件夹
    case HTTP_MOVE: return handleMove(); // 重命名或移动文件/目录
    case HTTP_DELETE: return handleDelete(); // 删除文件或目录
    case HTTP_COPY: return handleCopy(); // 复制文件或目录
    default: {
      LOG_ERR("Unhandled method %s", HTTP_METHOD_STRING(req->method));
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Unhandled method");
      return false;
    }
  }
  return true;
}

#endif

