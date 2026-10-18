#ifndef __ST7735_H
#define __ST7735_H

#include "main.h"

/* 屏幕方向设置：0或1为竖屏(128x160)，2或3为横屏(160x128)
 * 1.6寸 ST7735 屏为竖屏 128x160，取 0 或 1 */
#define LCD_HORIZONTAL  1

#if (LCD_HORIZONTAL == 0 || LCD_HORIZONTAL == 1)
#define LCD_WIDTH   128
#define LCD_HEIGHT  160
#else
#define LCD_WIDTH   160
#define LCD_HEIGHT  128
#endif

// 常用 RGB565 颜色
#define WHITE   0xFFFF
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define YELLOW  0xFFE0
#define CYAN    0x7FFF
#define GRAY    0x8410

/* ================= F407 板载 TFT 接口 P2 (Header 8) 引脚定义 =================
 * P2-1  3V3
 * P2-2  GND
 * P2-3  PB15  ->  SPI2_MOSI (SDA)
 * P2-4  PB13  ->  SPI2_SCK  (SCL)
 * P2-5  PB12  ->  CS
 * P2-6  PB14  ->  RST
 * P2-7  PC5   ->  DC (RS)
 * P2-8  PB1   ->  BLK (背光)
 *
 * SPI 走硬件 SPI2 (hspi2)：PB13/PB15 已由 CubeMX 配为 SPI2 复用，
 * 本驱动不再软件模拟时序，直接调用 HAL_SPI_Transmit。
 * ============================================================================== */
#define LCD_CS_CLR()    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET)
#define LCD_CS_SET()    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET)

#define LCD_DC_CLR()    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5,  GPIO_PIN_RESET)
#define LCD_DC_SET()    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5,  GPIO_PIN_SET)

#define LCD_RST_CLR()   HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET)
#define LCD_RST_SET()   HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET)

#define LCD_BLK_CLR()   HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1,  GPIO_PIN_RESET)
#define LCD_BLK_SET()   HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1,  GPIO_PIN_SET)

void ST7735_Init(void);
void ST7735_Clear(uint16_t color);
void ST7735_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t fc, uint16_t bc);
void ST7735_ShowString(uint16_t x, uint16_t y, const char *str, uint16_t fc, uint16_t bc);
void ST7735_FillRect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t *color_p);

#endif /* __ST7735_H */
