#ifndef __ESP8266_OTA_H
#define __ESP8266_OTA_H

#include "main.h"
#include "stdbool.h"

/* ============================================================================
 * OTA 模块对外契约（APP 侧）
 * ----------------------------------------------------------------------------
 * 这个头文件只放"跨文件/跨工程"的东西：
 *   1. W25Q64 上两个持久化地址（Bootloader 必须使用同一份地址）；
 *   2. OTA 运行状态枚举（只活在 RAM，给 UI / 任务同步用）；
 *   3. OTA_Info_t（APP 写、Bootloader 读的唯一持久化结构）；
 *   4. APP 侧对外 API。
 *
 * 数据流一句话：
 *   MQTTTask 收到 "upgrade"
 *     -> ESP8266_HTTP_OTA_Download() 把 HTTP 正文写进 W25Q64 暂存区并算 CRC
 *     -> 把 {magic, fw_size, fw_crc32, version} 写到 W25Q64 标志扇区
 *     -> NVIC_SystemReset()
 *     -> Bootloader 读标志、校验、搬运到内部 Flash 0x08010000。
 *
 * W25Q64 地址布局（8MB，24 位地址 0x000000 ~ 0x7FFFFF）：
 *
 *   0x000000 ┌──────────────────────────────┐
 *            │ 未使用                       │
 *   0x0FF000 ├──────────────────────────────┤  sector 255（4KB）
 *            │ OTA 标志区 OTA_Info_t (16B)  │  W25Q64_OTA_FLAG_ADDR
 *   0x100000 ├──────────────────────────────┤  sector 256
 *            │ OTA 固件暂存区               │  W25Q64_OTA_BIN_ADDR
 *            │ 当前只擦 80 个扇区 = 320KB   │  [0x100000, 0x150000)
 *   0x150000 └──────────────────────────────┘
 *
 * 注意两个容易混淆的数字：
 *   - 当前"擦除窗口"是 320KB（ESP8266_HTTP_OTA_Download 里的 80 个扇区）；
 *   - 代码对 Content-Length 的"接受上限"是 960KB（APP 分区大小）。
 *   两者目前并不相等：超过 320KB 的固件会通过大小检查，却写进未擦除区域。
 *   改大固件时必须同步扩大擦除范围，或者把上限收紧到擦除窗口。
 * ========================================================================== */

#define ESP_RX_BUF_SIZE 2048
//w25q64容量8mb
//最小擦除代为为扇区(sector) = 4kb(4096 bytes)
//写入单位:页(page) = 256 bytes

/* 标志扇区：255 号扇区（0x0FF000 / 0x1000 = 255），4KB 对齐。
 * 放在暂存区之前、APP 内部 Flash 之外：
 *   - Bootloader 擦 APP 区时不会碰它；
 *   - APP 升级失败时旧 APP 仍能正常跑，标志也不会被误擦。 */
#define W25Q64_OTA_FLAG_ADDR    0x000FF000  //255号扇区:存放ota标志与元信息

/* 固件暂存区：从 1MB 边界开始（sector 256）。
 * 当前只擦 80 个扇区（320KB），实际固件必须 <= 320KB 才能完整落盘。 */
#define W25Q64_OTA_BIN_ADDR     0x00100000  //1MB 边界:专门存放下载的新固件 .bin

/* 升级请求魔数：只有标志扇区里读到这个值，Bootloader 才认为"存在一次升级请求"。
 * APP 成功写完标志后复位；Bootloader 搬运并校验通过后必须清掉它（擦扇区或写回无效值），
 * 否则每次上电都会重复搬运同一份固件。 */
#define OTA_MAGIC_NUMBER        0x5AA5F00F  //Bootloader 用它判断是否存在合法的升级请求

/* 运行态枚举：只用于 UI 显示和任务间同步，不是持久化格式。
 *
 * 重要：枚举值的大小没有先后语义！
 *   ERROR(=5) 比 WIFI_CONNECTED(=2) 大，所以禁止写
 *       while (g_ota_state < OTA_STATE_WIFI_CONNECTED)
 *   这种比较会把失败态当成"已就绪"放行。历史 bug 就是这么来的。
 *   一律用 == / != 精确比较。
 *
 * 合法迁移（详见 freertos.c 顶部与 docs/OTA升级模块分析.md）：
 *   IDLE -> WIFI_CONNECTING -> WIFI_CONNECTED -> DOWNLOADING
 *                              ^         |
 *                              |         +-> 成功: 写标志 + 复位（不返回）
 *                              |         +-> 失败: ERROR -> ESP8266_Init() -> WIFI_CONNECTED
 *                              +-- WiFi 失败: ERROR -> 5s -> WIFI_CONNECTING
 */
typedef enum {
    OTA_STATE_IDLE = 0,         // 空闲状态（上电后、NetworkTask 启动前）
    OTA_STATE_WIFI_CONNECTING,  // Wi-Fi 正在连接中（含失败后的重试）
    OTA_STATE_WIFI_CONNECTED,   // Wi-Fi 连接成功，已获取 IP（不代表 MQTT 在线）
    OTA_STATE_DOWNLOADING,      // 正在从 HTTP 服务器拉取固件 bin（MQTT 保活暂停）
    OTA_STATE_VERIFY_OK,        // 保留态；当前 APP 不赋值，真正校验在 Bootloader
    OTA_STATE_ERROR             // 发生错误（WiFi 失败 / OTA 失败）
} OTA_State_t;


/* OTA 标志结构：APP 与 Bootloader 的"共同 ABI"。
 *
 * 布局固定为 4 个 uint32_t（ARM EABI：16 字节、4 字节对齐、无填充、小端）：
 *   偏移 0x00  magic     必须等于 OTA_MAGIC_NUMBER
 *   偏移 0x04  fw_size   固件字节数；APP 成功时必须 == total_fw_size == written_bytes
 *   偏移 0x08  fw_crc32  对暂存区 [BIN_ADDR, BIN_ADDR+fw_size) 的最终 CRC32
 *                        （初值 0xFFFFFFFF，算完再异或 0xFFFFFFFF）
 *   偏移 0x0C  version   人看的版本标识（如 300），不参与校验
 *
 * 字段之间是"同一份固件"的三个视角，不能各自独立改：
 *   fw_size 决定 Bootloader 擦多少/搬多少；
 *   fw_crc32 决定"搬完的内容是否和 APP 写下去的一致"；
 *   magic 决定"这次上电到底要不要搬"；
 *   version 只用于打印/屏幕，不参与决策。
 *
 * 修改本结构（字段、顺序、类型）必须同时改独立 Bootloader 工程；
 * 否则 Bootloader 会把 fw_size 读成 crc 之类，直接拒绝或搬错。
 */
typedef struct
{
    uint32_t magic;      /* OTA_MAGIC_NUMBER */
    uint32_t fw_size;    /* 固件字节数（不含 HTTP 头，不含 +IPD 帧头） */
    uint32_t fw_crc32;   /* 固件正文的最终 CRC32 */
    uint32_t version;    /* 版本标识，如 300 = v3.0 */
}OTA_Info_t;

/* 串口接收仓库：[0, esp_rx_len) 是尚未消费的数据。
 * 生产者：USART3 中断（HAL_UART_RxCpltCallback）只往尾部追加；
 * 消费者：MQTTTask / OTA 解析器只从头部通过 OTA_Consume() 移除。
 * 它不是以 '\0' 结尾的字符串；按内容查找必须带长度（见 OTA_BufFind）。 */
extern uint8_t esp_rx_buf[ESP_RX_BUF_SIZE];

/* USART3 出错次数（ORE/NE/FE/PE）。涨了说明曾经溢出过，
 * 溢出会让 HAL abort 接收，必须靠 HAL_UART_ErrorCallback 重新挂上。
 * 该值只增不减，用于诊断，不参与 OTA 控制流。 */
extern volatile uint32_t g_uart3_err_cnt;


/* 清空接收缓冲（memset + esp_rx_len=0）。
 * 注意：内部没有临界区；调用点应在"准备发新 AT 指令、旧数据不再需要"时使用。 */
void ESP_8266_ClearBuf(void);

/* 返回当前未消费字节数（0..2046）。
 * 供 MQTTTask 判断缓冲是否接近上限；不要把它当"本次收到多少"用。 */
uint16_t ESP8266_GetRxLen(void);

/* 初始化 USART3 接收：清缓冲、挂单字节中断、发 "+++" 退出可能的透传、AT/ATE0。 */
void ESP8266_Init(void);

/* 发送 AT 指令并等待 ack 子串。
 * - 进入函数先清空接收缓冲，因此不能用于"数据已经开始回流"的场景；
 * - ack == NULL 表示只发不等（用于 +MQTTxxx 这类异步事件）；
 * - 查找使用 OTA_BufFind（带长度），避免缓冲满时 strstr 越界。 */
bool ESP8266_SendCmd(const char *cmd, const char *ack, uint32_t timeout_ms);

/* 连接 WiFi（CWMODE=1 -> CWJAP），成功返回 true。
 * 判据用 "OK" 而不是 "WIFI GOT IP"：v2.x 自动重连时只回 OK，不重复打印 GOT IP。 */
bool ESP8266_JoinAP(const char *ssid, const char *pwd);

/* 通过 HTTP GET 下载固件到 W25Q64，并写 OTA 标志。
 *
 * 调用上下文：
 *   - 必须在 MQTTTask 内同步调用（本工程靠"占住 MQTTTask"获得 USART3 独占）；
 *   - 函数会阻塞数秒到数十秒，期间该任务不做 MQTT 保活；
 *   - 成功路径写标志后 NVIC_SystemReset()，正常情况下不返回；
 *   - 失败路径返回 false，且不写标志，调用方负责 ESP8266_Init() + 重连 MQTT。
 *
 * 成功后保证：
 *   written_bytes == total_fw_size == ota_info.fw_size；
 *   ota_info.fw_crc32 == 对 [BIN_ADDR, BIN_ADDR+fw_size) 的最终 CRC32；
 *   ota_info.magic == OTA_MAGIC_NUMBER。
 */
bool ESP8266_HTTP_OTA_Download(const char *server_ip, uint16_t port, const char *file_path);


#endif
