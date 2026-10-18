#ifndef __ESP8266_OTA_H
#define __ESP8266_OTA_H

#include "main.h"
#include "stdbool.h"

#define ESP_RX_BUF_SIZE 2048
//w25q64容量8mb
//最小擦除代为为扇区(sector) = 4kb(4096 bytes)
//写入单位:页(page) = 256 bytes

#define W25Q64_OTA_FLAG_ADDR    0x000FF000  //255号扇区:存放ota标志与元信息
#define W25Q64_OTA_BIN_ADDR     0x00100000  //1mb编译处器:专门存放下载的新固件 .bin
#define OTA_MAGIC_NUMBER        0x5AA5F00F  //该数字用于识别BootLoader识别是否存在合法的升级请求

//状态枚举，用于展示ui当前进度
typedef enum {
    OTA_STATE_IDLE = 0,         // 空闲状态
    OTA_STATE_WIFI_CONNECTING,  // Wi-Fi 正在连接中
    OTA_STATE_WIFI_CONNECTED,   // Wi-Fi 连接成功，已获取 IP
    OTA_STATE_DOWNLOADING,      // 正在从 HTTP 服务器拉取固件 bin
    OTA_STATE_VERIFY_OK,        // 固件下载完毕且 CRC32 校验成功
    OTA_STATE_ERROR             // 发生错误（连接超时、校验失败等）
} OTA_State_t;


typedef struct 
{
    uint32_t magic;
    uint32_t fw_size;
    uint32_t fw_crc32;
    uint32_t version;
}OTA_Info_t;

extern uint8_t esp_rx_buf[ESP_RX_BUF_SIZE];
/* USART3 出错次数（ORE/NE/FE/PE）。涨了说明曾经溢出过，
 * 溢出会让 HAL abort 接收，必须靠 HAL_UART_ErrorCallback 重新挂上 */
extern volatile uint32_t g_uart3_err_cnt;


void ESP_8266_ClearBuf(void);
uint16_t ESP8266_GetRxLen(void);
void ESP8266_Init(void);
bool ESP8266_SendCmd(const char *cmd, const char *ack, uint32_t timeout_ms);
bool ESP8266_JoinAP(const char *ssid, const char *pwd);
bool ESP8266_HTTP_OTA_Download(const char *server_ip, uint16_t port, const char *file_path);


#endif
