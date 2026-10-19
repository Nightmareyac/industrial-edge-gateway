# 工业边缘多协议（Modbus/CAN）网关系统

> `industrial-edge-gateway` — STM32F407 + FreeRTOS + LVGL 的工业边缘网关，带 **巴法云 MQTT 远程 OTA 升级**。

从零打通了整条链路：云端下发指令 → ESP-01S 联网下载固件 → 写入外部 Flash → 重启校验搬运 → 新版本运行。全过程踩了 12 个坑，本文档完整记录了每一个的**现象、定位手法、根因、修复**，以及面试时该怎么讲。

---

## 目录

- [1. 项目概览](#1-项目概览)
- [2. 系统架构](#2-系统架构)
- [3. OTA 完整链路](#3-ota-完整链路)
- [4. 可靠性设计（面试重点）](#4-可靠性设计面试重点)
- [5. 踩坑全记录（12 个）](#5-踩坑全记录12-个)
- [6. 调试方法论](#6-调试方法论)
- [7. 面试问答速查](#7-面试问答速查)
- [8. 目录结构与构建](#8-目录结构与构建)
- [9. 代码复盘路线](./docs/复盘路线.md) ← **按顺序复盘代码 + 三分钟口述稿**

---

## 1. 项目概览

| 项 | 内容 |
|---|---|
| MCU | STM32F407VGT6（Cortex-M4F，1MB Flash / 192KB RAM） |
| RTOS | FreeRTOS（CMSIS-RTOS2 封装） |
| GUI | LVGL + ST7735 SPI 屏（横向 160×128） |
| 联网 | ESP-01S（ESP8266，AT 固件 v2.2.0，USART3 115200） |
| 云平台 | 巴法云 MQTT（`mqtt.bemfa.com:9501`） |
| 存储 | W25Q64（8MB SPI Flash），用于 OTA 固件暂存 |
| 构建 | EIDE（VSCode）+ armcc，调试器 JLink / OpenOCD |
| Bootloader | 独立工程，占用 sector 0-3 |

屏幕会显示当前版本标题（如 `Edge Gateway V2.1`）和 MQTT 状态，OTA 成功后标题变成 `V3.0` —— 这就是版本验证手段。

---

## 2. 系统架构

### 2.1 Flash 分区

```
0x08000000 ┌────────────────────┐
           │  Bootloader 64KB   │  sector 0-3
0x08010000 ├────────────────────┤
           │  APP 960KB         │  sector 4-11
0x080F0000 └────────────────────┘
```

RAM 内没有双区，所以**升级必须借外部 Flash 做中转**：

```
W25Q64:
0x000000  ┌────────────────┐
          │ OTA 固件暂存区 │  OTA_BIN_ADDR
0x07F000  ├────────────────┤
          │ OTA 标志区     │  OTA_FLAG_ADDR（magic + size + crc32 + version）
          └────────────────┘
```

### 2.2 任务划分

| 任务 | 优先级 | 栈 | 职责 |
|---|---|---|---|
| UITask | Normal | 4096 | LVGL 刷新、状态显示 |
| MQTTTask | Low | 4096 | MQTT 连接/订阅/收指令，**OTA 下载也在这里执行** |
| NetworkTask | Low | 2048 | 上电 AT 握手、WiFi 连接、失败重试 |
| SensorTask | Low1 | 2048 | 传感器采集 |
| DogTask | Realtime | 1024 | 看门狗 |

> MQTTTask 栈从 2048 加到 4096 是有原因的，见 [坑 12](#坑-12命令模式卡在-82kb整帧解析死锁)。

---

## 3. OTA 完整链路

```
① 巴法云控制台发送 "upgrade" 到订阅主题
            │
② MQTTTask 收到 +MQTTSUBRECV
            │  AT+MQTTCLEAN=0  ← 释放 link 0，串口让给 HTTP
            ▼
③ 命令模式建链
   AT+CIPMODE=0 / AT+CIPCLOSE / AT+CIPMUX=0
   AT+CIPSTART="TCP","192.168.137.1",80
            │
④ 发送请求（两条回执都要等到）
   AT+CIPSEND=<len>  → 必须回 '>'
   <GET /A_Project_v3.0.bin HTTP/1.1...>  → 必须回 "SEND OK"
            │
⑤ 接收响应（+IPD 帧流式解析）
   +IPD,1460:<data>  → 响应头攒进 hdr_buf → 正文写 W25Q64 + 累加 CRC32
            │
⑥ 收满 Content-Length 字节
   写入 OTA 标志：magic / fw_size / crc32 / version
   NVIC_SystemReset()
            │
⑦ Bootloader 启动
   读标志 → 三重校验(SP + Reset 向量 + CRC32) → 擦 APP 扇区 → 搬运 → 回读复核
            │
⑧ 跳转新 APP：清 NVIC 中断使能/挂起 → 设 MSP → 设 VTOR → 跳 Reset_Handler
```

---

## 4. 可靠性设计（面试重点）

### 4.1 三重校验，缺一不可

Bootloader 拿到镜像后依次检查：

| 检查项 | 目的 | 判据 |
|---|---|---|
| **初始 SP** | 防止跳到无效栈 | 必须落在 `0x20000000` 段内（掩码 `0x2FFE0000`） |
| **Reset 向量** | 防止跳到错误地址区间 | 必须落在 APP 区 `[0x08010000, 0x08010000+size)`，且 bit0=1（Thumb） |
| **CRC32** | 防止搬运/传输损坏 | 标准反射多项式 `0xEDB88320`，初值 `0xFFFFFFFF`，末尾异或 |

**为什么只验 SP 不够？** 我们踩的第一个坑就是只验了 magic + fw_size：一份按 `0x08000000` 链接的旧镜像被原样刷进 APP 区，它的 Reset 向量指向 `0x0800033D`（Bootloader 自己），跳过去直接 HardFault → **白屏**。

### 4.2 失败保留旧 APP

任何一步校验失败：**不擦除、不搬运、清标志、继续跑旧 APP**。设备永远可用，不会变砖。这比"重试三次"更重要——**可恢复性优于成功率**。

### 4.3 擦除时序前移

擦 W25Q64 必须在**固件流开始之前**完成。原因：2048 字节接收缓冲在 115200 波特率下 **178ms 就填满**，而擦一个扇区要几十毫秒。如果"边收边擦"，必然丢字节 → 固件损坏。

### 4.4 CRC 双端逐位一致

APP 端计算 CRC32 时逐块更新，Bootloader 端用**同一个多项式、同样的初值和结尾异或**独立回读重算。两端算法必须一模一样，否则永远对不上。

---

## 5. 踩坑全记录（12 个）

> 每一条都是「真实现象 → 怎么定位 → 根因 → 怎么修 → 面试一句话」。

---

### 坑 1：OTA 后白屏

**现象**：OTA 显示成功后重启，屏幕全白，程序不跑。

**定位**：把两份工程的链接地址、向量表内容都算出来对比。
- Bootloader 烧在 `0x08000000`，APP 应该在 `0x08010000`
- 出问题的那份 `firmware.bin`，Reset 向量 = `0x0800033D` —— 这是 `0x08000000` 段的地址

**根因**：**镜像链接基址和 Bootloader 的搬运目标不一致**，同时 Bootloader 只校验了 magic 和 fw_size，没校验 Reset 向量，非法镜像被原样接受。

**修复**：
1. Bootloader 增加 Reset 向量范围检查 + CRC32 校验
2. 重新按 `0x08010000` 链接生成载荷
3. APP 的 `SCB->VTOR = FLASH_BASE | 0x10000` 提到 `HAL_Init()` **之前**（原工程 HAL 时基用 TIM14，`HAL_Init()` 内部就使能中断，存在竞争窗口）

**面试一句话**：
> OTA 白屏十有八九是**镜像基址与 Bootloader 搬运地址不一致**。Bootloader 必须校验 Reset 向量落在 APP 区间且 bit0=1，光验 magic 是不够的。

---

### 坑 2：EIDE 下载基地址没改

**现象**：编译下载都"成功"，但设备跑的还是 Bootloader 或者直接卡住。

**定位**：查 `MDK-ARM/.eide/eide.yml` 的 `uploadConfigMap`。

**根因**：`baseAddr` 还是 `0x08000000`，把 APP 镜像直接写到了 Bootloader 的地址上，**把 Bootloader 自己覆盖了**。

**修复**：`uploadConfigMap.JLink.baseAddr` 和 `.OpenOCD.baseAddr` 都改成 `0x08010000`。

**面试一句话**：
> 换 Bootloader 方案后，**编译器的链接脚本、IDE 的下载地址、Bootloader 的搬运地址**这三者必须同步改，漏一个就是白屏或者覆盖 Bootloader。

---

### 坑 3：ESP-01S 固件不带 MQTT

**现象**：`AT+MQTTCONN` 一直回 `ERROR`，屏幕显示离线。

**定位**：`AT+GMR` 查版本 → `AT version:1.7.4.0`。这是 **NONOS 通用版 AT 固件**，指令集里根本没有 MQTT 系列。

**根因**：出厂固件版本太老，不含 `AT+MQTTUSERCFG` / `AT+MQTTCONN`。

**修复**：用 esptool 刷 **ESP-AT v2.2.0（ESP8266_1MB）**，刷完 `AT+GMR` 变成 `2.3.0.0-dev / Bin 2.2.0`，MQTT 指令全部可用。

**面试一句话**：
> 接 ESP8266 第一件事就是 `AT+GMR` **确认固件能力边界**。1.7.x 和 2.x 的指令集差一大截，老固件上再怎么调代码都没用。

---

### 坑 4：烧录固件反复失败

**现象**：`Invalid SLIP escape (0xdb, 0x73)`、`crc_efuse_4bit: 0` vs `crc_calc_4bit: 13`，烧录卡住不动。

**定位**：换供电、并电容后重试对比。

**根因**：**供电不足**。CH340 模块的 3.3V 输出只有 30~50mA，而 ESP-01S 峰值电流 300mA+。电压跌落导致 Flash 写入时序错乱。

**修复**：独立 3.3V 供电 + 加去耦电容；烧录时 GPIO0 拉低进下载模式，CH_PD 必须接 3.3V。

**面试一句话**：
> ESP8266 的"玄学故障"里，**供电**占一大半。峰值 300mA，USB-TTL 的 3.3V 针脚根本带不动。

---

### 坑 5：AT 指令无任何响应

**现象**：串口工具发 `AT`，一个字节都不回。屏幕显示 `ESP: NO AT` / `AT: <NO DATA>`。

**定位**：三个错误同时存在——
1. **TX/RX 接反**（模块 TXD 应接 MCU PB11，RXD 接 PB10）
2. 串口工具发送模式是"无追加"，**没带 `\r\n`**
3. 指令拼错：发了 `AT+GRM`（正确是 `AT+GMR`）

**根因**：AT 指令必须以 `\r\n` 结尾，且链路方向要对。

**修复**：改接线、串口工具加 `\r\n`、指令拼写校正。同时把界面文案从 `<NO REPLY>` 改成 `<no data>` —— 原写法会让人误以为"模块哑了"，实际只是"这一瞬间缓冲里没抓到"。

**面试一句话**：
> AT 无响应先查三件事：**供电、TX/RX 是否交叉、指令有没有 `\r\n` 结尾**。这三样占 90%。

---

### 坑 6：巴法云一直"发送失败"

**现象**：设备订阅正常（云端显示在线），但网页上手动发消息一直提示发送失败。

**定位**：用 API 做对照实验——
- `type=3` 的主题 `xxx002` → `code: 0`（成功）
- 出问题的主题 → `code: 40004`
- 查该主题详情，发现带 `protocol` 对象，`error: "device model is not configured"`

**根因**：该主题创建时绑定了**物模型（deviceType）但模型没配置**，发布路径被平台侧拒绝，而订阅不受影响 —— 所以"在线但发不出"。

**修复**：新建一个纯 `type=3`（无物模型）的主题，实测 `code: 0`。

**面试一句话**：
> "在线但消息发不出去"这类问题，**先怀疑平台侧物模型/权限配置**，用 API 做对照实验最快定位——同一账号下换一个主题，能发就说明不是代码问题。

---

### 坑 7：UART 溢出后永久失聪

**现象**：跑一段时间后，AT 指令全部无响应，但模块本身是活的（重新上电就好）。

**定位**：看 HAL 源码发现：`HAL_UART_Receive_IT` 遇到 **ORE（溢出错误）** 会调用 `UART_EndRxTransfer` 把接收**中止**，而原工程**没有实现 `HAL_UART_ErrorCallback`**，接收就再也不会重新挂上。

**根因**：ORE 一旦发生 → HAL 内部 aborted → 没人重挂接收 → 永久失聪。FreeRTOS 任务切换的几毫秒空窗就足以触发 ORE。

**修复**：
1. 实现 `HAL_UART_ErrorCallback`：清 ORE 标志 + 重新 `HAL_UART_Receive_IT`，并累计 `g_uart3_err_cnt` 作为观测指标
2. 接收架构改为 **中断单字节接收 + 线性缓冲 + 后台任务解析**，不用轮询

**面试一句话**：
> 用 HAL 的 `HAL_UART_Receive_IT` 必须自己实现 `HAL_UART_ErrorCallback` 重新挂接收，否则一次溢出就永久收不到数据。**这是 HAL 的经典坑。**

---

### 坑 8：状态机条件写反

**现象**：WiFi 连接失败后，MQTT 任务反而认为"已就绪"开始干活。

**根因**：
```c
while (g_ota_state < OTA_STATE_WIFI_CONNECTED)   // 错
```
枚举里 `ERROR = 5` 是**最后一个值**，比 `WIFI_CONNECTED = 2` 大，失败态反而满足"<"条件直接放行。

**修复**：改成显式比较 `while (g_ota_state != OTA_STATE_WIFI_CONNECTED)`。

**面试一句话**：
> 状态机判断**永远用 `!=` 或 `==` 精确比较，不要用 `<` `>`**——枚举加一个值就可能让顺序语义整体失效。

---

### 坑 9：OTA 卡在 downloading

**现象**：屏幕显示 `OTA downloading` 永远不动，服务器日志能看到请求，但数据传不完。

**根因**：原代码在**收到响应头之后**才开始擦 W25Q64。响应头一到，正文就跟着灌进来，而擦扇区要几十毫秒，2048 字节缓冲在 115200 下 178ms 就满 —— **边收边擦必然丢字节**。

**修复**：擦除整个动作**前移到 `AT+CIPSTART` 之前**；并且按需擦（80 个扇区 = 320KB）而不是无脑擦满 960KB，省 12 秒等待。

**面试一句话**：
> 流式接收 + 慢速外设操作（Flash 擦除）必须**串行化**，绝不能边收边擦。要么提前擦，要么双缓冲。

---

### 坑 10：`CIPSEND` 和透传模式打架

**现象**：发了 `AT+CIPMODE=1` 之后，再发 `AT+CIPSEND`，等不到 `>` 提示符，整个流程卡死。

**根因**：**透传模式下 `CIPSTART` 一回 `CONNECT` 就自动进入透传**，此时串口上任何字节都会被当作数据发给服务器。`AT+CIPSEND` 被当数据发走了，自然等不到提示符。

**修复**：两条路不能混——
- 透传模式：`CIPMODE=1` → `CIPSTART` → **直接灌数据**，不要 `CIPSEND`
- 命令模式：`CIPMODE=0` → `CIPSTART` → `CIPSEND=<len>` 等 `>` → 灌数据 → 等 `SEND OK`

**面试一句话**：
> ESP-AT 的透传和命令模式是**两套互斥的状态机**，混用必然卡死。透传的判据是 `CIPSTART` 回 `CONNECT`，之后串口就不再是 AT 通道了。

---

### 坑 11：透传"连上了但一个字节都不发"（最耗时的一个）

**现象**：屏幕依次显示 `ota0:begin` → `ota2` → `ota3:hdr` → `error`，连续 5 次，每次都无法完成。

**定位过程**（这一段是方法论的核心）：

1. **`netstat -ano | grep :80`** → 发现 5 条来自 ESP（`192.168.137.13`）的 `ESTABLISHED` 连接
   → **结论：TCP 握手成功，网络、防火墙、端口、IP 全都没问题**。这直接推翻了前几轮"网络不通"的假设。

2. **看 HTTP 服务端访问日志** → 一条来自 ESP 的 `GET` 都没有
   → **结论：连上了，但一个字节都没进 socket**。

3. 由此定性：**透传模式下串口数据没有真正转发到 socket，而且透传没有任何回执，外部完全观测不到原因。**

**根因**：AT 2.2.0 在这块 ESP-01S 上透传不可靠；更关键的是——**透传模式没有中间回执，把问题变成了黑盒**。

**修复**：彻底放弃透传，改**命令模式 + `AT+CIPSEND`**：

```
AT+CIPSTART="TCP",...   → CONNECT + OK    （连接已建立，有回执）
AT+CIPSEND=<len>        → >               （可以发了，有回执）
<GET ...>               → SEND OK         （数据真的进了 TCP 发送缓冲，有回执）
```

有了 `SEND OK` 这句回执，"连上了但服务器没收到"这种情况再也不可能含糊过去。

**面试一句话**：
> 遇到"连接建立但数据不流动"，**先用 netstat 看连接状态、再看服务端日志看有无请求**，两步就能把问题夹在中间。如果一条路径**没有中间回执**，果断换成有回执的方案——可观测性比省几条 AT 指令重要得多。

---

### 坑 12：命令模式卡在 82KB —— 整帧解析死锁

**现象**：改命令模式后日志出现 `GET 200`（请求终于通了！），但固件传了 82KB 就不动了，一分钟都没进展。

**定位过程**：

1. **服务端日志**：`"GET /A_Project_v3.0.bin HTTP/1.1" 200 -` → 请求、响应头都正常
2. **采样服务进程 IO 计数**（`GetProcessIoCounters`）：
   ```
   WriteTransferCount  82422  →  82422   delta = 0 bytes / 3s
   ```
   固件 206884 字节，**只送出 82422，3 秒内增量 0**，连接仍 `ESTABLISHED`
3. **结论链**：MCU 不再消费串口 → ESP 模块缓冲满 → 停止从 socket 取数 → TCP 窗口归零 → 服务器阻塞在 `send()`
4. **代码复查**，找到死锁点：

```c
if ((pay_len == 0U) || ((uint16_t)(n - pay) < pay_len))
{
    OTA_Consume(pos);   // pos == 0 时这行什么都不做！
    osDelay(5);
    continue;
}
```

三种任意一种命中就永久死循环：
- 某一帧长度 > 缓冲可用空间（模块合并上报多段时会发生）
- 丢 1 个字节，长度字段被解析成一个巨大值
- 帧头正好在缓冲头部，`pos == 0`，`OTA_Consume(0)` 是空操作

**根因**：**"整帧到齐才处理"的解析模型，配上"消费量为 0 也 continue"的退让分支，等于给自己留了一条死锁路径。**

**修复**：改成**流式状态机**，只记"本帧还剩多少字节"，边收边写：

```c
enum { RX_SEEK, RX_LEN, RX_DATA };

RX_SEEK: 找 "+IPD,"        → 消费到帧头之后
RX_LEN : 解析长度到 ':'     → 遇到非法字符就丢 1 字节回 RX_SEEK 重同步
RX_DATA: take = min(n, frame_remain)
         → 写 Flash + 累加 CRC + OTA_Consume(take)
         → frame_remain == 0 才回 RX_SEEK
```

配套改动：
- **任何分支都必须消费至少 1 字节**，杜绝新增的不消费路径（加兜底分支）
- `hdr_buf[768]` 从栈改 `static`（.bss），
- MQTTTask 栈 2048 → 4096（OTA 在该任务内同步执行，`cmd_buf` + `http_req` + `at_cmd` 已占 768）
- 屏幕加实时进度 `d:82000/206884`，卡住时一眼看到停在第几字节
- 用 `OTA_BufFind()`（带长度）替换 `strstr` —— 接收缓冲满 2047 字节时末尾没有 `'\0'`，`strstr` 会越界

**面试一句话**：
> 流式协议解析**不要设计成"整包到齐才处理"**，一旦出现"消费 0 字节还继续循环"的退让分支，就是死锁。正确做法是状态机 + 剩余长度计数，保证每次循环都有推进。

---

## 6. 调试方法论

这是我认为比代码更值钱的部分。

### 6.1 分层定位，逐层排除

```
物理层    供电够不够？TX/RX 交叉？CH_PD 拉高？
  ↓
链路层    AT 通不通？AT+GMR 固件什么版本？指令集够不够？
  ↓
网络层    WiFi 连上没有？IP 拿到没有？CIPSTART 回什么？
  ↓
传输层    TCP 建立了吗？（netstat）数据真的发出去了吗？（SEND OK）
  ↓
应用层    HTTP 状态码？Content-Length 对不对？字节数够不够？
```

**每一层都要有独立的、外部的观测手段**，不能靠"代码看起来对"。

### 6.2 用外部观测替代猜测

| 想看什么 | 用什么工具 |
|---|---|
| TCP 到底连上没有 | `netstat -ano \| grep :80` |
| 服务器到底收到请求没有 | HTTP 服务端访问日志 |
| 服务器到底发了多少字节 | `GetProcessIoCounters`（进程 IO 计数采样） |
| 设备卡在哪一步 | 屏幕分级标记 `ota0/1/2/3...` |
| 模块回了什么原文 | USART1 日志（PA9/PA10，115200） |

这五个手段组合起来，能把"设备→模块→网络→服务器"整条链路上的每一段都夹住，问题无处可藏。

### 6.3 失败开放优于失败关闭

透传那次，我用 `ESP8266_SendCmd(cmd, "CONNECT", 8000)` 等 `CONNECT` 作为"连接成功"判据。透传下 `CONNECT` 应答和清缓冲存在**天然竞态**，误判成失败就提前 `return`，留下一条没人用的僵尸连接。

**教训**：对**没有可靠回执的状态**，判据要放在**后面真正能证明成功的事件**上（这里就是"有没有收到 HTTP 响应头"），而不是用一个可能误判的前置信号。

### 6.4 回执驱动

每一步都要有**明确的、可观测的回执**：
- 发指令 → 等 `OK`
- 要发数据 → 等 `>`
- 数据发完 → 等 `SEND OK`

没有回执的路径 = 黑盒 = 无法调试。**宁可多花几条 AT 指令，也要换可观测性。**

### 6.5 屏幕分级标记

屏幕只有 22 个字符，但足够放阶段标记：

| 标记 | 含义 |
|---|---|
| `ota0:begin` | 收到 upgrade 指令 |
| `ota1:mux` | CIPMUX 设置失败 |
| `ota2:xxx` | CIPSTART 失败，附模块原文 |
| `o4:HTTP/1.0 404` | 服务器返回非 200，附状态行原文 |
| `d:82000/206884` | 实时下载进度 |
| `ota6:rx` / `ota6:allto` | 流中断 / 总超时 |
| `ota7:cps` / `ota8:snd` | CIPSEND 没回 `>` / 没有 `SEND OK` |

不接串口也能定位到**具体哪条 AT 指令**。

---

## 7. 面试问答速查

**Q：OTA 怎么保证不刷成砖？**
> 三层。第一，固件先写到外部 Flash 暂存，不动 APP 区；第二，Bootloader 做三重校验（初始 SP 合法性、Reset 向量落在 APP 区间且 bit0=1、CRC32 逐字节一致）；第三，任何一步失败都不擦除、清标志、继续跑旧 APP。**可恢复性优先于成功率。**

**Q：为什么 CRC32 够了，不用签名？**
> CRC32 解决的是**传输/存储损坏**，不是**恶意篡改**。当前是局域网内自己搭的 HTTP 服务，威胁模型里没有攻击者，CRC32 足够且开销低。如果要做安全 OTA，应该在 CRC 之上加 ECDSA/Ed25519 签名，公钥固化在 Bootloader 里。**要分清"完整性校验"和"认证"是两件事。**

**Q：为什么放弃 ESP8266 透传模式？**
> 透传模式下串口数据和 socket 之间**没有任何回执**。我遇到的现象是：TCP 五次都连上了（netstat 有 ESTABLISHED），但服务端日志里一个 GET 都没有，串口数据没进 socket，而且模块不报任何错。**没有回执就没法调试。** 换成命令模式后，`CIPSEND` 有 `>`、数据发完有 `SEND OK`，每一步都能确认，问题立刻现形。

**Q：设备"连上了但服务器收不到"，你怎么定位？**
> 两步夹逼：①`netstat` 看 TCP 是否 ESTABLISHED —— 判断网络层是否通；②看服务端访问日志有没有请求 —— 判断应用层有没有数据。如果连接在、日志空，问题就锁定在**模块的数据转发**这一段。再看服务端进程的 IO 写出字节数，就能知道是"一个字节没发"还是"发到一半停了"。

**Q：接收缓冲怎么设计的？为什么不是环形缓冲？**
> 中断单字节接收 + 线性缓冲 + 后台任务解析。选线性是因为解析器需要**随机访问**（找帧头、跨帧拼响应头），环形缓冲还得处理回绕，反而更复杂。代价是消费时要 `memmove`，但每次都是整帧消费、`memmove` 的量不大，可以接受。真正要注意的是**任何分支都必须消费至少 1 字节**——我踩过一次"消费 0 字节还 continue"导致的死锁。

**Q：OTA 过程中最难的是什么？**
> 不是写代码，是**建立可观测性**。整个过程在 MCU、ESP 模块、网络、服务器四段之间流转，任何一段出问题表现都一样：屏幕卡住。我最后靠五件事把链路夹死——netstat 看连接、服务端日志看请求、进程 IO 计数看字节数、屏幕分级标记看阶段、USART1 日志看模块原文。**能观测才能定位。**

**Q：为什么擦 Flash 要放在发请求之前？**
> 因为擦扇区是几十毫秒级的慢操作，而 2048 字节接收缓冲在 115200 下只有 178ms 的余量。边收边擦必然丢字节，固件就坏了。要么提前擦完，要么用双缓冲把接收和擦除解耦。我选了提前擦，并且按需擦 320KB 而不是无脑擦 960KB，省了 12 秒。

**Q：HAL 库 UART 有什么坑？**
> `HAL_UART_Receive_IT` 遇到 ORE 溢出会调用 `UART_EndRxTransfer` 中止接收，**必须自己实现 `HAL_UART_ErrorCallback` 重新挂上接收**，否则一次溢出就永久收不到数据。这个坑很隐蔽，因为现象是"跑一段时间后突然全哑了"。

---

## 8. 目录结构与构建

```
A_Project/
├── Core/
│   ├── Inc/
│   │   ├── esp8266_ota.h        # OTA 接口与 OTA_Info_t 定义
│   │   ├── w25q64.h             # 外部 Flash 驱动
│   │   └── st7735.h             # 屏幕驱动
│   └── Src/
│       ├── freertos.c           # 任务创建 / MQTT 状态机 / OTA 触发
│       ├── esp8266_ota.c        # AT 指令封装 / OTA 下载 / +IPD 流式解析
│       ├── spi.c                # SPI（W25Q64 + ST7735）
│       └── main.c               # VTOR 设置 / 时钟 / 外设初始化
├── Middlewares/Third_Party/
│   ├── FreeRTOS/
│   └── lvgl/
├── MDK-ARM/
│   └── .eide/eide.yml           # 内存布局 + 下载地址（关键！baseAddr=0x08010000）
├── ota_release/                 # 固件发布目录（按版本号命名）
└── tools/                       # 调试脚本与历史备份
```

### 构建与烧录

1. VSCode + EIDE 打开工程，选择 `A_Project` target，编译
2. 确认 `eide.yml` 的 `baseAddr` 是 `0x08010000`（APP 区）
3. JLink / OpenOCD 下载
4. 首次上电打开 USART1（PA9/PA10，115200）看 AT 握手日志

### 发布新版本

```bash
# 1. 改版本号（屏幕标题 + OTA 标志里的 version）
#    Core/Src/freertos.c    -> "Edge Gateway Vx.y"
#    Core/Src/esp8266_ota.c -> ota_info.version = xyz
# 2. 编译，把产物拷进发布目录并带版本号
cp MDK-ARM/build/A_Project/A_Project.bin ota_release/A_Project_vx.y.bin
# 3. 让设备下载对应路径的固件（freertos.c 里的 ESP8266_HTTP_OTA_Download 第三个参数）
```

服务端的根目录就是 `ota_release/`，文件一换立刻生效，不用重启服务。
所以实际发布流程收敛成三步：**编译 → 拷进 `ota_release` → 巴法云发 `upgrade`**。

### OTA 服务端（常驻部署）

设备要能下载固件，局域网里必须有一个 HTTP 服务在跑。

**不要用 `python -m http.server`**：它是前台进程，绑在当前控制台上，
关掉终端 / 退出 IDE / 注销用户就没了，设备一发 `upgrade` 就是连接被拒；
而且它不写日志文件，OTA 失败时无法回查"服务器到底收没收到请求"——
排查第 11、12 个坑时，全靠这份日志才把范围从"网络不通"缩到"设备侧没消费"。

`tools/` 下是一套常驻方案：

| 文件 | 作用 |
|---|---|
| `ota_server.py` | 服务本体：固定根目录、日志落文件、`/health` 自检 |
| `ota_service.ps1` | 管理入口：`Install / Start / Stop / Restart / Status / Log / Uninstall` |
| `install_ota_service.cmd` | 双击安装（自动请求管理员权限） |
| `uninstall_ota_service.cmd` | 双击卸载 |
| `ota_server_run.cmd` | 前台运行，想实时看日志时用 |

服务由 **Windows 计划任务**承载：`SYSTEM` 身份、session 0 无窗口运行。
不占桌面、不需要登录、关掉任何窗口都不影响；进程意外退出后 1 分钟内
自动重启（最多 3 次）。

两个触发器：**开机**（延迟 15 秒）+ **登录**（延迟 30 秒）。
加登录触发是因为 Windows 快速启动（混合关机）在部分机器上会漏跑开机触发
—— 表现为"重启后服务没起来"。两个触发器撞在一起也不会起两个实例：
任务本身 `MultipleInstances=IgnoreNew`，且服务在 bind 前会主动探测端口，
发现是自己人就静默退出（退出码 0）。

> **Install 只需要执行一次**。任务写进系统任务库，重启后依然有效，
> 开机自动启动，**不需要每次开机再敲一遍命令**。只有这三种情况要重跑：
>
> | 什么时候 | 为什么 |
> |---|---|
> | Python 换了大版本（3.13 → 3.14） | 任务 Action 里记的是 `pythonw.exe` 的绝对路径 |
> | 改了端口或固件目录 | 参数写在任务 Action 里 |
> | 执行过 `Uninstall` | 任务被删掉了 |

```powershell
# 安装（也可以直接双击 install_ota_service.cmd）
powershell -ExecutionPolicy Bypass -File tools\ota_service.ps1 -Action Install

# 日常操作
powershell -ExecutionPolicy Bypass -File tools\ota_service.ps1 -Action Status
powershell -ExecutionPolicy Bypass -File tools\ota_service.ps1 -Action Restart
powershell -ExecutionPolicy Bypass -File tools\ota_service.ps1 -Action Log -Tail 50
```

`-Action Status` 的输出同时给出三层信息：

```
计划任务 : Running
端口监听 : TCP  0.0.0.0:80  LISTENING  23968
自检     : OK
{ "status": "ok", "firmwares": [ { "name": "A_Project_v3.0.bin", "size": 206884 } ] }
```

**访问日志** `tools/ota_server.log`，一个请求一行：

```
2026-10-19 10:09:11  192.168.137.13  GET  /A_Project_v3.0.bin  -> 200  206884B   25ms
```

排查 OTA 时先看这一行，三种情况一眼分开：

| 日志现象 | 结论 |
|---|---|
| 完全没有这一行 | 设备的 GET 没过网（连接建了但没发数据） |
| `-> 404` | 路径写错，检查 `ESP8266_HTTP_OTA_Download` 第三个参数 |
| `-> 200` 但字节数不足 / 行尾标注"客户端提前断开" | 传到一半中断，看设备侧接收循环 |

换端口用 `-Port 8080 -Action Install`，但设备端
`ESP8266_HTTP_OTA_Download("192.168.137.1", 80, ...)` 的端口要同步改。
若 80 被别的程序占用，`Status` 会直接显示"端口监听：无"，
用 `netstat -ano | findstr :80` 查出占用进程。

### 配置项（部署前必改）

| 位置 | 宏 / 参数 | 说明 |
|---|---|---|
| `freertos.c` | `BEMFA_UID` | 巴法云私钥 |
| `freertos.c` | `BEMFA_TOPIC` | 订阅主题（必须是纯 `type=3`，不要绑物模型） |
| `freertos.c` | `ESP8266_JoinAP("ssid", "pwd")` | WiFi 凭据 |
| `freertos.c` | `ESP8266_HTTP_OTA_Download(ip, port, path)` | OTA 服务器地址与固件路径 |

---

## 版本记录

| 版本 | 说明 |
|---|---|
| V2.1 | OTA 基线版，MQTT 在线，title 显示 `Edge Gateway V2.1` |
| V3.0 | 首次通过 OTA 成功升级的版本，title 显示 `Edge Gateway V3.0` |

v2.1 → v3.0 的升级验证了整条链路：云端发 `upgrade`，设备下载 206884 字节固件，写入 W25Q64，Bootloader 校验搬运，重启后屏幕标题从 `V2.1` 变成 `V3.0`。
