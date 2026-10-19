#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/* ==================================================================
 * 版本号 与 OTA 服务配置
 *
 * 这几项原来散落在两个文件里：
 *   屏幕标题文本  -> freertos.c   "Edge Gateway V2.1"
 *   OTA 标志版本  -> esp8266_ota.c  ota_info.version = 300
 *   服务器地址    -> freertos.c   ESP8266_HTTP_OTA_Download("192.168.137.1", 80, ...)
 *
 * 发一个版本要改两三处，漏一处就是"屏幕显示 V3.1、标志里还是 300"，
 * 事后对账时很难自证。集中到这里，发布时只改这一段。
 * ================================================================== */

/* 屏幕标题显示的版本号。
 * 屏幕宽 128px、10 号字，一行最多放 17 个字符 ——
 * "Edge Gateway V3.0" 正好 17，再长会横向溢出。 */
#define APP_VERSION_STR         "V3.0"

/* 写进 W25Q64 OTA 标志的版本号，格式：主版本 * 100 + 次版本，V3.0 -> 300。
 * 纯标识用途，Bootloader 不拿它做判断，只用于事后确认刷的是哪一版 */
#define APP_VERSION_NUM         300

/* ---- OTA 固件服务器 ---- */

/* 192.168.137.1 是 Windows 移动热点的网关地址，也就是本机 PC。
 * 别写 192.168.137.13 —— 那是热点 DHCP 分给 ESP8266 自己的地址，
 * 等于让模块去连自己的 80 端口，必然连接被拒（踩过） */
#define OTA_SERVER_IP           "192.168.137.1"
#define OTA_SERVER_PORT         80

/* 固件路径：**固定不变，不带版本号**。
 *
 * 早期这里写的是 "/A_Project_v3.0.bin"，结果每发一版都要
 * 改这一行 + 重新编译 + 重新烧录基线固件，等于根本没法"推送"。
 *
 * 改成固定名之后，发布只要两步：
 *   1. 服务端覆盖 ota_release/A_Project.bin
 *   2. 云端发 upgrade
 * 设备永远请求同一个 URL，拿到的却是最新那份。
 * 版本差异靠 bin 内嵌的 APP_VERSION_STR 和 OTA 标志里的 version 区分。 */
#define OTA_FIRMWARE_PATH       "/A_Project.bin"

#endif /* __APP_CONFIG_H */
