
#include "appGlobals.h"

#if INCLUDE_EXTHB

// 外部心跳
char external_heartbeat_domain[MAX_HOST_LEN] = "";  // 外部心跳域名/IP
char external_heartbeat_uri[64] = "";     // 外部心跳 URI（例如 /myesp32-cam-hub/index.php）
int external_heartbeat_port;              // 外部心跳服务器连接端口
char external_heartbeat_token[EXTHB_LEN] = "";   // 外部心跳服务器认证令牌  

bool external_heartbeat_active = false;

void sendExternalHeartbeat() {
  
  // external_heartbeat_active~0~2~C~启用外部心跳服务器
  // external_heartbeat_domain~~2~T~心跳接收端域名或 IP（例如 www.mydomain.com）
  // external_heartbeat_uri~~2~T~心跳接收端 URI（例如 /my-esp32cam-hub/index.php）
  // external_heartbeat_port~443~2~N~心跳接收端端口
  // external_heartbeat_token~~2~T~心跳接收端认证令牌
  
  // 向外部心跳地址发送 POST 请求
  char uri[64 + 10 + EXTHB_LEN] = "";
  strcpy(uri, external_heartbeat_uri);
  strcat(uri, "?token=");
  strcat(uri, external_heartbeat_token);
  
  NetworkClientSecure hclient;
  
  buildJsonString(false);

  //hclient.setInsecure();
  if (remoteServerConnect(hclient, external_heartbeat_domain, external_heartbeat_port, "", EXTERNALHB)) {
    HTTPClient https;
    int httpCode = HTTP_CODE_NOT_FOUND;
    if (https.begin(hclient, external_heartbeat_domain, external_heartbeat_port, uri, true)) {

      https.addHeader("Content-Type", "application/json");
      
      httpCode = https.POST(jsonBuff);
      //httpCode = https.GET();
      if (httpCode == HTTP_CODE_OK) {
        LOG_INF("External Heartbeat sent to: %s%s", external_heartbeat_domain, uri);
      } else LOG_WRN("External Heartbeat request failed, error: %s", https.errorToString(httpCode).c_str());    
      //if (httpCode != HTTP_CODE_OK) doGetExtIP = false;
      https.end();     
    }
    remoteServerClose(hclient);
  }
}

#endif
