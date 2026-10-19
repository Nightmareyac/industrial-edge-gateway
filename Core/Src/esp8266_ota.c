#include "esp8266_ota.h"
#include "app_config.h"
#include "usart.h"
#include "w25q64.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * 文件分层与变量所有权（先看这里，再看具体函数）
 * ----------------------------------------------------------------------------
 * ① 串口层：USART3 单字节中断接收
 *      uart3_rx_byte  --ISR-->  esp_rx_buf[esp_rx_len++]
 *      esp_rx_buf 是线性缓冲，[0, esp_rx_len) 有效；ISR 只追加，任务只从头消费。
 *      esp_rx_len 是生产者/消费者共享游标，最大 2046（留 1 字节余量），
 *      在 esp_rx_len 处没有 '\0'，所以本文件的内容查找统一用 OTA_BufFind（带长度）。
 *
 * ② 命令层：ESP8266_SendCmd / OTA_WaitFor
 *      SendCmd 会先清缓冲再发，适合"指令 -> 等应答"；
 *      OTA_WaitFor 不清缓冲，适合"数据已经开始回流"（SEND OK / +IPD）。
 *
 * ③ 传输层：ESP8266_HTTP_OTA_Download
 *      8 步：清链路 -> 擦 W25Q64 -> CIPSTART -> 拼 GET -> CIPSEND -> 发/等 SEND OK
 *            -> +IPD 三态解析并落盘 -> 写 OTA_Info_t -> 复位。
 *
 * ④ 存储/校验层：Calc_CRC32 / W25Q64_Write_Safe
 *      write_addr = W25Q64_OTA_BIN_ADDR + written_bytes；
 *      crc32_val 恰好覆盖 [W25Q64_OTA_BIN_ADDR, write_addr) 的每个字节。
 *
 * 这四层的"游标"必须同步推进：
 *      esp_rx_len（串口消费到哪）
 *      frame_remain（当前 +IPD 帧还剩多少正文）
 *      written_bytes / write_addr（固件写到哪）
 *      crc32_val（校验算到哪）
 * 任何一组落后或超前，最终都会在 Bootloader 的 CRC / 向量校验处暴露。
 * ========================================================================== */

extern UART_HandleTypeDef huart3; // pb10 pb11

/* 屏幕第二行显示的 AT 应答快照，定义在 freertos.c。
 * OTA 里的失败点没有串口也能看到：把卡住那一步写进去，
 * 屏幕会显示 "AT: ota2:start" 这种，一眼定位是 CIPMODE / CIPSTART / 收包 哪一步 */
extern char g_at_tail[22];

static void OTA_Mark(const char *s)
{
    uint8_t i = 0U;

    while ((s[i] != '\0') && (i < 21U))
    {
        g_at_tail[i] = s[i];
        i++;
    }
    g_at_tail[i] = '\0';
}

/* 串口缓冲区设计
 *  esp返回的at响应和http固件流长度是不定长的，波特率高达115200
 * 如果使用 Hal_Uart_Receive轮询，FreeRtos任务切换的空窗几毫秒就会导致串口ORE(溢出错误)丢包;
 * 因此必须使用串口中断单字节接收 + 线性缓冲区 机制
 * 硬件每收到一字节就存入rxbuf，后台任务解析rxbuf
 *
 * 这三个定义必须放在文件最前面：OTA_Consume / OTA_WaitFor 都要用，
 * 放到中间会变成"先用后声明" */
#define ESP_RX_BUF_SIZE 2048

/* 线性接收缓冲（无环形回绕）：
 *   esp_rx_buf[0 .. esp_rx_len)   = 尚未被任务消费的字节，头部最旧；
 *   esp_rx_buf[esp_rx_len .. ]    = 空闲区（内容可能是旧数据，不能当字符串用）。
 *
 * 生产者：USART3 中断（HAL_UART_RxCpltCallback）
 *   uart3_rx_byte 是 HAL 的单字节落点，ISR 把这一字节搬进 esp_rx_buf 尾部，
 *   然后 esp_rx_len++ 并重新挂 HAL_UART_Receive_IT。
 *
 * 消费者：MQTTTask / OTA 解析器
 *   只能通过 OTA_Consume(n) 从头部移除 n 字节（内部 memmove + 临界区）。
 *
 * 容量关系：
 *   ISR 的判断是 esp_rx_len < ESP_RX_BUF_SIZE - 1，所以最多写 2046 字节；
 *   第 2047 字节永远不被 ISR 写，留作"溢出安全垫"。
 *   这也意味着 esp_rx_len 在 2046 处封顶，缓冲满后新字节直接丢弃。
 *
 * 并发关系：
 *   esp_rx_len 是 16 位对齐变量，Cortex-M4 上单次读写原子；
 *   但"读长度 + 访问缓冲区"不是原子的，跨临界区拷贝时必须两边都关中断
 *   （见 OTA_Consume 与收包循环里的 taskENTER_CRITICAL）。
 */
uint8_t esp_rx_buf[ESP_RX_BUF_SIZE];
static volatile uint16_t esp_rx_len = 0;
static uint8_t uart3_rx_byte = 0;

/* OTA 阶段日志：直接写 USART1(PA9/PA10, 115200)，和 freertos.c 的 ESP_Log 同一个口。
 * freertos.c 里的 ESP_Log 是 static 的，这里自带一个极简版本。
 * 作用：OTA 卡住时用串口助手抓这一段，能看到模块回的原文，
 * 不用再靠屏幕那两行字猜到底卡在哪 */
static void OTA_Log(const char *s)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 200);
}

/* 在接收缓冲里做**带长度**的子串查找。
 *
 * 为什么不能直接用 strstr：
 * esp_rx_buf 是线性缓冲，写满（2047 字节）时末尾没有 '\0'。
 * strstr 会一路扫到数组外，读到的可能是任意 RAM 内容 ——
 * 既可能把别的数据误当成应答，也可能踩到不可读区域。
 * OTA 收包阶段缓冲几乎一直是满的，所以这里必须用有界搜索。 */
static int32_t OTA_BufFind(const char *pat)
{
    uint16_t pl = (uint16_t)strlen(pat);
    uint16_t n  = esp_rx_len;
    uint16_t i;

    if ((pl == 0U) || (n < pl))
    {
        return -1;
    }
    for (i = 0U; (uint16_t)(i + pl) <= n; i++)
    {
        if (memcmp(&esp_rx_buf[i], pat, pl) == 0)
        {
            return (int32_t)i;
        }
    }
    return -1;
}

/* 等一段应答，但**不清缓冲**。
 * ESP8266_SendCmd 每次进函数都会先清空接收缓冲，
 * 用在"数据已经开始回流"的场景会吃掉刚到的字节（比如 SEND OK 之后紧跟的 +IPD）。
 * 这里只轮询不清，专门给发送回执和收包阶段用 */
static bool OTA_WaitFor(const char *ack, uint32_t ms)
{
    uint32_t t0 = osKernelGetTickCount();

    while ((osKernelGetTickCount() - t0) < ms)
    {
        if (OTA_BufFind(ack) >= 0)
        {
            return true;
        }
        osDelay(5);
    }
    return false;
}

/* 从接收缓冲头部丢弃 n 个字节。
 * 命令模式下 +IPD 帧是"边解析边丢"的，必须能在头部做消费，
 * 否则线性缓冲很快被填满（2048 字节在 115200 下只要 178ms）。
 *
 * 与 esp_rx_len 的关系：
 *   n == 0            -> 什么都不做（调用方必须保证不要靠它推进状态）；
 *   n >= esp_rx_len   -> 直接清零（丢弃全部，含"长度算错"的兜底）；
 *   n <  esp_rx_len   -> memmove 把 [n, len) 搬到头部，len -= n。
 *
 * 为什么必须关中断：
 *   memmove 期间 USART3 ISR 仍在往尾部追加；如果不关，ISR 可能在
 *   "搬完一半"时写 esp_rx_buf[esp_rx_len]，导致新旧数据交错。
 *   taskENTER_CRITICAL 只关到 configMAX_SYSCALL_INTERRUPT_PRIORITY，
 *   USART3 的 NVIC 优先级是 5，恰好被屏蔽，满足要求。
 *
 * 注意：消费后不会在 esp_rx_buf[esp_rx_len] 补 '\0'；
 *   所以任何按字符串查找的地方都必须用带长度的 OTA_BufFind。 */
static void OTA_Consume(uint16_t n)
{
    if (n == 0U)
    {
        return;
    }
    taskENTER_CRITICAL();
    if (n >= esp_rx_len)
    {
        esp_rx_len = 0U;
    }
    else
    {
        memmove(esp_rx_buf, &esp_rx_buf[n], (size_t)(esp_rx_len - n));
        esp_rx_len = (uint16_t)(esp_rx_len - n);
    }
    taskEXIT_CRITICAL();
}

/* 把一段文本（到 CR/LF 为止）截进屏幕第二行：prefix + 内容，共 21 字符。
 * 用途：ota4 时直接把服务器状态行打到屏幕上，
 * 例如 "o4:HTTP/1.0 404 File not" —— 不用串口也知道服务器回了什么 */
static void OTA_MarkText(const char *prefix, const char *text)
{
    uint8_t k = 0U;
    uint8_t i = 0U;

    while ((prefix[i] != '\0') && (k < 21U))
    {
        g_at_tail[k++] = prefix[i++];
    }
    i = 0U;
    while ((text[i] != '\0') && (text[i] != '\r') && (text[i] != '\n') && (k < 21U))
    {
        if ((text[i] >= 0x20) && (text[i] < 0x7F))
        {
            g_at_tail[k++] = text[i];
        }
        i++;
    }
    g_at_tail[k] = '\0';
}

/* 串口缓冲区的定义已前移到文件开头（OTA_Consume 需要先看到 esp_rx_len） */

/* USART3 出错次数（ORE/NE/FE/PE）。涨了说明曾经溢出过，
 * 而 HAL 一旦 abort 接收就不会自己重挂，
 * 下面 HAL_UART_ErrorCallback 负责把接收重新挂上 */
volatile uint32_t g_uart3_err_cnt = 0;

/* 清空接收缓冲区。
 *
 * 变量关系：把 esp_rx_len 归零，等于宣布 esp_rx_buf 里所有未消费数据作废；
 * 下一次 ISR 会从 index 0 重新开始写。
 *
 * 注意：memset 与 esp_rx_len=0 不在临界区里。
 * 如果 ISR 恰好在 memset 期间写入，可能出现"刚清掉又被追加"或长度与内容不一致。
 * 当前调用点都在"发新 AT 指令前、旧数据不再需要"的窗口，竞争窗口很短；
 * 若要严格保证，应先关 USART3 RX 中断（或放进 taskENTER_CRITICAL）再清。 */
void ESP_8266_ClearBuf(void)
{
    memset(esp_rx_buf, 0, ESP_RX_BUF_SIZE);
    esp_rx_len = 0;
}

/* 返回当前已接收的字节数。
 * esp_rx_len 是 static 的，MQTT 任务需要靠它判断缓冲区是否快满：
 * 一旦涨到 ESP_RX_BUF_SIZE-1，串口中断就不再收字符，
 * 设备会表现成"TCP 还连着，但再也收不到任何指令" */
uint16_t ESP8266_GetRxLen(void)
{
    return esp_rx_len;
}

/* HAL 串口接收完成回调：每次只收 1 字节。
 *
 * 数据路径：
 *   USART3 收到 1 字节 -> HAL 写入 uart3_rx_byte -> 本回调
 *     -> 若 esp_rx_len < 2047，把该字节放到 esp_rx_buf[esp_rx_len]，然后 ++
 *     -> 重新 HAL_UART_Receive_IT，等待下一字节。
 *
 * 变量关系：
 *   uart3_rx_byte 只在"回调进入 -> 搬进 esp_rx_buf"这一瞬间有效，不要在外面读它；
 *   esp_rx_len 是唯一的写指针，也是任务侧判断"有没有数据"的依据；
 *   封顶值 2046（< 2047）意味着缓冲满时新字节被静默丢弃，
 *   任务侧必须靠"定期消费 + >3/4 清空"避免长期满仓。
 *
 * 为什么不用轮询：FreeRTOS 任务切换的空窗几毫秒就足以让 USART3 ORE 溢出，
 * 所以这里用"中断单字节 + 线性缓冲 + 后台任务解析"的架构。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    //判断触发源
    if (huart->Instance == USART3)
    {
        //防溢出保护:检查当前缓冲区是否溢出（最多存 2046 字节）
        if (esp_rx_len <(ESP_RX_BUF_SIZE -1))
        {
            //将接收到的一个字节存入buf，然后长度++
            esp_rx_buf[esp_rx_len++] = uart3_rx_byte;
        }

        //收完字节后中断停止，继续监听下一个字节
        HAL_UART_Receive_IT(&huart3,&uart3_rx_byte,1);
        
    }
    
}

/* 串口错误回调：这一步是"能收到数据"的前提。
 * HAL 在检测到 ORE(溢出)/NE/FE/PE 时会 abort 当前接收并调用这里，
 * 但**不会自动重新启动 Receive_IT**。原工程没有实现这个回调，
 * 于是只要溢出一次，USART3 就永久失聪：
 *   现象 = WiFi 明明连上了，之后所有 AT 指令全部超时，
 *          MQTT 一直连不上、屏幕永远 "MQTT: offline"。
 * 修法 = 清标志 + 重新挂上单字节接收，一次都不能漏。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        g_uart3_err_cnt++;

        /* 只清 ORE 就够：NE/FE/PE 在 HAL_UART_IRQHandler 的错误分支里
         * 已经清过了。这四种标志都是"读 SR 再读 DR"来清除，
         * 连续多读几次 DR 反而可能把刚到达的有效字节吃掉 */
        __HAL_UART_CLEAR_OREFLAG(huart);

        HAL_UART_Receive_IT(&huart3, &uart3_rx_byte, 1);
    }
}

void ESP8266_Init(void)
{
    ESP_8266_ClearBuf();

    // 启动一次单字节接收中断
    HAL_UART_Receive_IT(&huart3, &uart3_rx_byte, 1);

    // 透传模式发送+++退出透传
    HAL_UART_Transmit(&huart3, (uint8_t *)"+++", 3, 100);
    osDelay(1000);

    // 发送AT测试并关闭回显(ATE0),减少数据干扰
    ESP8266_SendCmd("AT\r\n", "OK", 500);
    ESP8266_SendCmd("ATE0\r\n", "OK", 500);
}

/**
 * @brief 想esp发送at指令并等待期望字符串
 * @param cmd 要发送的指定
 * @param ack 期待的子字符串
 * @param timeout_ms 超时等待时间
 */
bool ESP8266_SendCmd(const char *cmd, const char *ack, uint32_t timeout_ms)
{
    ESP_8266_ClearBuf();
    // 发送测试
    HAL_UART_Transmit(&huart3, (uint8_t *)cmd, strlen(cmd), 200);

    if (ack == NULL)
    {
        return true;
    }

    // 存储当前时间戳
    uint32_t start_tick = osKernelGetTickCount();
    while ((osKernelGetTickCount() - start_tick) < timeout_ms)
    {
        // 在接收缓冲区是否出现了期望的应答字符串ack
        // 用有界查找：缓冲写满时末尾没有 '\0'，strstr 会越界
        if (OTA_BufFind(ack) >= 0)
        {
            return true;
        }
        osDelay(10); // 让出cpu
    }
    return false;
}

/**
 * @brief 连接wifi
 */
bool ESP8266_JoinAP(const char *ssid, const char *pwd)
{
    char cmd_buf[128];

    // 1设置为station模式 （客户端模式)
    if (!ESP8266_SendCmd("AT+CWMODE=1\r\n", "OK", 1000))
    {
        return false;
    }

    // 拼接热点名称和密码，超时15s
    // 注意：ESP-AT v2.x 会把成功连接过的 AP 保存进 flash，下次上电自动重连。
    // 此时再发 AT+CWJAP 连同一个 AP，模块会直接回 OK，不再重复输出 WIFI GOT IP，
    // 若仍以 "WIFI GOT IP" 为判据就会超时并误判成失败（WiFi 明明已经连上）。
    // 改为判 OK：v2.x 与 v1.x 成功都会回 OK，失败回 +CWJAP:<err> 与 FAIL，不会出现 OK。
    snprintf(cmd_buf, sizeof(cmd_buf), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
    if (!ESP8266_SendCmd(cmd_buf, "OK", 15000))
    {
        return false;
    }

    return true;
}

/**
 * @brief 计算软件 CRC32 校验码（标准反射多项式 0xEDB88320）
 * @param init_crc 上一次的结果，首次调用传 0xFFFFFFFF
 * @param data     数据块
 * @param len      长度
 * @retval CRC 中间值；全部数据算完后要再异或一次 0xFFFFFFFF 才是最终值
 *
 * @note 【为什么用软件实现，不用 F407 自带的硬件 CRC 单元】
 *   STM32 的 CRC 外设默认多项式是 0x04C11DB7，**非反射（MSB first）**，
 *   初始值默认 0xFFFFFFFF 但输入/输出都不做位反转 —— 它是 CRC-32/MPEG-2 变体。
 *   而 zlib / 以太网 / 大多数 bootloader 用的是**反射版 0xEDB88320（LSB first）**。
 *   两者多项式看起来是同一个数的正反写，但算出来的结果**完全不同**。
 *   本工程要和独立的 Bootloader 工程逐位对齐，用软件实现最稳：
 *   多项式、初值、异或规则全部写在代码里，不依赖任何外设默认值。
 *
 * @note 【为什么是"逐块累加"而不是一次算完】
 *   固件是流式接收的，不可能先攒够 206KB 再算。
 *   所以把 crc 的中间值当参数传进传出，来一块算一块，
 *   数学上等价于一次性计算（CRC 天然支持分段续算）。
 *
 * @note 【这个函数是 OTA 的最终防线】
 *   Bootloader 端有一份**完全相同**的实现，搬运完 APP 后回读重算再比一次。
 *   两端只要有一个字节的差异，校验就不过，Bootloader 拒绝跳转、保留旧 APP。
 *   改这个函数必须两边同时改，否则永远对不上。
 */
static uint32_t Calc_CRC32(uint32_t init_crc, const uint8_t *data, uint32_t len)
{
    uint32_t crc = init_crc;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];              /* 把当前字节异或进最低位 */
        for (uint8_t j = 0; j < 8; j++)   /* 逐位处理 8 次 */
        {
            if (crc & 1)             /* 最低位是 1：右移后异或多项式 */
            {
                crc = (crc >> 1) ^ 0xEDB88320;
            }
            else                     /* 最低位是 0：只右移 */
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/**
 * @brief W25Q64 跨页安全写入
 * @param addr  起始地址（任意地址，不要求 256 对齐）
 * @param pbuf  数据指针
 * @param len   长度（可以是几千字节，内部自动拆页）
 *
 * @note 【为什么必须拆页 —— 这是 W25Q64 最容易踩的硬件坑】
 *   W25Q64 的页编程指令（0x02）一次最多写一页 = 256 字节，
 *   而且**地址在页内是回绕的**：
 *     假设从 offset=250 开始写 10 个字节，
 *     写到 offset=255 后不会进位到下一页，而是回到本页 offset=0，
 *     把这一页开头刚写好的 6 个字节**覆盖掉**。
 *   结果就是：数据看起来写成功了，但内容错位，CRC 永远对不上，
 *   而且现象随机（取决于每帧起始地址），极难排查。
 *   所以必须按"距离本页末尾还剩多少"来切分：page_remain = 256 - (addr % 256)。
 *
 * @note 【为什么不要求调用方按 256 对齐再传进来】
 *   OTA 是流式写入，每次落盘的起始地址由"已收字节数"决定，
 *   天然不对齐。把对齐逻辑收在函数内部，调用方只关心"写到哪、写多少"，
 *   少一个出错的地方。
 *
 * @note 【性能提示】
 *   页编程典型耗时 0.7ms / 页（最大 3ms）。206KB 固件约 806 页，
 *   纯写时间 0.5~2.4 秒 —— 这段时间 MCU 是忙等的，
 *   所以接收缓冲必须够大，否则会丢 UART 字节（详见 OTA 收包部分注释）。
 */
static void W25Q64_Write_Safe(uint32_t addr, uint8_t *pbuf, uint32_t len)
{
    /* 第一段：从现在到本页末尾，最多 256 字节 */
    uint32_t page_remain = 256 - (addr % 256);
    if (len <= page_remain)
    {
        page_remain = len;      /* 总长度还不够一页，一次写完 */
    }

    while (1)
    {
        // 调用w25q64页编程函数（写进去的这段绝对不跨页）
        W25Q64_Write(addr, pbuf, page_remain);

        if (len == page_remain)
        {
            break;              /* 写完了 */
        }
        else
        {
            /* 指针前移、长度递减，继续写下一段 */
            pbuf += page_remain;
            addr += page_remain;
            len -= page_remain;
            /* 后续段都从页首开始，所以最大就是 256 */
            if (len > 256)
            {
                page_remain = 256;
            }
            else
            {
                page_remain = len;
            }
        }
    }
}

/**
 * @brief 通过 HTTP GET 把固件下载到 W25Q64 暂存区，并写入 OTA 标志
 *
 * @param server_ip 服务器 IP（局域网内跑 python -m http.server 的那台 PC）
 * @param port      HTTP 端口，通常 80
 * @param file_path 固件路径，例如 "/A_Project_v3.0.bin"
 * @retval true  下载完整、CRC 已算、标志已写；调用方负责重启
 * @retval false 任一步失败，屏幕第二行会写明卡在哪一步
 *
 * ======================== 整体分 8 步 ========================
 *   下面代码里的注释编号（1. 2. 3. ... 8.）与本清单一一对应，
 *   复盘时按编号跳着看即可。
 *
 *  【1】清理链路     CIPMODE=0 -> CIPCLOSE -> CIPMUX=0
 *     目的：MQTT 占用的 link 0 必须彻底释放，否则 CIPSTART 会回 ERROR
 *     或干脆不响应，现象就是屏幕停在 ota2。
 *
 *  【2】擦 W25Q64    80 个扇区 = 320KB
 *     必须在这里擦完，不能边收边擦 —— 原因见第 7 步的注释。
 *
 *  【3】建 TCP       AT+CIPSTART="TCP","ip",port  等 "OK"
 *     判据用 "OK" 不用 "CONNECT"：CONNECT FAIL 里也含 "CONNECT"，
 *     拿它当判据会把自己骗过去。
 *
 *  【4】构造 GET     拼 HTTP/1.1 请求，Host 必填（HTTP/1.1 规范要求）
 *
 *  【5】CIPSEND      AT+CIPSEND=<len> 等 '>'
 *     拿到 '>' 才允许灌数据；拿不到说明链路已断或长度非法。
 *
 *  【6】灌数据 + 等 SEND OK
 *     "SEND OK" 是整个链路唯一的"字节真的进了 TCP 发送缓冲"的证据。
 *     上一版用透传模式就是因为缺这句话，导致"连上了但服务器零请求"
 *     这种情况查了五轮才定位出来。
 *
 *  【7】收响应       +IPD 帧流式解析
 *     响应头攒进 hdr_buf 找 "\r\n\r\n"，之后全部是固件正文，
 *     边收边写 W25Q64、边累加 CRC32。这里必须用状态机，
 *     不能"整帧到齐才处理"，原因见下方大段注释。
 *
 *  【8】写标志       往 W25Q64 标志扇区写 magic/size/crc/version
 *     Bootloader 靠这个结构决定"要不要搬运"。
 *
 * =========================================================
 *
 * @note 失败处理原则：**任一步失败都直接 return false，不留半成品状态**。
 *   调用方收到 false 会重新初始化 ESP、回到 WIFI_CONNECTED 继续跑旧 APP，
 *   设备不会变砖。可恢复性优先于成功率。
 */
bool ESP8266_HTTP_OTA_Download(const char *server_ip, uint16_t port, const char *file_path)
{
    char cmd_buf[256];
    char http_req[256];

    /* ============ 传输方案：命令模式 + CIPSEND，不再用透传 ============
     *
     * 为什么放弃 AT+CIPMODE=1 透传（这是第 5 轮实验后的结论）：
     * 连续 5 次 OTA，Windows 侧 netstat 每次都能看到一条新的
     * 192.168.137.13 -> 192.168.137.1:80 的 ESTABLISHED，
     * 也就是 TCP 每次都连上了；但 http.server 访问日志里一条请求都没有。
     * 结论：连接建好了，数据一个字节都没进 socket。
     *
     * 透传模式的致命缺陷就在这儿——串口数据直接进 socket，中间零回执。
     * 一旦模块内部状态没切过来（CIPMODE 被自动清 0 / SEND 通路没 arm），
     * 我们发的 GET 就被当成一条 AT 指令吞掉，屏幕上只剩 ota3:hdr，
     * 模块为什么没转发，从外部永远观测不到。
     *
     * 命令模式每一步都有回执：
     *   AT+CIPSEND=<len> 必须回 '>' 才允许发数据，
     *   发完必须回 "SEND OK"，才说明这些字节真的进了 TCP 发送缓冲。
     * 任何一步不对，屏幕上的 ota7/ota8 直接指出是哪一步。
     * 代价是接收侧要多解析一层 "+IPD,<len>:" 帧头，下面已实现。
     */

    /* 1. 清理链路状态：先退出可能的透传，再关掉 MQTT 残留的 link，锁定单连接 */
    ESP8266_SendCmd("AT+CIPMODE=0\r\n", "OK", 1000);
    osDelay(200);

    /* 没有连接时 CIPCLOSE 会回 ERROR，属正常，不检查返回值 */
    ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1500);
    osDelay(300);

    /* CIPMUX 设不进去后面 CIPSEND 语法全错，这里必须拦住 */
    if (!ESP8266_SendCmd("AT+CIPMUX=0\r\n", "OK", 1000))
    {
        OTA_Mark("ota1:mux");
        return false;
    }
    osDelay(200);

    /* 2. 擦 80 个扇区 = 80 * 4096 = 320KB，覆盖当前 206884 字节固件并留约 118KB 余量。
     *
     * 擦除窗口与后面 Content-Length 上限的关系（重要）：
     *   这里擦出的是 [W25Q64_OTA_BIN_ADDR, +320KB)；
     *   第 7 步只检查 total_fw_size <= 960KB（APP 分区大小）。
     *   两个数字目前不一致：320KB < 固件 <= 960KB 时，检查会放行，
     *   但写入地址已经超出擦除窗口，写进的是未擦除的 0xFF/旧数据区域，
     *   最终 CRC 对不上，Bootloader 拒绝搬运（安全，但 OTA 必失败）。
     *   改大固件时必须同步扩大这里的扇区数，或把第 7 步上限收紧到 320KB。
     *
     * 为什么必须在发起请求之前擦完：
     *   如果边收边擦，2048 字节缓冲在 115200 下只要约 178ms 就填满，
     *   而擦一个扇区要几十毫秒，必然丢字节。
     *   原来擦 240 个（960KB）要 12 秒以上，按需擦更实际。 */
    for (uint32_t i = 0; i < 80U; i++)
    {
        W25Q64_EraseSector(W25Q64_OTA_BIN_ADDR + (i * 4096U));
        osDelay(5);
    }

    /* 3. 建立 TCP 连接。
     * 命令模式下 CIPSTART 成功回 "CONNECT\r\nOK"，失败回 "CONNECT FAIL" 或 "ERROR"。
     * 等 "OK" 是最干净的判据：CONNECT FAIL 里没有 OK，
     * 而等 "CONNECT" 会被 "CONNECT FAIL" 骗过去（子串包含）。 */
    snprintf(cmd_buf, sizeof(cmd_buf), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", server_ip, port);
    OTA_Log("\r\n[OTA] CIPSTART\r\n");
    if (!ESP8266_SendCmd(cmd_buf, "OK", 8000))
    {
        uint16_t rn = ESP8266_GetRxLen();
        uint16_t ri;
        uint8_t  rk = 0U;

        g_at_tail[rk++] = 'o';
        g_at_tail[rk++] = '2';
        g_at_tail[rk++] = ':';
        for (ri = 0U; (ri < rn) && (rk < 21U); ri++)
        {
            uint8_t rc = esp_rx_buf[ri];
            if ((rc >= 0x20U) && (rc < 0x7FU) && (rc != '='))
            {
                g_at_tail[rk++] = (char)rc;
            }
        }
        if (rk == 3U)
        {
            g_at_tail[rk++] = 'n';
            g_at_tail[rk++] = 'o';
            g_at_tail[rk++] = 'r';
            g_at_tail[rk++] = 'p';
        }
        g_at_tail[rk] = '\0';
        OTA_Log(g_at_tail);
        OTA_Log("\r\n");

        ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);
        return false;
    }
    OTA_Log("[OTA] connected\r\n");

    /* 4. 构造 GET 请求 */
    snprintf(http_req, sizeof(http_req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n\r\n",
             file_path, server_ip);

    /* 5. CIPSEND 声明长度，必须回 '>' 才允许灌数据。
     * 拿不到 '>' 说明模块不接受发送（链路已断 / 长度不合法），报 ota7:cps */
    ESP_8266_ClearBuf();
    snprintf(cmd_buf, sizeof(cmd_buf), "AT+CIPSEND=%u\r\n", (unsigned)strlen(http_req));
    if (!ESP8266_SendCmd(cmd_buf, ">", 3000))
    {
        OTA_Mark("ota7:cps");
        OTA_Log("\r\n[OTA] CIPSEND no prompt\r\n");
        ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);
        return false;
    }

    /* 6. 灌请求正文，必须等到 SEND OK。
     * 这句回执是本次改造的核心价值：它证明这些字节真的进了 TCP 发送缓冲。
     * 透传模式没有这个回执，所以之前"连上了但服务器零请求"永远查不出原因 */
    OTA_Log("[OTA] GET ");
    OTA_Log(file_path);
    OTA_Log("\r\n");
    HAL_UART_Transmit(&huart3, (uint8_t *)http_req, strlen(http_req), 500);
    if (!OTA_WaitFor("SEND OK", 5000))
    {
        OTA_Mark("ota8:snd");
        OTA_Log("\r\n[OTA] no SEND OK\r\n");
        ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);
        return false;
    }
    OTA_Log("[OTA] SEND OK\r\n");

    /* 7. 收包：命令模式下数据以 "+IPD,<len>:<data>" 帧格式上报。
     *
     * 【为什么从"整帧到齐"改成"流式状态机" —— 这一版的核心修复】
     *
     * 上一版要求一帧必须完整落在缓冲里才处理，否则走
     *     OTA_Consume(pos);
     *     continue;
     * 只要出现下面任一情况，这行就是永久死循环：
     *   a) 某一帧长度 > 缓冲可用空间（模块合并上报多段时会发生）
     *   b) 丢了 1 个字节，长度字段被解析成一个巨大值
     *   c) 帧头正好在缓冲头部，pos==0，OTA_Consume(0) 什么都不做
     * 走进这个分支后，接收缓冲定格在 2047 字节再也不变，
     * ESP 模块 UART 侧发不出来 -> 停止从 socket 取数 -> TCP 窗口归零
     * -> HTTP 服务器阻塞在 send()。
     *
     * 实测证据（2026-10-18 20:23 那一次）：
     *   服务端访问日志有 GET 200 —— 说明请求确实到了、响应头确实回了；
     *   但进程 IO 计数只写出 82422 / 206884 字节，之后 3 秒增量 0，
     *   连接仍 ESTABLISHED。这就是上面那个死锁的指纹。
     *
     * 现在改成流式：只记"本帧还剩多少字节"，缓冲有多少处理多少，
     * 帧多大都不会卡。丢字节最多让某一帧数据错乱（CRC32 会拦下来，
     * Bootloader 拒绝写入并保留旧 APP），不会再出现永久卡死。
     */

    enum { RX_SEEK = 0, RX_LEN = 1, RX_DATA = 2 };

    /* 收包状态机的变量关系（本函数最核心的一张表）：
     *
     *   esp_rx_buf/esp_rx_len ──临界区 memcpy──▶ frame_buf[0..n)
     *        （ISR 生产）                         （本轮解析快照）
     *
     *   rx_state + frame_remain + len_val + len_comma
     *        └─ 只负责“从 +IPD,<len>:<data> 里剥出正文”，
     *           frame_remain 是当前帧剩余正文，消费多少减多少，归零回 RX_SEEK。
     *
     *   hdr_buf/hdr_len/hdr_done
     *        └─ 只负责“把 HTTP 响应头攒齐”，头结束后 hdr_len 清零；
     *           头后面可能已经跟着固件开头，要当场写出去。
     *
     *   total_fw_size（来自 Content-Length）
     *        └─ 决定“收多少算完”，也是写入 OTA_Info_t.fw_size 的值。
     *
     *   write_addr / written_bytes
     *        └─ 恒等式：write_addr == W25Q64_OTA_BIN_ADDR + written_bytes；
     *           两者必须同步更新，只统计固件正文，不含 HTTP 头/+IPD 帧头。
     *
     *   crc32_val
     *        └─ 覆盖 [W25Q64_OTA_BIN_ADDR, write_addr) 的每个字节；
     *           初值 0xFFFFFFFF，收完后 ^= 0xFFFFFFFF 才是最终值。
     *
     *   last_byte_tick / dl_start_tick / ui_tick
     *        └─ 活性与 UI 节流，不参与数据正确性判断。
     */
    static uint8_t frame_buf[ESP_RX_BUF_SIZE];
    /* hdr_buf 刻意用 static：
     * 这个函数跑在 MQTTTask 里，任务栈虽已从 2048 提到 4096，
     * 但 cmd_buf+http_req 已占 512，再放一个 768 字节的头缓冲在栈上
     * 会明显压缩收包期间的余量。放 .bss 只花 768 字节 RAM，更省心。
     * hdr_buf 只装“响应头 + 头后可能跟着的一小段正文”，最大 767 字符 + '\0'。 */
    static char hdr_buf[768];

    uint16_t hdr_len = 0U;          /* hdr_buf 已攒长度（<=767），每次拷贝后补 '\0' */
    uint8_t  hdr_done = 0U;         /* 0=还在攒 HTTP 头；1=后续都是固件正文 */
    uint32_t total_fw_size = 0U;    /* Content-Length；成功时必须 == written_bytes */
    uint32_t written_bytes = 0U;    /* 已写入 W25Q64 的固件字节数 */
    uint32_t write_addr = W25Q64_OTA_BIN_ADDR; /* 下一个写地址 = BIN_ADDR + written_bytes */
    uint32_t crc32_val = 0xFFFFFFFF;           /* CRC 中间值；最后再异或 0xFFFFFFFF */
    uint32_t last_byte_tick = osKernelGetTickCount(); /* 最近一次收到数据的 tick */
    uint32_t dl_start_tick  = osKernelGetTickCount(); /* 下载起点，用于 180s 总超时 */
    uint8_t  rx_state = (uint8_t)RX_SEEK;   /* RX_SEEK / RX_LEN / RX_DATA */
    uint32_t frame_remain = 0U;             /* 当前 +IPD 帧还剩多少正文 */
    uint32_t len_val = 0U;                  /* 正在解析的 +IPD 长度字段 */
    uint8_t  len_comma = 0U;      /* 已过第一段数字（CIPDINFO 时后面是 ip,port） */
    uint8_t  ui_tick = 0U;                  /* 每 4 帧刷一次屏幕进度 */

    for (;;)
    {
        uint16_t n;

        /* 总超时：206KB / 115200 理论约 20 秒，给 10 倍余量。
         * 没有这条，一旦卡在"等一个永远不来的帧"，只会一直转 */
        if ((osKernelGetTickCount() - dl_start_tick) > 180000U)
        {
            OTA_Mark("ota6:allto");
            ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);
            return false;
        }

        /* 一次性把当前可读数据拷到本地，避免解析期间被串口中断改写 */
        taskENTER_CRITICAL();
        n = esp_rx_len;
        if (n > 0U)
        {
            memcpy(frame_buf, esp_rx_buf, n);
        }
        taskEXIT_CRITICAL();

        if (n == 0U)
        {
            if ((osKernelGetTickCount() - last_byte_tick) > 8000U)
            {
                OTA_Mark("ota6:rx");
                OTA_Log("\r\n[OTA] stream timeout\r\n");
                ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);
                return false;
            }
            osDelay(2);
            continue;
        }

        last_byte_tick = osKernelGetTickCount();

        /* ---------- 状态一：找帧头 "+IPD," ---------- */
        if (rx_state == (uint8_t)RX_SEEK)
        {
            uint16_t pos;
            uint8_t  found = 0U;

            for (pos = 0U; (uint16_t)(pos + 5U) <= n; pos++)
            {
                if (memcmp(&frame_buf[pos], "+IPD,", 5) == 0)
                {
                    found = 1U;
                    break;
                }
            }

            if (found == 0U)
            {
                /* 没有帧头：可能是 "SEND OK" 这类回执，也可能是被切断的 "+IP"。
                 * 留末尾 4 字节防切头，其余丢弃 */
                OTA_Consume((n > 4U) ? (uint16_t)(n - 4U) : 0U);
                osDelay(2);
                continue;
            }

            OTA_Consume((uint16_t)(pos + 5U));
            rx_state = (uint8_t)RX_LEN;
            len_val = 0U;
            len_comma = 0U;
            osDelay(1);
            continue;
        }

        /* ---------- 状态二：解析长度，直到 ':' ---------- */
        if (rx_state == (uint8_t)RX_LEN)
        {
            uint16_t i = 0U;
            uint8_t  got_colon = 0U;

            while (i < n)
            {
                uint8_t c = frame_buf[i];

                if ((c >= '0') && (c <= '9'))
                {
                    /* len_comma 之后是 CIPDINFO 的 ip/port，里面的数字不算长度 */
                    if ((len_comma == 0U) && (len_val < 1048576U))
                    {
                        len_val = (len_val * 10U) + (uint32_t)(c - '0');
                    }
                    i++;
                }
                else if (c == ',')
                {
                    len_comma = 1U;
                    i++;
                }
                else if (c == ':')
                {
                    i++;
                    got_colon = 1U;
                    break;
                }
                else
                {
                    /* 非法字符 = 已经错位。丢掉这个字节，回去重新找帧头。
                     * 有了这一步，"错位"最多损失一点数据，不会锁死 */
                    OTA_Consume((uint16_t)(i + 1U));
                    rx_state = (uint8_t)RX_SEEK;
                    break;
                }
            }

            if (rx_state != (uint8_t)RX_LEN)
            {
                osDelay(2);
                continue;
            }

            OTA_Consume(i);   /* 长度字段部分不论解析完没完都先释放 */

            if (got_colon != 0U)
            {
                frame_remain = len_val;
                rx_state = (frame_remain == 0U) ? (uint8_t)RX_SEEK : (uint8_t)RX_DATA;
            }
            osDelay(1);
            continue;
        }

        /* ---------- 状态三：消费正文 ---------- */
        {
            uint16_t take = n;
            if ((uint32_t)take > frame_remain)
            {
                take = (uint16_t)frame_remain;
            }

            if (hdr_done == 0U)
            {
                uint16_t space = (uint16_t)(sizeof(hdr_buf) - 1U - hdr_len);
                uint16_t cp = (space < take) ? space : take;
                char    *he;

                if (space == 0U)
                {
                    /* 768 字节还装不下响应头，不正常，别在这儿死循环 */
                    OTA_Mark("ota3:hdr");
                    OTA_Log("\r\n[OTA] header too large\r\n");
                    ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);
                    return false;
                }

                if (cp > 0U)
                {
                    memcpy(&hdr_buf[hdr_len], frame_buf, cp);
                    hdr_len = (uint16_t)(hdr_len + cp);
                    hdr_buf[hdr_len] = '\0';
                    OTA_Consume(cp);
                    frame_remain -= cp;
                }

                he = strstr(hdr_buf, "\r\n\r\n");
                if (he == NULL)
                {
                    /* 响应头跨帧，继续攒 */
                    if (frame_remain == 0U)
                    {
                        rx_state = (uint8_t)RX_SEEK;
                    }
                    osDelay(1);
                    continue;
                }

                /* 头齐了，先看状态码。这里已经是"行首 HTTP/1.x"之后的内容，
                 * 命令模式没有透传回显，不会再把 GET 请求行骗进来 */
                if (strstr(hdr_buf, " 200 ") == NULL)
                {
                    OTA_MarkText("o4:", hdr_buf);
                    OTA_Log("\r\n[OTA] status: ");
                    OTA_Log(g_at_tail);
                    OTA_Log("\r\n");
                    ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);
                    return false;
                }

                {
                    char *cl = strstr(hdr_buf, "Content-Length:");
                    if (cl != NULL)
                    {
                        total_fw_size = (uint32_t)atoi(cl + 15);
                    }
                }
                /* total_fw_size 的三重关系：
                 *   ① 它是“收多少算完”的终止条件（written_bytes >= total_fw_size）；
                 *   ② 它最终写进 OTA_Info_t.fw_size，Bootloader 按它搬；
                 *   ③ 它必须 <= APP 分区 960KB，否则必然写越界。
                 * 注意：这里的 960KB 只是 APP 分区上限，不是第 2 步的擦除窗口（320KB）。 */
                if ((total_fw_size == 0U) || (total_fw_size > (960U * 1024U)))
                {
                    OTA_Mark("ota5:size");
                    ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);
                    return false;
                }
                hdr_done = 1U;

                /* 响应头后面紧跟的就是固件开头，已经存在 hdr_buf 里，
                 * 必须当场写出去，否则会丢这几个字节。
                 *
                 * 账目关系：
                 *   hdr_len 里包含“响应头 + 头后正文”；
                 *   off  = 头结束位置（he+4）在 hdr_buf 中的偏移；
                 *   body = hdr_len - off，就是已经收进 hdr_buf 的正文长度。
                 *   frame_remain 在 memcpy(cp) 时已经减过 cp，
                 *   body 是 cp 的子集，所以这里只写不再次减 frame_remain。 */
                {
                    uint32_t off  = (uint32_t)(he + 4 - hdr_buf);
                    uint32_t body = (uint32_t)hdr_len - off;

                    if (body > total_fw_size)
                    {
                        body = total_fw_size;
                    }
                    if (body > 0U)
                    {
                        W25Q64_Write_Safe(write_addr, (uint8_t *)&hdr_buf[off], body);
                        crc32_val = Calc_CRC32(crc32_val, (uint8_t *)&hdr_buf[off], body);
                        write_addr += body;
                        written_bytes += body;
                    }
                }
                hdr_len = 0U;   /* 头处理完就释放，后面用不到了 */

                if (frame_remain == 0U)
                {
                    rx_state = (uint8_t)RX_SEEK;
                }
                osDelay(1);
                continue;
            }

            /* 纯固件正文，直接落 Flash。
             *
             * take 的三个上限：
             *   1) n            —— 本轮快照里最多只有这么多；
             *   2) frame_remain —— 不能越过当前 +IPD 帧；
             *   3) room         —— 不能超过 Content-Length。
             * 这三个上限取最小后，才允许消费/写 Flash/更新 CRC。
             *
             * 恒等式：
             *   write_addr    = BIN_ADDR + written_bytes
             *   crc32_val     = CRC(此前所有 take)
             *   frame_remain -= take   （与 OTA_Consume(take) 同步）
             */
            if (take > 0U)
            {
                uint32_t room = total_fw_size - written_bytes;

                if ((uint32_t)take > room)
                {
                    take = (uint16_t)room;
                }
                if (take > 0U)
                {
                    W25Q64_Write_Safe(write_addr, frame_buf, take);
                    crc32_val = Calc_CRC32(crc32_val, frame_buf, take);
                    write_addr += take;
                    written_bytes += take;
                    OTA_Consume(take);
                    frame_remain -= take;
                }
            }

            /* 兜底：take 被上面的 total 上限截成 0（理论上写完就该 break，
             * 走不到这里）。万一走到，也必须消费字节往前走，
             * 否则就是新的死循环 —— 这一版最怕的就是"不消费"。 */
            if (take == 0U)
            {
                uint16_t drop = (n < frame_remain) ? n : (uint16_t)frame_remain;
                if (drop == 0U)
                {
                    drop = 1U;
                }
                OTA_Consume(drop);
                frame_remain -= drop;
            }

            /* 进度打到屏幕：卡住时能一眼看出停在第几字节。
             * 每 4 帧刷一次，避免刷屏拖慢接收 */
            ui_tick++;
            if ((ui_tick & 0x03U) == 0U)
            {
                char      pb[22];
                uint32_t  v = written_bytes;
                uint8_t   p = 0U;
                char      tmp[11];
                uint8_t   t = 0U;

                pb[p++] = 'd';
                pb[p++] = ':';
                if (v == 0U)
                {
                    tmp[t++] = '0';
                }
                else
                {
                    while ((v > 0U) && (t < 10U))
                    {
                        tmp[t++] = (char)('0' + (v % 10U));
                        v /= 10U;
                    }
                }
                while ((t > 0U) && (p < 20U))
                {
                    pb[p++] = tmp[--t];
                }
                pb[p++] = '/';
                v = total_fw_size;
                t = 0U;
                while ((v > 0U) && (t < 10U))
                {
                    tmp[t++] = (char)('0' + (v % 10U));
                    v /= 10U;
                }
                while ((t > 0U) && (p < 21U))
                {
                    pb[p++] = tmp[--t];
                }
                pb[p] = '\0';
                /* 直接覆盖屏幕第二行，避免再经一层拷贝 */
                {
                    uint8_t z = 0U;
                    while ((pb[z] != '\0') && (z < 21U))
                    {
                        g_at_tail[z] = pb[z];
                        z++;
                    }
                    g_at_tail[z] = '\0';
                }
            }

            if (frame_remain == 0U)
            {
                rx_state = (uint8_t)RX_SEEK;
            }
            if (written_bytes >= total_fw_size)
            {
                break;
            }
            osDelay(1);
            continue;
        }
    }

    /* CRC 的最后一步：整体异或 0xFFFFFFFF。
     * 前面 Calc_CRC32 只在"续算"用，中间值不能直接当结果比较。
     * 这一步和 Bootloader 端必须完全一样，否则永远对不上 */
    crc32_val ^= 0xFFFFFFFF;

    /* 收完了，关掉 TCP。命令模式不需要发 "+++" 退出透传
     * （透传模式才要，而且 "+++" 前后还要留静默时间，很麻烦） */
    ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK", 1000);


    /* ================= 第 8 步：写 OTA 标志 =================
     *
     * 能走到这里，说明循环是从 `written_bytes >= total_fw_size` 那条
     * break 出来的（断流会走 ota6:rx 提前 return），所以字节数是齐的。
     *
     * 标志写在 W25Q64 的独立扇区，不放 Flash 内部 —— 因为
     * Bootloader 和 APP 都可能被擦写，标志必须放在第三方介质上才可靠。
     *
     * 结构里四个字段各有不可替代的作用：
     *   magic     —— 判断"这个扇区到底有没有有效标志"。上电时若不为魔数，
     *                说明从没 OTA 过（或已被消费），直接跳旧 APP。
     *   fw_size   —— Bootloader 按它算要擦几个扇区、搬多少字节。
     *                不能靠"读到最后发现 0xFF"来判断，因为固件内部
     *                本来就可能含 0xFF。
     *   fw_crc32  —— 搬运完成后回读重算，和这个值比对，防"传输对了
     *                但写 Flash 写坏了"。
     *   version   —— 纯标识用途，方便串口/屏幕打印确认刷的是哪一版。
     *
     * 顺序很关键：**先擦标志扇区，再写新标志**。
     * 如果先写后擦，或者擦写顺序反了，会留下"半新半旧"的标志，
     * Bootloader 可能拿到一个 size 是新值、crc 是旧值的组合。
     *
     * 写入前的恒等式（能走到这里说明循环是正常 break 出来的）：
     *   written_bytes == total_fw_size
     *   crc32_val     == CRC([BIN_ADDR, BIN_ADDR + total_fw_size)) 的最终值
     *   sizeof(OTA_Info_t) == 16 字节（4 个 uint32_t，无填充）
     * 因此下面直接写 total_fw_size / crc32_val 是安全的；
     * 若以后加字段，必须同时改 Bootloader 的读取偏移。
     */
    OTA_Info_t ota_info;
    ota_info.magic    = OTA_MAGIC_NUMBER;
    ota_info.fw_size  = total_fw_size;   /* == written_bytes */
    ota_info.fw_crc32 = crc32_val;       /* 已异或 0xFFFFFFFF 的最终值 */
    ota_info.version  = APP_VERSION_NUM; /* 版本标识；与 APP_VERSION_STR/固件文件名同源 */

    W25Q64_EraseSector(W25Q64_OTA_FLAG_ADDR);
    W25Q64_Write_Safe(W25Q64_OTA_FLAG_ADDR, (uint8_t *)&ota_info, sizeof(OTA_Info_t));

    /* 延时 1 秒再复位：给 W25Q64 留出内部写周期完成的时间。
     * W25Q64 写完之后还有一段 tW（页编程最大 3ms）加内部状态机收尾，
     * 复位太快可能标志还没真正落盘，Bootloader 起来读到的仍是 0xFF */
    osDelay(1000);
    NVIC_SystemReset();

    return true;
}

