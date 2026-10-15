#ifndef __MODBUS_RTU_H
#define __MODBUS_RTU_H

#include "main.h"
#include <stdbool.h>

//KTH2-R 数据测量结构体
typedef struct 
{
    float temperature;      //温度
    float humidity;         //湿度
    bool is_valid;          //数据有效性
} SensorData_t;

#define RS485_TX_EN()       HAL_GPIO_WritePin(RS485_RE_DE_GPIO_Port,RS485_RE_DE_Pin,GPIO_PIN_SET);
#define RS485_RX_EN()       HAL_GPIO_WritePin(RS485_RE_DE_GPIO_Port,RS485_RE_DE_Pin,GPIO_PIN_RESET);

//api接口
void Modbus_Init(void);
uint16_t Modbus_CRC16(const uint8_t *buffer,uint16_t length);
bool KTH2R_ReadData(SensorData_t *out_date);


#endif

