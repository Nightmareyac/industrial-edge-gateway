#include "w25q64.h"
#include "spi.h"

extern SPI_HandleTypeDef hspi1;

//spi单字节收发
static uint8_t SPI1_ReadWriteByte(uint8_t data)
{
    uint8_t rx_data = 0;
    HAL_SPI_TransmitReceive(&hspi1,&data,&rx_data,1,100);
    return rx_data;
}

//写使能
static void W25Q64_WriteEnable(void)
{
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(W25Q_WRITE_ENABLE);
    W25Q_CS_HIGH();
}

//等待写入/擦除空闲
static void W25Q64_WaitBusy(void)
{
    uint8_t status = 0;
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(W25Q_READ_STATUS_REG_1);
    while ((SPI1_ReadWriteByte(0xff)&0x01) == 0x01);//检查是否繁忙
    W25Q_CS_HIGH();
}

//读取芯片ID
uint16_t W25Q64_ReadID(void)
{
    uint16_t temp = 0;
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(W25Q_DEVICE_ID);
    SPI1_ReadWriteByte(0x00);
    SPI1_ReadWriteByte(0x00);
    SPI1_ReadWriteByte(0x00);
    temp |= SPI1_ReadWriteByte(0xFF)<<8;
    temp |= SPI1_ReadWriteByte(0xFF);
    W25Q_CS_HIGH();
    return temp;

}


/* 读取 JEDEC ID (W25Q64 正常返回 0xEF4017) */
uint32_t W25Q64_ReadJEDECID(void)
{
    uint32_t id = 0;
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(W25Q_JEDEC_ID);
    id |= (uint32_t)SPI1_ReadWriteByte(0xFF) << 16;
    id |= (uint32_t)SPI1_ReadWriteByte(0xFF) << 8;
    id |= (uint32_t)SPI1_ReadWriteByte(0xFF);
    W25Q_CS_HIGH();
    return id;
}

/* 扇区擦除 (4KB，传入扇区起始地址) */
void W25Q64_EraseSector(uint32_t sector_addr)
{
    W25Q64_WriteEnable();
    W25Q64_WaitBusy();

    W25Q_CS_LOW();
    SPI1_ReadWriteByte(W25Q_SECTOR_ERASE);
    SPI1_ReadWriteByte((uint8_t)((sector_addr) >> 16));
    SPI1_ReadWriteByte((uint8_t)((sector_addr) >> 8));
    SPI1_ReadWriteByte((uint8_t)sector_addr);
    W25Q_CS_HIGH();

    W25Q64_WaitBusy();
}

/* 连续读取数据 */
void W25Q64_Read(uint32_t addr, uint8_t *buf, uint16_t size)
{
    W25Q_CS_LOW();
    SPI1_ReadWriteByte(W25Q_READ_DATA);
    SPI1_ReadWriteByte((uint8_t)((addr) >> 16));
    SPI1_ReadWriteByte((uint8_t)((addr) >> 8));
    SPI1_ReadWriteByte((uint8_t)addr);

    HAL_SPI_Receive(&hspi1, buf, size, 1000);
    W25Q_CS_HIGH();
}

/* 页编程写入 (单页最大 256 字节) */
static void W25Q64_WritePage(uint32_t addr, uint8_t *buf, uint16_t size)
{
    W25Q64_WriteEnable();

    W25Q_CS_LOW();
    SPI1_ReadWriteByte(W25Q_PAGE_PROGRAM);
    SPI1_ReadWriteByte((uint8_t)((addr) >> 16));
    SPI1_ReadWriteByte((uint8_t)((addr) >> 8));
    SPI1_ReadWriteByte((uint8_t)addr);

    HAL_SPI_Transmit(&hspi1, buf, size, 1000);
    W25Q_CS_HIGH();

    W25Q64_WaitBusy();
}

/* 任意长度数据写入 (带跨页处理) */
void W25Q64_Write(uint32_t addr, uint8_t *buf, uint16_t size)
{
    uint16_t page_offset = addr % 256;
    uint16_t page_remain = 256 - page_offset;

    if (size <= page_remain)
    {
        page_remain = size;
    }

    while (1)
    {
        W25Q64_WritePage(addr, buf, page_remain);
        if (size == page_remain)
        {
            break;
        }
        else
        {
            buf += page_remain;
            addr += page_remain;
            size -= page_remain;
            page_remain = (size > 256) ? 256 : size;
        }
    }
}







