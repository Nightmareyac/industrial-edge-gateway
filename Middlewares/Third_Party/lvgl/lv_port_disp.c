#include "lv_port_disp.h"
#include "lvgl.h"
#include "st7735.h"

/* 屏幕实际分辨率 (ST7735 / 1.6寸 128x160 竖屏) */
#define MY_DISP_HOR_RES    128
#define MY_DISP_VER_RES    160

/* 分配 30 行高度的渲染单缓冲区 (128 * 30 * 2 = 7.5 KB 内存) */
#define DISP_BUF_LINES     30
static lv_color_t buf_1[MY_DISP_HOR_RES * DISP_BUF_LINES];

static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);

void lv_port_disp_init(void)
{
    /* 屏幕硬件初始化统一放在 main.c 的 ST7735_Init()，此处只注册 LVGL 显示驱动 */

    /* 1. 初始化显存缓冲区描述符 */
    static lv_disp_draw_buf_t draw_buf_dsc;
    lv_disp_draw_buf_init(&draw_buf_dsc, buf_1, NULL, MY_DISP_HOR_RES * DISP_BUF_LINES);

    /* 2. 初始化并注册显示驱动 */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res = MY_DISP_HOR_RES;
    disp_drv.ver_res = MY_DISP_VER_RES;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf_dsc;

    lv_disp_drv_register(&disp_drv);
}

/* 核心刷新回调：LVGL 渲染完成后通知底层硬件刷屏 */
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    /* 调用 ST7735 批量刷点接口 */
    ST7735_FillRect(area->x1, area->y1, area->x2, area->y2, (uint16_t *)color_p);

    /* 必须调用此函数通知 LVGL 本次刷新完成 */
    lv_disp_flush_ready(disp_drv);
}
