#include "st7735.h"
#include "font.h"

/* 使用硬件 SPI2：PB13 = SPI2_SCK(SCL)，PB15 = SPI2_MOSI(SDA) */
extern SPI_HandleTypeDef hspi2;

/* ============================ 底层：SPI2 收发 ============================ */

static void ST7735_WriteCmd(uint8_t cmd)
{
    LCD_DC_CLR();
    LCD_CS_CLR();
    HAL_SPI_Transmit(&hspi2, &cmd, 1, 10);
    LCD_CS_SET();
}

static void ST7735_WriteData(uint8_t data)
{
    LCD_DC_SET();
    LCD_CS_CLR();
    HAL_SPI_Transmit(&hspi2, &data, 1, 10);
    LCD_CS_SET();
}

static void ST7735_WriteData16(uint16_t data)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(data >> 8);
    buf[1] = (uint8_t)(data & 0xFF);
    LCD_DC_SET();
    LCD_CS_CLR();
    HAL_SPI_Transmit(&hspi2, buf, 2, 10);
    LCD_CS_SET();
}

/**
 * @brief 设置显示窗口（只发 0x2A/0x2B，0x2C 由写显存函数在同一次片选中发出）
 */
static void ST7735_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    ST7735_WriteCmd(0x2A);          /* 列地址 */
    ST7735_WriteData16(x0);
    ST7735_WriteData16(x1);

    ST7735_WriteCmd(0x2B);          /* 行地址 */
    ST7735_WriteData16(y0);
    ST7735_WriteData16(y1);
}

/**
 * @brief 发 0x2C 开启显存写入。调用后 CS 保持低电平，
 *        必须由调用者用 LCD_CS_SET() 收尾 —— 保证 0x2C 与像素数据同处一次片选。
 */
static void ST7735_BeginRamWrite(void)
{
    uint8_t cmd = 0x2C;
    LCD_DC_CLR();
    LCD_CS_CLR();
    HAL_SPI_Transmit(&hspi2, &cmd, 1, 10);
    LCD_DC_SET();
}

/**
 * @brief 在已设好的窗口内写 count 个像素，调用者保证 pix 长度足够
 */
static void ST7735_WritePixels(const uint16_t *pix, uint32_t count)
{
    uint8_t buf[LCD_WIDTH * 2];

    while (count > 0)
    {
        uint32_t n = (count > LCD_WIDTH) ? LCD_WIDTH : count;
        for (uint32_t i = 0; i < n; i++)
        {
            buf[2 * i]     = (uint8_t)(pix[i] >> 8);   /* RGB565 高字节先发 */
            buf[2 * i + 1] = (uint8_t)(pix[i] & 0xFF);
        }
        HAL_SPI_Transmit(&hspi2, buf, n * 2, 1000);
        pix   += n;
        count -= n;
    }
}

/**
 * @brief 用同一颜色连续写 count 个像素（不需要调用者准备长缓冲）
 */
static void ST7735_WriteColorRepeated(uint16_t color, uint32_t count)
{
    uint8_t buf[LCD_WIDTH * 2];

    for (uint32_t i = 0; i < LCD_WIDTH; i++)
    {
        buf[2 * i]     = (uint8_t)(color >> 8);
        buf[2 * i + 1] = (uint8_t)(color & 0xFF);
    }

    while (count > 0)
    {
        uint32_t n = (count > LCD_WIDTH) ? LCD_WIDTH : count;
        HAL_SPI_Transmit(&hspi2, buf, n * 2, 1000);
        count -= n;
    }
}

/* ============================ 初始化 ============================ */

/**
 * @brief 初始化 GPIO：CS=PB12, RST=PB14, BLK=PB1, DC=PC5
 *        SCL/SDA 由 CubeMX 的 SPI2 复用接管，此处不碰 PB13/PB15。
 */
static void ST7735_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PB12(CS), PB14(RST), PB1(BLK) 推挽输出 */
    GPIO_InitStruct.Pin   = GPIO_PIN_12 | GPIO_PIN_14 | GPIO_PIN_1;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* PC5(DC) 推挽输出 */
    GPIO_InitStruct.Pin = GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    LCD_CS_SET();
    LCD_DC_SET();
    LCD_BLK_SET();      /* 点亮背光 */
}

/**
 * @brief ST7735 初始化：硬件复位 + ST7735S 初始化序列
 */
void ST7735_Init(void)
{
    ST7735_GPIO_Init();

    /* 硬件复位脉冲：PB14 -> 屏 RST */
    LCD_RST_SET();
    HAL_Delay(10);
    LCD_RST_CLR();
    HAL_Delay(20);
    LCD_RST_SET();
    HAL_Delay(120);

    ST7735_WriteCmd(0x11); /* Sleep out */
    HAL_Delay(120);

    /* ST7735S Frame Rate */
    ST7735_WriteCmd(0xB1);
    ST7735_WriteData(0x05); ST7735_WriteData(0x3C); ST7735_WriteData(0x3C);
    ST7735_WriteCmd(0xB2);
    ST7735_WriteData(0x05); ST7735_WriteData(0x3C); ST7735_WriteData(0x3C);
    ST7735_WriteCmd(0xB3);
    ST7735_WriteData(0x05); ST7735_WriteData(0x3C); ST7735_WriteData(0x3C);
    ST7735_WriteData(0x05); ST7735_WriteData(0x3C); ST7735_WriteData(0x3C);

    ST7735_WriteCmd(0xB4); /* Dot inversion */
    ST7735_WriteData(0x03);

    /* ST7735S Power Sequence */
    ST7735_WriteCmd(0xC0);
    ST7735_WriteData(0x28); ST7735_WriteData(0x08); ST7735_WriteData(0x04);
    ST7735_WriteCmd(0xC1);
    ST7735_WriteData(0xC0);
    ST7735_WriteCmd(0xC2);
    ST7735_WriteData(0x0D); ST7735_WriteData(0x00);
    ST7735_WriteCmd(0xC3);
    ST7735_WriteData(0x8D); ST7735_WriteData(0x2A);
    ST7735_WriteCmd(0xC4);
    ST7735_WriteData(0x8D); ST7735_WriteData(0xEE);

    ST7735_WriteCmd(0xC5); /* VCOM */
    ST7735_WriteData(0x1A);

    ST7735_WriteCmd(0x36); /* 显示方向 MADCTL */
#if (LCD_HORIZONTAL == 0)
    ST7735_WriteData(0x00);
#elif (LCD_HORIZONTAL == 1)
    ST7735_WriteData(0xC0);
#elif (LCD_HORIZONTAL == 2)
    ST7735_WriteData(0x70);
#else
    ST7735_WriteData(0xA0);
#endif

    /* ST7735S Gamma Sequence */
    ST7735_WriteCmd(0xE0);
    ST7735_WriteData(0x04); ST7735_WriteData(0x22); ST7735_WriteData(0x07); ST7735_WriteData(0x0A);
    ST7735_WriteData(0x2E); ST7735_WriteData(0x30); ST7735_WriteData(0x25); ST7735_WriteData(0x2A);
    ST7735_WriteData(0x28); ST7735_WriteData(0x26); ST7735_WriteData(0x2E); ST7735_WriteData(0x3A);
    ST7735_WriteData(0x00); ST7735_WriteData(0x01); ST7735_WriteData(0x03); ST7735_WriteData(0x13);

    ST7735_WriteCmd(0xE1);
    ST7735_WriteData(0x04); ST7735_WriteData(0x16); ST7735_WriteData(0x06); ST7735_WriteData(0x0D);
    ST7735_WriteData(0x2D); ST7735_WriteData(0x26); ST7735_WriteData(0x23); ST7735_WriteData(0x27);
    ST7735_WriteData(0x27); ST7735_WriteData(0x25); ST7735_WriteData(0x2D); ST7735_WriteData(0x3B);
    ST7735_WriteData(0x00); ST7735_WriteData(0x01); ST7735_WriteData(0x04); ST7735_WriteData(0x13);

    ST7735_WriteCmd(0x3A); /* 65K 色，RGB565 */
    ST7735_WriteData(0x05);

    ST7735_WriteCmd(0x29); /* Display on */
    HAL_Delay(50);
}

/* ============================ 绘图 API ============================ */

void ST7735_Clear(uint16_t color)
{
    ST7735_SetWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    ST7735_BeginRamWrite();
    ST7735_WriteColorRepeated(color, (uint32_t)LCD_WIDTH * LCD_HEIGHT);
    LCD_CS_SET();
}

void ST7735_DrawChar(uint16_t x, uint16_t y, char ch, uint16_t fc, uint16_t bc)
{
    if (ch < ' ' || ch > '~') return;
    if (x + 8 > LCD_WIDTH || y + 16 > LCD_HEIGHT) return;

    uint8_t  c_idx = ch - ' ';
    uint16_t buf[8 * 16];
    uint16_t ptr = 0;

    for (int i = 0; i < 16; i++)
    {
        uint8_t line = font_8x16[c_idx][i];
        for (int j = 0; j < 8; j++)
        {
            buf[ptr++] = (line & (0x80 >> j)) ? fc : bc;
        }
    }

    ST7735_SetWindow(x, y, x + 7, y + 15);
    ST7735_BeginRamWrite();
    ST7735_WritePixels(buf, 8 * 16);
    LCD_CS_SET();
}

void ST7735_ShowString(uint16_t x, uint16_t y, const char *str, uint16_t fc, uint16_t bc)
{
    while (*str)
    {
        ST7735_DrawChar(x, y, *str, fc, bc);
        x += 8;
        if (x + 8 > LCD_WIDTH)
        {
            x = 0;
            y += 16;
        }
        str++;
    }
}

/**
 * @brief 供 LVGL disp_flush 调用的区域填充函数
 */
void ST7735_FillRect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t *color_p)
{
    uint32_t size = (uint32_t)(x2 - x1 + 1) * (y2 - y1 + 1);

    ST7735_SetWindow(x1, y1, x2, y2);
    ST7735_BeginRamWrite();
    ST7735_WritePixels(color_p, size);
    LCD_CS_SET();
}
