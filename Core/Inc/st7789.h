#ifndef __ST7789_H
#define __ST7789_H

#include "main.h"

#define LCD_WIDTH   240
#define LCD_HEIGHT  240

// 常用 RGB565 颜色
#define WHITE   0xFFFF
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define YELLOW  0xFFE0
#define CYAN    0x7FFF
#define GRAY    0x8410

// 引脚控制宏
#define LCD_CS_CLR()   HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET)
#define LCD_CS_SET()   HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET)

#define LCD_DC_CLR()   HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5,  GPIO_PIN_RESET)
#define LCD_DC_SET()   HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5,  GPIO_PIN_SET)

#define LCD_RST_CLR()  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET)
#define LCD_RST_SET()  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET)

#define LCD_BLK_SET()  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1,  GPIO_PIN_SET)

void ST7789_Init(void);
void ST7789_Clear(uint16_t color);
void ST7789_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t fc, uint16_t bc);
void ST7789_ShowString(uint16_t x, uint16_t y, const char *str, uint16_t fc, uint16_t bc);

void ST7789_FillRect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t *color_p);

#endif