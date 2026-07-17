 /*
 定义应用使用静态 IP 地址，并用此作为 openssl 的变量替换：
   set APP_IP="192.168.1.135"

 创建应用服务器私钥和公钥证书：
   openssl req -nodes -x509 -sha256 -newkey rsa:2048 -subj "/CN=%APP_IP%" -addext "subjectAltName=IP:%APP_IP%" -extensions v3_ca -keyout prvtkey.pem -out servercert.pem -days 3660

 查看服务器证书内容：
   openssl x509 -in servercert.pem -noout -text

 使用应用网页的 OTA Upload 选项卡将 servercert.pem 和 prvtkey.pem 复制到 ESP 存储中。

 HTTPS 的使用由网页上的选项控制：Access Settings / Authentication settings 或 Edit Config / Network settings 中的「Use HTTPS」
 若未加载私钥或公钥证书，Use HTTPS 设置将被忽略。
 
 在浏览器中输入 `https://static_ip` 访问应用。由于证书为自签名且不受信任，将显示安全警告。
 要信任该证书，需在设备上安装：
 - 打开 Chrome 设置页面。
 - 在「隐私和安全」面板中，展开「安全」部分，点击「管理证书」。
 - 在证书管理器面板中，点击「从 Windows 管理导入的证书」
 - 在证书弹出窗口中，选择「受信任的根证书颁发机构」选项卡，点击「导入...」按钮启动导入向导。
 - 点击「下一步」，在下一页选择「浏览... 所有文件」并找到 servercert.pem 文件。
 - 点击「下一步」，然后「完成」，在安全警告弹出窗口中点击「是」，另一弹出窗口将提示导入成功。
 */

#include "appGlobals.h"

#if INCLUDE_CERTS

#ifndef CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
#define CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC 1
#endif

#define PRVTKEY DATA_DIR "/prvtkey" ".pem"
#define SERVERCERT DATA_DIR "/servercert" ".pem"

char* serverCerts[2]; // 私钥、服务器证书
#define NUM_CERTS 2

void loadCerts() {
  if (useHttps) {
    const char* certFiles[NUM_CERTS] = {PRVTKEY, SERVERCERT};
    for (int i = 0; i < NUM_CERTS; i++) {
      File file;
      if (STORAGE.exists(certFiles[i])) {
        file = STORAGE.open(certFiles[i], FILE_READ);
        if (!file || !file.size()) {
          LOG_WRN("Failed to load file %s", certFiles[i]);
          useHttps = false;
        } else {
          // 加载内容
          serverCerts[i] = psramFound() ? (char*)ps_malloc(file.size() + 1) : (char*)malloc(file.size() + 1); 
          size_t inBytes = file.readBytes(serverCerts[i], file.size());
          if (inBytes != file.size()) {
            LOG_WRN("File %s not correctly loaded", certFiles[i]);
            useHttps = false;
          }
        }
        file.close();
      } else {
        LOG_WRN("File %s not found", certFiles[i]);
        useHttps = false;
      }
    }
    if (!useHttps) LOG_WRN("HTTPS not available as server keys not loaded, using HTTP");
  }
}


/********* 远程服务器证书 *********/

// GitHub 公钥证书，有效期至 2031 年 4 月
const char* git_rootCACertificate = R"~(
-----BEGIN CERTIFICATE-----
MIIEyDCCA7CgAwIBAgIQDPW9BitWAvR6uFAsI8zwZjANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0yMTAzMzAwMDAwMDBaFw0zMTAzMjkyMzU5NTlaMFkxCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxMzAxBgNVBAMTKkRpZ2lDZXJ0IEdsb2Jh
bCBHMiBUTFMgUlNBIFNIQTI1NiAyMDIwIENBMTCCASIwDQYJKoZIhvcNAQEBBQAD
ggEPADCCAQoCggEBAMz3EGJPprtjb+2QUlbFbSd7ehJWivH0+dbn4Y+9lavyYEEV
cNsSAPonCrVXOFt9slGTcZUOakGUWzUb+nv6u8W+JDD+Vu/E832X4xT1FE3LpxDy
FuqrIvAxIhFhaZAmunjZlx/jfWardUSVc8is/+9dCopZQ+GssjoP80j812s3wWPc
3kbW20X+fSP9kOhRBx5Ro1/tSUZUfyyIxfQTnJcVPAPooTncaQwywa8WV0yUR0J8
osicfebUTVSvQpmowQTCd5zWSOTOEeAqgJnwQ3DPP3Zr0UxJqyRewg2C/Uaoq2yT
zGJSQnWS+Jr6Xl6ysGHlHx+5fwmY6D36g39HaaECAwEAAaOCAYIwggF+MBIGA1Ud
EwEB/wQIMAYBAf8CAQAwHQYDVR0OBBYEFHSFgMBmx9833s+9KTeqAx2+7c0XMB8G
A1UdIwQYMBaAFE4iVCAYlebjbuYP+vq5Eu0GF485MA4GA1UdDwEB/wQEAwIBhjAd
BgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwdgYIKwYBBQUHAQEEajBoMCQG
CCsGAQUFBzABhhhodHRwOi8vb2NzcC5kaWdpY2VydC5jb20wQAYIKwYBBQUHMAKG
NGh0dHA6Ly9jYWNlcnRzLmRpZ2ljZXJ0LmNvbS9EaWdpQ2VydEdsb2JhbFJvb3RH
Mi5jcnQwQgYDVR0fBDswOTA3oDWgM4YxaHR0cDovL2NybDMuZGlnaWNlcnQuY29t
L0RpZ2lDZXJ0R2xvYmFsUm9vdEcyLmNybDA9BgNVHSAENjA0MAsGCWCGSAGG/WwC
ATAHBgVngQwBATAIBgZngQwBAgEwCAYGZ4EMAQICMAgGBmeBDAECAzANBgkqhkiG
9w0BAQsFAAOCAQEAkPFwyyiXaZd8dP3A+iZ7U6utzWX9upwGnIrXWkOH7U1MVl+t
wcW1BSAuWdH/SvWgKtiwla3JLko716f2b4gp/DA/JIS7w7d7kwcsr4drdjPtAFVS
slme5LnQ89/nD/7d+MS5EHKBCQRfz5eeLjJ1js+aWNJXMX43AYGyZm0pGrFmCW3R
bpD0ufovARTFXFZkAdl9h6g4U5+LXUZtXMYnhIHUfoyMo5tS58aI7Dd8KvvwVVo4
chDYABPPTHPbqjc1qCmBaZx2vN4Ye5DUys/vZwP9BFohFrH/6j/f3IL16/RZkiMN
JCqVJUzKoZHm1Lesh3Sz8W2jmdv51b2EQJ8HmA==
-----END CERTIFICATE-----
)~";

// 您的 FTPS 服务器根公钥证书（未实现）
const char* ftps_rootCACertificate = R"~(
)~";


// 您的 SMTP 服务器公钥证书（例如 smtp.gmail.com，有效期至 2028 年 1 月）
const char* smtp_rootCACertificate = R"~(
-----BEGIN CERTIFICATE-----
MIIFYjCCBEqgAwIBAgIQd70NbNs2+RrqIQ/E8FjTDTANBgkqhkiG9w0BAQsFADBX
MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE
CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIwMDYx
OTAwMDA0MloXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT
GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFIx
MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAthECix7joXebO9y/lD63
ladAPKH9gvl9MgaCcfb2jH/76Nu8ai6Xl6OMS/kr9rH5zoQdsfnFl97vufKj6bwS
iV6nqlKr+CMny6SxnGPb15l+8Ape62im9MZaRw1NEDPjTrETo8gYbEvs/AmQ351k
KSUjB6G00j0uYODP0gmHu81I8E3CwnqIiru6z1kZ1q+PsAewnjHxgsHA3y6mbWwZ
DrXYfiYaRQM9sHmklCitD38m5agI/pboPGiUU+6DOogrFZYJsuB6jC511pzrp1Zk
j5ZPaK49l8KEj8C8QMALXL32h7M1bKwYUH+E4EzNktMg6TO8UpmvMrUpsyUqtEj5
cuHKZPfmghCN6J3Cioj6OGaK/GP5Afl4/Xtcd/p2h/rs37EOeZVXtL0m79YB0esW
CruOC7XFxYpVq9Os6pFLKcwZpDIlTirxZUTQAs6qzkm06p98g7BAe+dDq6dso499
iYH6TKX/1Y7DzkvgtdizjkXPdsDtQCv9Uw+wp9U7DbGKogPeMa3Md+pvez7W35Ei
Eua++tgy/BBjFFFy3l3WFpO9KWgz7zpm7AeKJt8T11dleCfeXkkUAKIAf5qoIbap
sZWwpbkNFhHax2xIPEDgfg1azVY80ZcFuctL7TlLnMQ/0lUTbiSw1nH69MG6zO0b
9f6BQdgAmD06yK56mDcYBZUCAwEAAaOCATgwggE0MA4GA1UdDwEB/wQEAwIBhjAP
BgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBTkrysmcRorSCeFL1JmLO/wiRNxPjAf
BgNVHSMEGDAWgBRge2YaRQ2XyolQL30EzTSo//z9SzBgBggrBgEFBQcBAQRUMFIw
JQYIKwYBBQUHMAGGGWh0dHA6Ly9vY3NwLnBraS5nb29nL2dzcjEwKQYIKwYBBQUH
MAKGHWh0dHA6Ly9wa2kuZ29vZy9nc3IxL2dzcjEuY3J0MDIGA1UdHwQrMCkwJ6Al
oCOGIWh0dHA6Ly9jcmwucGtpLmdvb2cvZ3NyMS9nc3IxLmNybDA7BgNVHSAENDAy
MAgGBmeBDAECATAIBgZngQwBAgIwDQYLKwYBBAHWeQIFAwIwDQYLKwYBBAHWeQIF
AwMwDQYJKoZIhvcNAQELBQADggEBADSkHrEoo9C0dhemMXoh6dFSPsjbdBZBiLg9
NR3t5P+T4Vxfq7vqfM/b5A3Ri1fyJm9bvhdGaJQ3b2t6yMAYN/olUazsaL+yyEn9
WprKASOshIArAoyZl+tJaox118fessmXn1hIVw41oeQa1v1vg4Fv74zPl6/AhSrw
9U5pCZEt4Wi4wStz6dTZ/CLANx8LZh1J7QJVj2fhMtfTJr9w4z30Z209fOU0iOMy
+qduBmpvvYuR7hZL6Dupszfnw0Skfths18dG9ZKb59UhvmaSGZRVbNQpsg3BZlvi
d0lIKO2d1xozclOzgjXPYovJJIultzkMu34qQb9Sz/yilrbCgj8=
-----END CERTIFICATE-----
)~";


// 您的 MQTT 服务器公钥证书
const char* mqtt_rootCACertificate = R"~(
)~";

// Telegram 服务器证书（api.telegram.org），有效期至 2031 年 5 月
const char* telegram_rootCACertificate = R"~(
-----BEGIN CERTIFICATE-----
MIIEfTCCA2WgAwIBAgIDG+cVMA0GCSqGSIb3DQEBCwUAMGMxCzAJBgNVBAYTAlVT
MSEwHwYDVQQKExhUaGUgR28gRGFkZHkgR3JvdXAsIEluYy4xMTAvBgNVBAsTKEdv
IERhZGR5IENsYXNzIDIgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTQwMTAx
MDcwMDAwWhcNMzEwNTMwMDcwMDAwWjCBgzELMAkGA1UEBhMCVVMxEDAOBgNVBAgT
B0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoTEUdvRGFkZHku
Y29tLCBJbmMuMTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRpZmljYXRlIEF1
dGhvcml0eSAtIEcyMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAv3Fi
CPH6WTT3G8kYo/eASVjpIoMTpsUgQwE7hPHmhUmfJ+r2hBtOoLTbcJjHMgGxBT4H
Tu70+k8vWTAi56sZVmvigAf88xZ1gDlRe+X5NbZ0TqmNghPktj+pA4P6or6KFWp/
3gvDthkUBcrqw6gElDtGfDIN8wBmIsiNaW02jBEYt9OyHGC0OPoCjM7T3UYH3go+
6118yHz7sCtTpJJiaVElBWEaRIGMLKlDliPfrDqBmg4pxRyp6V0etp6eMAo5zvGI
gPtLXcwy7IViQyU0AlYnAZG0O3AqP26x6JyIAX2f1PnbU21gnb8s51iruF9G/M7E
GwM8CetJMVxpRrPgRwIDAQABo4IBFzCCARMwDwYDVR0TAQH/BAUwAwEB/zAOBgNV
HQ8BAf8EBAMCAQYwHQYDVR0OBBYEFDqahQcQZyi27/a9BUFuIMGU2g/eMB8GA1Ud
IwQYMBaAFNLEsNKR1EwRcbNhyz2h/t2oatTjMDQGCCsGAQUFBwEBBCgwJjAkBggr
BgEFBQcwAYYYaHR0cDovL29jc3AuZ29kYWRkeS5jb20vMDIGA1UdHwQrMCkwJ6Al
oCOGIWh0dHA6Ly9jcmwuZ29kYWRkeS5jb20vZ2Ryb290LmNybDBGBgNVHSAEPzA9
MDsGBFUdIAAwMzAxBggrBgEFBQcCARYlaHR0cHM6Ly9jZXJ0cy5nb2RhZGR5LmNv
bS9yZXBvc2l0b3J5LzANBgkqhkiG9w0BAQsFAAOCAQEAWQtTvZKGEacke+1bMc8d
H2xwxbhuvk679r6XUOEwf7ooXGKUwuN+M/f7QnaF25UcjCJYdQkMiGVnOQoWCcWg
OJekxSOTP7QYpgEGRJHjp2kntFolfzq3Ms3dhP8qOCkzpN1nsoX+oYggHFCJyNwq
9kIDN0zmiN/VryTyscPfzLXs4Jlet0lUIDyUGAzHHFIYSaRt4bNYC8nY7NmuHDKO
KHAN4v6mF56ED71XcLNa6R+ghlO773z/aQvgSMO3kwvIClTErF0UZzdsyqUvMQg3
qm5vjLyb4lddJIGvl5echK1srDdMZvNhkREg5L4wn3qkKQmw4TRfZHcYQFHfjDCm
rw==
-----END CERTIFICATE-----
)~";


// 您的 HTTPS 文件服务器公钥证书
const char* hfs_rootCACertificate = R"~(
-----BEGIN CERTIFICATE-----
MIIDKzCCAhOgAwIBAgIQJOOyowYpLEiLnhpjD0HR5DANBgkqhkiG9w0BAQsFADAg
MR4wHAYDVQQDExVSZWJleCBUaW55IFdlYiBTZXJ2ZXIwHhcNMjMxMTI1MTQ1NzU4
WhcNMzMxMTI1MTQ1NzU4WjAgMR4wHAYDVQQDExVSZWJleCBUaW55IFdlYiBTZXJ2
ZXIwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDGgUPfvM+SI1OiOozW
MQUtj/E9461SRFIYfce7b0KANrc/0S4qD11tTQvKvoY7HY5gvc0jgWZBGqQ6/KO7
CIrLnXtsldeUXokcd8cvr8Ko704iOboHLLhewZ4egeBgu6+/1dw6REeExHjNIjiO
1InShPIV8yxX1oUo+EztIo5qecVvrousyIL9KBmAAi7Pdw0/yKQaoXzL9Ehkpv7g
Pbabt6k7W+CofXRhaoGlf5ERvb5T/921PXXdCq7mnB9OjGu0almqYIWh1jRjnBQH
e2avg+LD/+1dakIXXzByhbEpECxtQHZ17iB3DvW0ExiMH+0A8bqQIMp3sOgEzf2J
42XpAgMBAAGjYTBfMA4GA1UdDwEB/wQEAwIHgDAdBgNVHQ4EFgQUUmrFj3+h5tsV
ASwGkjFZ9W5FDf8wEwYDVR0lBAwwCgYIKwYBBQUHAwEwGQYDVR0RBBIwEIIJbG9j
YWxob3N0ggNtcG8wDQYJKoZIhvcNAQELBQADggEBAJ+rf1UUwaZAhsHrL2KX0WPm
E9lCcnBFeQHUILSfM7r7fbEuIXa68mZDeMIV9xs4ex45wN4AAZW1l79Okia9kin8
JkqkhZ/rCvqWsbNt3ryOvWCB2a2JEWW6yRA6EgK+STo3T/Z8Sau0ys8woc7y486l
5BhGu7rlXcbXl8hcEORD/ILxxdae7hHi7sXIReyS2kGiYJUwj+1+6mm26TXuRyCV
jqlsBxH8gnwIlupODKZ/7jU/HhiYaKEbrnNxiOiPeWAw/KJJH5lUxt0piOYIXhj4
DuDay+U7jeJKpND7EYheZY/U6c1wqwXt1DHuFnCCzK8jdOGT9aUSqZUeWfNn9cc=
-----END CERTIFICATE-----
)~";

#endif
