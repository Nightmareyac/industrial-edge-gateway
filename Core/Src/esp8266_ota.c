#include "esp8266_ota.h"
#include "usart.h"
#include "w25q64.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern UART_HandleTypeDef huart3; // pb10 pb11

/* 串口缓冲区设计
 *  esp返回的at响应和http固件流长度是不定长的，波特率高达115200
 * 如果使用 Hal_Uart_Receive轮询，FreeRtos任务切换的空窗几毫秒就会导致串口ORE(溢出错误)丢包;
 * 因此必须使用串口中断单字节接收 + 线性/环形缓冲区 机制
 * 硬件没收到一字节就存入rxbuf 后台任务解析rxbuf
 */
#define ESP_RX_BUF_SIZE 2048
static uint8_t esp_rx_buf[ESP_RX_BUF_SIZE];
static volatile uint16_t esp_rx_len = 0;
static uint8_t uart3_rx_byte = 0;

// 清空接收缓冲区
static void ESP_8266_ClearBuf(void)
{
    memset(esp_rx_buf, 0, ESP_RX_BUF_SIZE);
    esp_rx_len = 0;
}

// hal库串口回调函数:接收一字节自动调用，压入缓冲区并开启下次接收
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    //判断触发源
    if (huart->Instance == USART3)
    {
        //防溢出保护:检查当前缓冲区是否溢出
        if (esp_rx_len <(ESP_RX_BUF_SIZE -1))
        {
            //存入仓库并且移动游标
            //将接收到的一个字节存入buf，然后长度++
            esp_rx_buf[esp_rx_len++] = uart3_rx_byte;
        }

        //收完字节后中断停止，继续监听下一个字节
        HAL_UART_Receive_IT(&huart3,&uart3_rx_byte,1);
        
    }
    
}

void ESP8266_Init(void)
{
    ESP_8266_ClearBuf();

    // 启动一次单字节接收中断
    HAL_UART_Receive_IT(&huart3, &uart3_rx_byte, 1);

    // 透传模式发送+++退出透传
    HAL_UART_Transmit(&huart3, (uint8_t *)"+++", 3, 100);
    osDelay(1000);

    // 发送AT测试并关闭回显(ATE0),减少数据干扰
    ESP8266_SendCmd("AT\r\n", "OK", 500);
    ESP8266_SendCmd("ATE0\r\n", "OK", 500);
}

/**
 * @brief 想esp发送at指令并等待期望字符串
 * @param cmd 要发送的指定
 * @param ack 期待的子字符串
 * @param timeout_ms 超时等待时间
 */
bool ESP8266_SendCmd(const char *cmd, const char *ack, uint32_t timeout_ms)
{
    ESP_8266_ClearBuf();
    // 发送测试
    HAL_UART_Transmit(&huart3, (uint8_t *)cmd, strlen(cmd), 200);

    if (ack == NULL)
    {
        return true;
    }

    // 存储当前时间戳
    uint32_t start_tick = osKernelGetTickCount();
    while ((osKernelGetTickCount() - start_tick) < timeout_ms)
    {
        // 在接收缓冲区是否出现了期望的应答字符串ack
        if (strstr((const char *)esp_rx_buf, ack) != NULL)
        {
            return true;
        }
        osDelay(10); // 让出cpu
    }
    return false;
}

/**
 * @brief 连接wifi
 */
bool ESP8266_JoinAP(const char *ssid, const char *pwd)
{
    char cmd_buf[128];

    // 1设置为station模式 （客户端模式)
    if (!ESP8266_SendCmd("AT+CWMODE=1\r\n", "OK", 1000))
    {
        return false;
    }

    // 拼接热点名称和密码，超时10s
    snprintf(cmd_buf, sizeof(cmd_buf), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
    if (!ESP8266_SendCmd(cmd_buf, "WIFI GOT IP", 10000))
    {
        return false;
    }

    return true;
}

/**
 * @brief 计算软件 CRC32 校验码（标准多项式 0xEDB88320）
 * @note  用于校验写入 W25Q64 的完整固件是否一字不差
 */
static uint32_t Calc_CRC32(uint32_t init_crc, const uint8_t *data, uint32_t len)
{
    uint32_t crc = init_crc;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 1)
            {
                crc = (crc >> 1) ^ 0xEDB88320;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/**
 * @brief 存储器跨页写入,单次最多写入256即一页数据,否则超出会覆盖本页开头
 */
static void W25Q64_Write_Safe(uint32_t addr, uint8_t *pbuf, uint32_t len)
{
    uint32_t page_remain = 256 - (addr % 256);
    if (len <= page_remain)
    {
        page_remain = len;
    }

    while (1)
    {
        // 调用w25q64页编程函数
        W25Q64_Write(addr, pbuf, page_remain);

        if (len == page_remain)
        {
            break;
        }
        else
        {
            pbuf += page_remain;
            addr += page_remain;
            len -= page_remain;
            if (len > 256)
            {
                page_remain = 256;
            }
            else
            {
                page_remain = len;
            }
        }
    }
}

/**
 * @brief 通过http get下载固件
 *
 * @param server_ip 服务器ip
 * @param port      http 端口
 * @param file_path 固件路径
 */
bool ESP8266_HTTP_OTA_Download(const char *server_ip, uint16_t port, const char *file_path)
{
    char cmd_buf[256];
    char http_req[256];

    // 简历tcp连接
    snprintf(cmd_buf, sizeof(cmd_buf), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", server_ip, port);
    if (!ESP8266_SendCmd(cmd_buf, "CONNECT", 5000))
        return false;

    // 开启透传
    // 非透传，数据会加上IPD前缀，拆包复杂
    if (!ESP8266_SendCmd("AT+CIPMODE=1\r\n", "OK", 1000))
        return false;

    if (!ESP8266_SendCmd("AT+CIPSENMD\r\n", ">", 1000))
        return false;

    // 构造标准http get请求报文
    snprintf(http_req, sizeof(http_req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n\r\n",
             file_path, server_ip, port);

    // 等待http响应并解析bin
    uint32_t timeout = osKernelGetTickCount();
    char *header_end = NULL;
    while ((osKernelGetTickCount() - timeout) < 5000)
    {
        // /r/n/r/n是响应头和固件包的分界线
        header_end = strstr((const char *)esp_rx_buf, "\r\n\r\n");
        if (header_end != NULL)
        {
            break;
        }
        osDelay(10);
    }
    if (header_end == NULL)
        return false; // 未收到响应头

    if (strstr((const char *)esp_rx_buf, "200 OK") == NULL)
        return false; // 检查状态码是否为200 ok

    // 提取content-length 字段获取固件大小
    uint32_t total_fw_size = 0;
    char *content_len_ptr = strstr((const char *)esp_rx_buf, "Content-Length");
    if (content_len_ptr != NULL)
    {
        // ASCII转换为整数
        total_fw_size = atoi(content_len_ptr + 15);
    }
    // 固件大小异常
    if (total_fw_size == 0 || total_fw_size > (960 * 1024))
    {
        return false;
    }

    // 根据固件大小，预先擦除w25q64对应扇区
    // nor flash 只能写1为0，写入前必须按4kb扇区擦除恢复为全0xFF
    uint32_t sector_to_erase = (total_fw_size + 4095) / 4096;
    for (uint32_t i = 0; i < sector_to_erase; i++)
    {
        W25Q64_EraseSector(W25Q64_OTA_BIN_ADDR + (i * 4096));
        osDelay(5);
    }

    // 写入/r/n/r/n后面的二进制固件到flash
    uint8_t *bin_start_ptr = (uint8_t *)(header_end + 4); // 跳过/r/n
    uint32_t header_bytes = bin_start_ptr - esp_rx_buf;
    uint32_t written_bytes = 0;
    uint32_t write_addr = W25Q64_OTA_BIN_ADDR;
    uint32_t crc32_val = 0xFFFFFFFF;

    // 进入临界区拷贝当前残余数据
    // 串口接收是在中断中进行的，所以需要进入临界区
    taskENTER_CRITICAL();
    uint32_t first_chunk_len = esp_rx_len - header_bytes;
    static uint8_t temp_chunk[ESP_RX_BUF_SIZE];
    memcpy(temp_chunk, bin_start_ptr, first_chunk_len);
    esp_rx_len = 0; // 终止接收游标
    taskEXIT_CRITICAL();

    if (first_chunk_len > 0)
    {
        // 写入到专属放置区
        W25Q64_Write(write_addr, temp_chunk, first_chunk_len);
        // 进行校验
        crc32_val = Calc_CRC32(crc32_val, temp_chunk, first_chunk_len);

        //游标向前推进
        write_addr += first_chunk_len;      //Flash写入地址向后移
        written_bytes += first_chunk_len;   //记录总共存了多少字节
    }

    uint32_t last_recv_tick = osKernelGetTickCount();
    while (written_bytes < total_fw_size)
    {
        if (esp_rx_len > 0)
        {
            taskENTER_CRITICAL();
            // 串口中断的数据复制进temp_chunk
            uint16_t chunk_len = esp_rx_len;
            memcpy(temp_chunk, esp_rx_buf, chunk_len);
            //计数清零开始下一批
            esp_rx_len = 0;
            taskEXIT_CRITICAL();

            //多出的字节直接丢弃
            if (written_bytes + chunk_len > total_fw_size)
            {
                chunk_len = total_fw_size - written_bytes;
            }

            W25Q64_Write(write_addr, temp_chunk, chunk_len);
            crc32_val = Calc_CRC32(crc32_val, temp_chunk, chunk_len);

            write_addr += chunk_len;
            written_bytes += chunk_len;
            last_recv_tick = osKernelGetTickCount();
        }
        else
        {
            if ((osKernelGetTickCount() - last_recv_tick) > 5000)
            {
                return false; // 断流超时
            }
            osDelay(5);
        }
    }

    crc32_val ^= 0xFFFFFFFF;

    //退出透传关闭tcp连接
    osDelay(1000);
    HAL_UART_Transmit(&huart3,(uint8_t *)"+++",3,100);
    osDelay(1000);
    ESP8266_SendCmd("AT+CIPMODE=0\r\n","OK",1000);
    ESP8266_SendCmd("AT+CIPCLOSE=0\r\n","OK",1000);
    

    //写入OTA标志到255号扇区,并重启进入Bootloader
    OTA_Info_t ota_info;
    ota_info.magic= OTA_MAGIC_NUMBER;
    ota_info.fw_size = total_fw_size;
    ota_info.fw_crc32 = crc32_val;
    ota_info.version = 201;//v2.1

    W25Q64_EraseSector(W25Q64_OTA_FLAG_ADDR);
    W25Q64_Write_Safe(W25Q64_OTA_FLAG_ADDR, (uint8_t *)&ota_info, sizeof(OTA_Info_t));

    osDelay(1000);
    NVIC_SystemReset();

    return true;
}
