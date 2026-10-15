#ifndef __W25Q64_H
#define __W25Q64_H

#include "main.h"

/* W25Q64 常用指令集 */
#define W25Q_WRITE_ENABLE       0x06
#define W25Q_WRITE_DISABLE      0x04
#define W25Q_READ_STATUS_REG_1  0x05
#define W25Q_READ_DATA          0x03
#define W25Q_FAST_READ_DATA     0x0B
#define W25Q_PAGE_PROGRAM       0x02
#define W25Q_SECTOR_ERASE       0x20
#define W25Q_BLOCK_ERASE_64K    0xD8
#define W25Q_CHIP_ERASE         0xC7
#define W25Q_DEVICE_ID          0x90
#define W25Q_JEDEC_ID           0x9F

/* 片选 CS 控制宏 (PD1) */
#define W25Q_CS_LOW()   HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET)
#define W25Q_CS_HIGH()  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET)

/* 基础 API 接口 */
uint16_t W25Q64_ReadID(void);
uint32_t W25Q64_ReadJEDECID(void);
void W25Q64_Read(uint32_t addr, uint8_t *buf, uint16_t size);
void W25Q64_Write(uint32_t addr, uint8_t *buf, uint16_t size);
void W25Q64_EraseSector(uint32_t sector_addr);

#endif /* __W25Q64_H */