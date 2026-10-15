#include "modbus_rtu.h"
#include "usart.h"
#include <string.h>

extern UART_HandleTypeDef huart2;

uint16_t Modbus_CRC16(const uint8_t *buffer, uint16_t length)
{
    // CRC寄存器初值,标准规定0xFFFF
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < length; i++)
    {
        crc ^= buffer[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x0001) // 取出最低位检查
            {
                // 右移一位,并于A001进行异或判断
                crc = (crc >> 1) ^ 0xA001;
            }
            else    //最低位为零
            {
                //仅右移一位
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

void Modbus_Init(void)
{
    RS485_RX_EN();//默认保持接收监听
}

/**
 * @brief 向kth2 下发03功能码读取温湿度
 * @param out_data 返回解析后的结构体
 */
bool KTH2R_ReadData(SensorData_t *out_date)
{
    // KTH2-R 标准查询指令 (读 0x0000 起始的 2 个寄存器)
    // 01(地址) 03(功能码) 00 00(起始地址) 00 02(寄存器数) C4 0B(CRC)
    uint8_t tx_frame[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B};
    uint8_t rx_frame[9] = {0};

    //切换发送模式
    RS485_TX_EN();
    HAL_UART_Transmit(&huart2 , tx_frame,sizeof(tx_frame),100);
    
    //等待最后一个字体移出寄存器（TC）
    while (__HAL_UART_GET_FLAG(&huart2,UART_FLAG_TC) == RESET);
     
    //切换接收模式
    RS485_RX_EN();

    //阻塞等待9字节返回
    HAL_StatusTypeDef status = HAL_UART_Receive(&huart2,rx_frame,9,200);
    if (status != HAL_OK)
    {
        out_date->is_valid = false;
        return false;
    }
    
    //校验从机地址与功能码
    if (rx_frame[0] != 0x01 || rx_frame[1] != 0x03 || rx_frame[2] != 0x04)
    {
        out_date->is_valid = false;
        return false;
    }

    //校验crc16
    uint16_t cal_crc = Modbus_CRC16(rx_frame, 7);
    uint16_t rec_crc = rx_frame[7] | (rx_frame[8] << 8); // CRC 低字节在前，高字节在后
    if (cal_crc != rec_crc)
    {
        out_date->is_valid = false;
        return false;
    }

    //解析温度与湿度 (KTH2-R 多数为: 湿度寄存器在前, 温度寄存器在后)
    uint16_t raw_humi = (rx_frame[3] << 8) | rx_frame[4];
    int16_t  raw_temp = (int16_t)((rx_frame[5] << 8) | rx_frame[6]);

    out_date->humidity = raw_humi / 10.0f;
    out_date->temperature = raw_temp / 10.0f;
    out_date->is_valid = true;

    return true;
}

