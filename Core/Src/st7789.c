#include "st7789.h"
#include "main.h"
#include "main.h"
#include "font.h"


extern SPI_HandleTypeDef hspi2;

static void ST7789_WriteCmd(uint8_t cmd)
{
    LCD_DC_CLR();
    LCD_CS_CLR();
    HAL_SPI_Transmit(&hspi2, &cmd, 1, 10);
    LCD_CS_SET();
}

static void ST7789_WriteData(uint8_t data)
{
    LCD_DC_SET();
    LCD_CS_CLR();
    HAL_SPI_Transmit(&hspi2, &data, 1, 10);
    LCD_CS_SET();
}

static void ST7789_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    ST7789_WriteCmd(0x2A); // 列地址设置
    ST7789_WriteData(x0 >> 8);
    ST7789_WriteData(x0 & 0xFF);
    ST7789_WriteData(x1 >> 8);
    ST7789_WriteData(x1 & 0xFF);

    ST7789_WriteCmd(0x2B); // 行地址设置
    ST7789_WriteData(y0 >> 8);
    ST7789_WriteData(y0 & 0xFF);
    ST7789_WriteData(y1 >> 8);
    ST7789_WriteData(y1 & 0xFF);
}

void ST7789_Init(void)
{
    LCD_BLK_SET(); // 点亮背光

    /* 硬件复位脉冲 (PB14) */
    LCD_RST_CLR();
    HAL_Delay(50);
    LCD_RST_SET();
    HAL_Delay(120);

    ST7789_WriteCmd(0x11); // 退出睡眠 (SLPOUT)
    HAL_Delay(120);

    ST7789_WriteCmd(0x36); // 显示方向 (MADCTL)
    ST7789_WriteData(0x00);

    ST7789_WriteCmd(0x3A); // 像素格式 (COLMOD): 16-bit RGB565
    ST7789_WriteData(0x05);

    ST7789_WriteCmd(0xB2); // Porch 控制
    ST7789_WriteData(0x0C);
    ST7789_WriteData(0x0C);
    ST7789_WriteData(0x00);
    ST7789_WriteData(0x33);
    ST7789_WriteData(0x33);

    ST7789_WriteCmd(0xB7); // Gate 控制
    ST7789_WriteData(0x35);

    ST7789_WriteCmd(0xBB); // VCOM 控制
    ST7789_WriteData(0x19);

    ST7789_WriteCmd(0xC0); // LCM 控制
    ST7789_WriteData(0x2C);

    ST7789_WriteCmd(0xC2); // VDV/VRH 控制
    ST7789_WriteData(0x01);

    ST7789_WriteCmd(0xC3); // VRH 设置
    ST7789_WriteData(0x12);

    ST7789_WriteCmd(0xC4); // VDV 设置
    ST7789_WriteData(0x20);

    ST7789_WriteCmd(0xC6); // 帧率控制 (60Hz)
    ST7789_WriteData(0x0F);

    ST7789_WriteCmd(0xD0); // 电源控制
    ST7789_WriteData(0xA4);
    ST7789_WriteData(0xA1);

    ST7789_WriteCmd(0x21); // 显示反转开启 (INVON，ST7789 必备，否则颜色相反)
    ST7789_WriteCmd(0x29); // 开启显示 (DISPON)
    HAL_Delay(50);
}

void ST7789_Clear(uint16_t color)
{
    ST7789_SetWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    // 1. 发送 0x2C 指令写显存
    LCD_DC_CLR();
    LCD_CS_CLR();
    uint8_t cmd = 0x2C;
    HAL_SPI_Transmit(&hspi2, &cmd, 1, 10);

    // 2. 切为数据模式，CS 全程拉低
    LCD_DC_SET();

    // 3. 按行缓冲高效发送 (240 像素 * 2 字节 = 480 字节)
    uint8_t line_buf[LCD_WIDTH * 2];
    for (int i = 0; i < LCD_WIDTH; i++)
    {
        line_buf[2 * i]     = (uint8_t)(color >> 8);
        line_buf[2 * i + 1] = (uint8_t)(color & 0xFF);
    }

    for (int y = 0; y < LCD_HEIGHT; y++)
    {
        HAL_SPI_Transmit(&hspi2, line_buf, sizeof(line_buf), 100);
    }

    // 4. 全部发完拉高片选
    LCD_CS_SET();
}

void ST7789_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t fc, uint16_t bc)
{
    if (ch < ' ' || ch > '~') return;
    if (x + 8 > LCD_WIDTH || y + 16 > LCD_HEIGHT) return;

    uint8_t c_idx = ch - ' ';
    uint8_t char_buf[8 * 16 * 2]; // 8x16 像素，每个像素 2 字节 RGB565

    uint16_t ptr = 0;
    for (int i = 0; i < 16; i++)
    {
        uint8_t line = font_8x16[c_idx][i];
        for (int j = 0; j < 8; j++)
        {
            uint16_t pixel_color = (line & (0x80 >> j)) ? fc : bc;
            char_buf[ptr++] = (uint8_t)(pixel_color >> 8);
            char_buf[ptr++] = (uint8_t)(pixel_color & 0xFF);
        }
    }

    /* 设置 8x16 物理窗口并批量写入显存 */
    ST7789_SetWindow(x, y, x + 7, y + 15);

    LCD_DC_CLR();
    LCD_CS_CLR();
    uint8_t cmd = 0x2C;
    HAL_SPI_Transmit(&hspi2, &cmd, 1, 10);

    LCD_DC_SET();
    HAL_SPI_Transmit(&hspi2, char_buf, sizeof(char_buf), 20);
    LCD_CS_SET();
}

/* 显示字符串 */
void ST7789_ShowString(uint16_t x, uint16_t y, const char *str, uint16_t fc, uint16_t bc)
{
    while (*str)
    {
        ST7789_DrawChar(x, y, *str, fc, bc);
        x += 8;
        if (x + 8 > LCD_WIDTH) // 超出一行自动换行
        {
            x = 0;
            y += 16;
        }
        str++;
    }
}


/**
 * @brief  向 ST7789 指定矩形区域批量写入像素点 (供 LVGL 刷新回调 disp_flush 调用)
 */
void ST7789_FillRect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t *color_p)
{
    uint32_t size = (x2 - x1 + 1) * (y2 - y1 + 1);

    // 1. 设置 ST7789 的刷屏窗口
    ST7789_SetWindow(x1, y1, x2, y2);

    // 2. 发送写显存命令 0x2C
    LCD_DC_CLR();
    LCD_CS_CLR();
    uint8_t cmd = 0x2C;
    HAL_SPI_Transmit(&hspi2, &cmd, 1, 10);

    // RGB565 values are stored little-endian in RAM; SPI sends the high byte first.
    LCD_DC_SET();
    uint8_t pixel_buf[240 * 2];
    while (size > 0)
    {
        uint32_t count = size > 240 ? 240 : size;
        for (uint32_t i = 0; i < count; i++)
        {
            pixel_buf[2 * i] = (uint8_t)(color_p[i] >> 8);
            pixel_buf[2 * i + 1] = (uint8_t)color_p[i];
        }
        HAL_SPI_Transmit(&hspi2, pixel_buf, count * 2, 1000);
        color_p += count;
        size -= count;
    }

    // 4. 拉高片选完成本次写入
    LCD_CS_SET();
}
