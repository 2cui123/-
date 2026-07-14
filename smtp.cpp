// 用于发送带附件的电子邮件的简单 SMTP 客户端
//
// 仅使用 Gmail 发件账户测试
//
// Gmail 发件账户前提条件：
// - 建议创建专用邮箱账户
// - 创建应用专用密码 - https://support.google.com/accounts/answer/185833
// - 在网页配置中将 smtpUse 设为 true，并填写账户信息
//
// s60sc 2022 

#include "appGlobals.h"

#if INCLUDE_SMTP
#if (!INCLUDE_CERTS)
const char* smtp_rootCACertificate = "";
#endif

// SMTP 连接参数，通过网页配置
char smtp_login[MAX_HOST_LEN]; // 发件邮箱账户
char SMTP_Pass[MAX_PWD_LEN]; // 16 位应用专用密码，非账户登录密码
char smtp_email[MAX_HOST_LEN]; // 收件人，可与 smtp_login 相同，或任意其他邮箱
char smtp_server[MAX_HOST_LEN]; // 邮件服务提供商，例如 smtp.gmail.com
uint16_t smtp_port; // Gmail SSL 端口 465

#define MIME_TYPE "image/jpg"
#define ATTACH_NAME "frame.jpg"

// SMTP 控制
// 调用方须填充 SMTPbuffer 并设置 smtpBufferSize 作为附件数据
TaskHandle_t emailHandle = NULL; 
static char rspBuf[256]; // SMTP 响应缓冲区
static char respCodeRx[4]; // SMTP 响应码
static char subject[50];
static char message[100];

bool smtpUse = false; // 是否发送邮件告警
int emailCount = 0;
int alertMax = 10; // 仅适用于邮件

static bool sendSmtpCommand(NetworkClientSecure& client, const char* cmd, const char* respCode) {
  // 等待 SMTP 服务器响应，检查响应码并提取响应数据
  LOG_VRB("Cmd: %s", cmd);
  if (strlen(cmd)) client.println(cmd);
  
	uint32_t start = millis();
  while (!client.available() && millis() < start + (responseTimeoutSecs * 1000)) delay(1);
  if (!client.available()) {
    LOG_WRN("SMTP server response timeout");
    return false;
  }

  // 读取响应码和消息
  client.read((uint8_t*)respCodeRx, 3); 
  respCodeRx[3] = 0; // 终止符
  int readLen = client.read((uint8_t*)rspBuf, 255);
  rspBuf[readLen] = 0;
  while (client.available()) client.read(); // 丢弃剩余响应

  // 检查响应码是否符合预期
  LOG_VRB("Rx code: %s, resp: %s", respCodeRx, rspBuf);
  if (strcmp(respCodeRx, respCode) != 0) {
    // 响应码不正确
    LOG_ERR("Command %s got wrong response: %s", cmd, rspBuf);
    return false;
  }
	return true;
}

static bool emailSend(const char* mimeType = MIME_TYPE, const char* fileName = ATTACH_NAME) {
  // 向配置的 SMTP 服务器发送邮件
  char content[100];
  
  NetworkClientSecure client;
  bool res = remoteServerConnect(client, smtp_server, smtp_port, smtp_rootCACertificate, EMAILCONN); 
  if (!res) return false;
  
  while (true) { // 伪非循环，便于 break
    res = false;
    if (!sendSmtpCommand(client, "", "220")) break;
  
    sprintf(content, "HELO %s: ", APP_NAME);
    if (!sendSmtpCommand(client, content, "250")) break;
    
    if (!sendSmtpCommand(client, "AUTH LOGIN", "334")) break; 
    if (!sendSmtpCommand(client, encode64(smtp_login), "334")) break;
    if (!sendSmtpCommand(client, encode64(SMTP_Pass), "235")) break;
  
    // 发送邮件头
    sprintf(content, "MAIL FROM: <%s>", APP_NAME);
    if (!sendSmtpCommand(client, content, "250")) break;
    sprintf(content, "RCPT TO: <%s>", smtp_email);
    if (!sendSmtpCommand(client, content, "250")) break;
  
    // 发送消息体头
    if (!sendSmtpCommand(client, "DATA", "354")) break;
    sprintf(content, "From: \"%s\" <%s>", APP_NAME, smtp_login);
    client.println(content);
    sprintf(content, "To: <%s>", smtp_email);
    client.println(content);
    sprintf(content, "Subject: %s", subject);
    client.println(content);
  
    // 发送消息
    client.println("MIME-Version: 1.0");
    sprintf(content, "Content-Type: Multipart/mixed; boundary=%s", BOUNDARY_VAL);
    client.println(content);
    sprintf(content, "--%s", BOUNDARY_VAL);
    client.println(content);
    client.println("Content-Type: text/plain; charset=UTF-8");
    client.println("Content-Transfer-Encoding: quoted-printable");
    client.println("Content-Disposition: inline");
    client.println();
    client.println(message);
    client.println();
    
    if (alertBufferSize) {
      // 发送附件
      client.println(content); // 边界
      sprintf(content, "Content-Type: %s", mimeType); 
      client.println(content);
      client.println("Content-Transfer-Encoding: base64");
      sprintf(content, "Content-Disposition: attachment; filename=\"%s\"; size=%d;", fileName, alertBufferSize); 
      
      client.println(content); 
      // Base64 编码附件并分块发送
      size_t chunkSize = 3;
      for (size_t i = 0; i < alertBufferSize; i += chunkSize) 
        client.write(encode64chunk(alertBuffer + i, min(alertBufferSize - i, chunkSize)), 4);
    } 
    client.println("\n"); // 两行结束头部
        
    // 关闭消息数据并退出
    if (!sendSmtpCommand(client, ".", "250")) break;
    if (!sendSmtpCommand(client, "QUIT", "221")) break;
    res = true;
    break;
  }
  // 干净地终止连接
  remoteServerClose(client);
  alertBufferSize = 0;
  return res;
}

static void emailTask(void* parameter) {
  // 发送邮件
  if (emailCount < alertMax) { 
    // 未超过每日限额则发送
    if (emailSend()) LOG_ALT("Sent daily email %u", emailCount + 1);
    else LOG_WRN("Failed to send email");
  }
  if (++emailCount >= alertMax) LOG_WRN("Daily email limit %u reached", alertMax);
  emailHandle = NULL;
  vTaskDelete(NULL);
}

void emailAlert(const char* _subject, const char* _message) {
  // 在所需事件发生时发送告警邮件
  if (smtpUse) {
    if (alertBuffer != NULL) {
      if (emailHandle == NULL) {
        strncpy(subject, _subject, sizeof(subject)-1);
        snprintf(subject+strlen(subject), sizeof(subject)-strlen(subject), " from %s", hostName);
        strncpy(message, _message, sizeof(message)-1);
        xTaskCreateWithCaps(&emailTask, "emailTask", EMAIL_STACK_SIZE, NULL, EMAIL_PRI, &emailHandle, HEAP_MEM);
        debugMemory("emailAlert");
      } else LOG_WRN("Email alert already in progress");
    } else LOG_WRN("Need to restart to setup email");
  }
}

void prepSMTP() {
  if (smtpUse) {
    emailCount = 0;
    if (alertBuffer == NULL) alertBuffer = psramFound() ? (byte*)ps_malloc(maxAlertBuffSize) : (byte*)malloc(maxAlertBuffSize);
   LOG_INF("Email alerts active");
  } 
}

#endif
