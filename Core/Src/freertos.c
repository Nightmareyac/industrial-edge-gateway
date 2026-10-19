/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "lvgl.h"
#include "lv_port_disp.h"
#include "w25q64.h"
#include "modbus_rtu.h"
#include "esp8266_ota.h"
#include "app_config.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */



/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define BEMFA_UID   "3396ed7c726d437583ccb9b23e1f579a"
/* 主题必须在控制台建【不带物模型】的普通主题。
 * 原来用的 iqPs8B5XC019 创建时带了 airqualitysensor 设备模型，
 * 而模型没配置（protocol.error = "device model is not configured"），
 * 服务端会拒绝任何发布 —— 网页点发送报"发送失败"，HTTP API 回 40004。
 * 订阅在协议层不过模型校验，所以设备照样显示"在线"，极具迷惑性。
 * 新主题 OGLMjPSOz006 无 protocol 字段，实测发布返回 code=0 */
#define BEMFA_TOPIC "OGLMjPSOz006"

#define MQTT_BROKER_ADDR   "mqtt.bemfa.com"
#define MQTT_BROKER_PORT   9501U    /* 9501 = 明文 TCP；9503 是 TLS，需要额外配证书 */
#define MQTT_CONN_TIMEOUT  10000U   /* 等 +MQTTCONNECTED 的时间 */
#define MQTT_RESUB_PERIOD  30000U   /* 周期性补订阅的间隔 */

/* AT 调试口：USART1(115200) 在板上是空的（huart2=Modbus 9600，huart3=ESP8266），
 * 接一个 USB-TTL 就能看到完整的 AT 收发原文，MQTT 卡在哪一步一目了然。
 * 这里不用 printf 重定向：AC5 标准库的 printf 在没开 microlib 时会走
 * semihosting（BKPT 0xAB），一调用就直接卡死，比没有日志更糟。 */
#define AT_LOG_UART        huart1

/* MQTT 连接失败落在哪一步，0=正常/未失败：
 *   1 = 模块对 AT 无应答（波特率、供电、接线、没复位）
 *   2 = AT+MQTTUSERCFG 被拒（AT 固件不带 MQTT 指令集，硬件门槛）
 *   3 = 等不到 +MQTTCONNECTED（UID 被占用、网络不通、端口错）
 *   4 = AT+MQTTSUB 被拒（主题非法、链路已断） */
#define MQTT_STAGE_NONE      0U
#define MQTT_STAGE_NO_AT     1U
#define MQTT_STAGE_USERCFG   2U
#define MQTT_STAGE_CONN      3U
#define MQTT_STAGE_SUB       4U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

osMessageQueueId_t sensorQueueHandle; //传感器数据队列句柄（与 OTA 无关，只供 UI 读传感器）

/* ============================================================================
 * 全局状态变量关系图（四个变量，各管一层，不要混用）
 * ----------------------------------------------------------------------------
 *   g_ota_state      ：网络/OTA 生命周期（IDLE / CONNECTING / CONNECTED /
 *                      DOWNLOADING / ERROR）。只回答"现在处在哪个大阶段"。
 *   g_mqtt_online    ：MQTT 是否"已连接且已订阅"（0/1）。它比 WIFI_CONNECTED
 *                      更严格：WiFi 连上不代表巴法云能看到设备。
 *   g_mqtt_fail_stage：最近一次 MQTT 失败卡在哪一层（0..4），是**粘滞诊断值**；
 *                      成功后清 0，失败后保留，所以 UI 必须配合 online==0 看。
 *   g_at_tail        ：模块最近一次应答/OTA 阶段标记的 20 字摘要，纯诊断，
 *                      不参与任何控制流；UI 每 5ms 比较一次并刷新。
 *
 * 关系：
 *   g_ota_state == WIFI_CONNECTED  只是"可以开始 MQTT"的前置条件；
 *   g_mqtt_online == 1             才是"云端在线"；
 *   g_mqtt_fail_stage != NONE      且 online == 0 时，UI 才显示具体失败原因；
 *   g_at_tail 与上面三者没有因果，只反映"模块/OTA 最后说了什么"。
 *
 * 合法迁移（与 esp8266_ota.h 的枚举注释对应）：
 *   IDLE -> WIFI_CONNECTING -> WIFI_CONNECTED -> DOWNLOADING
 *                              ^         |
 *                              |         +-> 成功: 写标志 + NVIC_SystemReset()
 *                              |         +-> 失败: ERROR -> ESP8266_Init() -> WIFI_CONNECTED
 *                              +-- WiFi 失败: ERROR -> 5s -> WIFI_CONNECTING
 * ========================================================================== */

/* 网络/OTA 大阶段，供 UI 展示，也供 MQTTTask 等待 WiFi 就绪。
 * 重要：枚举值大小没有顺序语义（ERROR=5 > WIFI_CONNECTED=2），
 *       只能用 == / != 比较，禁止用 < / >。 */
volatile OTA_State_t g_ota_state = OTA_STATE_IDLE;

/* MQTT 是否处于"已连接且已订阅"状态，0=离线 1=在线。
 * 巴法云控制台显示的"订阅者 在线/离线"看的就是这条订阅，
 * 所以这个变量既用于屏幕显示，也用于决定要不要重连。
 * 注意：它和 g_ota_state 是**两个维度**；WIFI_CONNECTED + online=0 是正常组合。 */
volatile uint8_t g_mqtt_online = 0;

/* MQTT 最近一次失败卡在哪个阶段，含义见 MQTT_STAGE_xxx。
 * 屏幕会显示成 "MQTT: off noAT/noFW/noBroker/noSub"，
 * 不接串口也能判断是模块没应答、AT 固件不支持 MQTT、连不上 Broker，还是订阅被拒。
 * 它是粘滞值：成功后清 NONE，失败后保留；因此 UI 判断时必须加 online==0 前置。 */
volatile uint8_t g_mqtt_fail_stage = MQTT_STAGE_NONE;

/* 模块最近一次应答的可打印摘要（最多 20 字符 + '\0'），UI 直接显示。
 * 这是"模块到底回没回"的直接证据，不接串口也能判：
 *   "<no data>" = 本次抓拍时缓冲为空（不等于模块没应答，可能只是没抓到）
 *   "ERROR"     = 模块听懂了但拒绝（多见于 AT 固件不带 MQTT 指令集）
 *   "otaX:..."  = OTA 当前卡在哪一步（由 esp8266_ota.c 的 OTA_Mark 写入）
 * 写入者：ESP_DumpResp / OTA_Mark / OTA_MarkText / OTA 进度 / MQTT 成功态；
 * 读取者：UITask。它是诊断快照，不参与控制流。 */
char g_at_tail[22] = {0};

/* USER CODE END Variables */
/* Definitions for UITask */
osThreadId_t UITaskHandle;
const osThreadAttr_t UITask_attributes = {
  .name = "UITask",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for SensorTask */
osThreadId_t SensorTaskHandle;
const osThreadAttr_t SensorTask_attributes = {
  .name = "SensorTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityLow1,
};
/* Definitions for MQTTTask */
osThreadId_t MQTTTaskHandle;
const osThreadAttr_t MQTTTask_attributes = {
  .name = "MQTTTask",
  /* 2048 -> 4096：HTTP OTA 下载是在这个任务里同步跑的，
   * ESP8266_HTTP_OTA_Download 自身有 cmd_buf[256] + http_req[256]
   * 以及 snprintf/strstr 的调用开销，本任务还有 at_cmd[256]，
   * 2048 只剩不到 500 字节余量，收包期间一旦叠加串口中断嵌套就很危险。
   * FreeRTOS 堆还有 48KB，多给 2KB 换稳定 */
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for NetworkTask */
osThreadId_t NetworkTaskHandle;
const osThreadAttr_t NetworkTask_attributes = {
  .name = "NetworkTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for DogTask */
osThreadId_t DogTaskHandle;
const osThreadAttr_t DogTask_attributes = {
  .name = "DogTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityRealtime,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartUITask(void *argument);
void StartSensorTask(void *argument);
void StartMQTTTask(void *argument);
void StartNetworkTask(void *argument);
void StartDogTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationTickHook(void);

/* USER CODE BEGIN 3 */
void vApplicationTickHook( void )
{
   /* This function will be called by each tick interrupt if
   configUSE_TICK_HOOK is set to 1 in FreeRTOSConfig.h. User code can be
   added here, but the tick hook is called from an interrupt context, so
   code must not attempt to block, and only the interrupt safe FreeRTOS API
   functions can be used (those that end in FromISR()). */
  lv_tick_inc(1);

}
/* USER CODE END 3 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */

  //创建一个容量为4的sensordata对象的队列
  sensorQueueHandle = osMessageQueueNew(4,sizeof(SensorData_t),NULL);

  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of UITask */
  UITaskHandle = osThreadNew(StartUITask, NULL, &UITask_attributes);

  /* creation of SensorTask */
  SensorTaskHandle = osThreadNew(StartSensorTask, NULL, &SensorTask_attributes);

  /* creation of MQTTTask */
  MQTTTaskHandle = osThreadNew(StartMQTTTask, NULL, &MQTTTask_attributes);

  /* creation of NetworkTask */
  NetworkTaskHandle = osThreadNew(StartNetworkTask, NULL, &NetworkTask_attributes);

  /* creation of DogTask */
  DogTaskHandle = osThreadNew(StartDogTask, NULL, &DogTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartUITask */
/**
  * @brief  Function implementing the UITask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartUITask */
void StartUITask(void *argument)
{
  /* USER CODE BEGIN StartUITask */
  //初始化lvgl核心引擎与显示驱动
  lv_init();
  lv_port_disp_init();


  // 屏幕只有 128x160，关掉页面滚动，避免内容越界时冒出滑动条
  lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(lv_scr_act(), 0, 0);

 // 1. 设置背景
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x1B1F24), 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

  // 2. 标题（10 号字，128px 宽度刚好容纳这 17 个字符）
  //    版本号来自 app_config.h，改版本只改那一处
  lv_obj_t *title_label = lv_label_create(lv_scr_act());
  lv_label_set_text(title_label, "Edge Gateway " APP_VERSION_STR);
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(title_label, lv_color_hex(0x58A6FF), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 6);

  // 3. 硬件信息（拆成两行，整行放不下会横向溢出）
  uint16_t flash_id = W25Q64_ReadID();
  lv_obj_t *info_label = lv_label_create(lv_scr_act());
  char info_str[40];
  snprintf(info_str, sizeof(info_str), "Flash: 0x%04X\nRS485: ON", flash_id);
  lv_label_set_text(info_label, info_str);
  lv_obj_set_style_text_font(info_label, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_align(info_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(info_label, lv_color_hex(0x8B949E), 0);
  lv_obj_align(info_label, LV_ALIGN_TOP_MID, 0, 26);

  // 4. 温度标签（16 号，作为主读数突出显示）
  lv_obj_t *temp_label = lv_label_create(lv_scr_act());
  lv_label_set_text(temp_label, "T: --.-C");
  lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(temp_label, lv_color_hex(0xF85149), 0);
  lv_obj_align(temp_label, LV_ALIGN_TOP_MID, 0, 66);

  // 5. 湿度标签（16 号）
  lv_obj_t *humi_label = lv_label_create(lv_scr_act());
  lv_label_set_text(humi_label, "H: --.-%");
  lv_obj_set_style_text_font(humi_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(humi_label, lv_color_hex(0x3FB950), 0);
  lv_obj_align(humi_label, LV_ALIGN_TOP_MID, 0, 92);

  // 6. 网络/OTA 状态（独立标签，不再覆盖上面的硬件信息）
  lv_obj_t *status_label = lv_label_create(lv_scr_act());
  lv_label_set_text(status_label, "WiFi: idle");
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xD29922), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 130);

  // 7. 模块应答回显：不接串口也能看到 ESP8266 到底回了什么。
  //    "<NO REPLY>" = 模块一个字节都没回（供电/接线/波特率/固件死机）
  //    "ERROR"      = 模块听懂了但拒绝（最常见：AT 固件不带 MQTT 指令集）
  lv_obj_t *at_label = lv_label_create(lv_scr_act());
  lv_label_set_text(at_label, "AT: -");
  lv_obj_set_style_text_font(at_label, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(at_label, lv_color_hex(0x8B949E), 0);
  lv_obj_align(at_label, LV_ALIGN_TOP_MID, 0, 144);

  SensorData_t recv_data;
  char val_buf[32];
  uint32_t ui_loop_cnt = 0;
  uint32_t update_cnt = 0;

  /* Infinite loop */
  for(;;)
  {

    // 处理队列与 UI 刷新
    while (osMessageQueueGet(sensorQueueHandle, &recv_data, NULL, 0) == osOK)
    {
      if (recv_data.is_valid)
      {
        int t = (int)(recv_data.temperature * 10);
        int h = (int)(recv_data.humidity * 10);
        snprintf(val_buf, sizeof(val_buf), "T: %d.%dC", t / 10, t % 10);
        lv_label_set_text(temp_label, val_buf);

        snprintf(val_buf, sizeof(val_buf), "H: %d.%d%%", h / 10, h % 10);
        lv_label_set_text(humi_label, val_buf);
      }
      else
      {
        lv_label_set_text(temp_label, "T: ERR");
        lv_label_set_text(humi_label, "H: OFF");
      }
    }

    // 实时同步显示当前 Wi-Fi / MQTT / OTA 状态
    // 把 MQTT 在线状态单独列出来：WiFi 连上 ≠ 云端能看到设备，
    // 这两件事分开显示，才能一眼看出"离线"卡在哪一层
    static OTA_State_t last_ota_state   = OTA_STATE_IDLE;
    static uint8_t     last_mqtt_online = 0xFF;
    static uint8_t     last_fail_stage  = 0xFF;
    /* 失败阶段码也参与变化判据：一直 offline 时若只看 online 位，
     * 屏幕永远不刷新，就看不出是"模块没应答"还是"连不上 Broker" */
    if ((g_ota_state != last_ota_state) ||
        (g_mqtt_online != last_mqtt_online) ||
        (g_mqtt_fail_stage != last_fail_stage))
    {
        char st[28];

        last_ota_state   = g_ota_state;
        last_mqtt_online = g_mqtt_online;
        last_fail_stage  = g_mqtt_fail_stage;

        if ((g_mqtt_online == 0U) && (g_mqtt_fail_stage == MQTT_STAGE_NO_AT))
            /* 模块连 AT 都不回：这是链路层，和 MQTT/WiFi 配置都无关。
             * 必须加 online==0 前置：阶段码是"上一次失败"的残留，
             * 一旦重连成功它会被清成 NONE，但在清除之前的窗口里
             * 这个分支优先级最高，会把 "MQTT: online" 盖成 "ESP: NO AT!" */
            lv_label_set_text(status_label, "ESP: NO AT!");
        else if (g_ota_state == OTA_STATE_WIFI_CONNECTING)
            lv_label_set_text(status_label, "WiFi: connecting");
        else if (g_ota_state == OTA_STATE_DOWNLOADING)
            lv_label_set_text(status_label, "OTA: downloading");
        else if (g_ota_state == OTA_STATE_ERROR)
            lv_label_set_text(status_label, "WiFi/OTA: error");
        else if (g_ota_state == OTA_STATE_WIFI_CONNECTED)
        {
            if (g_mqtt_online)
            {
                lv_label_set_text(status_label, "MQTT: online");
            }
            else
            {
                /* 用短语代替纯数字：数字容易被误读，而且未定义值能一眼看出来。
                 * 出现 "errN" 就说明这个变量被内存越界写坏了（代码里只赋 0/2/3/4） */
                switch (g_mqtt_fail_stage)
                {
                  case MQTT_STAGE_NONE:    snprintf(st, sizeof(st), "MQTT: off try");      break;
                  case MQTT_STAGE_NO_AT:   snprintf(st, sizeof(st), "MQTT: off noAT");     break;
                  case MQTT_STAGE_USERCFG: snprintf(st, sizeof(st), "MQTT: off noFW");     break;
                  case MQTT_STAGE_CONN:    snprintf(st, sizeof(st), "MQTT: off noBroker"); break;
                  case MQTT_STAGE_SUB:     snprintf(st, sizeof(st), "MQTT: off noSub");    break;
                  default:                 snprintf(st, sizeof(st), "MQTT: err%u", (unsigned)g_mqtt_fail_stage); break;
                }
                lv_label_set_text(status_label, st);
            }
        }
    }

    /* 模块应答回显：只在内容变化时重设文本，避免每 5ms 刷一次屏幕 */
    static char last_at_tail[22] = "";
    if (strcmp(g_at_tail, last_at_tail) != 0)
    {
        char at_text[28];

        strncpy(last_at_tail, g_at_tail, sizeof(last_at_tail) - 1U);
        last_at_tail[sizeof(last_at_tail) - 1U] = '\0';

        snprintf(at_text, sizeof(at_text), "AT: %s", g_at_tail);
        lv_label_set_text(at_label, at_text);
    }

    // 这两行必须在 for 循环主干上，决不能套进 if 内部！
    lv_timer_handler();
    osDelay(5);
  
  }
  /* USER CODE END StartUITask */
}

/* USER CODE BEGIN Header_StartSensorTask */
/**
* @brief Function implementing the SensorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSensorTask */
void StartSensorTask(void *argument)
{
  /* USER CODE BEGIN StartSensorTask */

  Modbus_Init();
  SensorData_t sensor_data;

  //模拟调试标志：未连接物理设备可以生成仿真数据测试
  //有物理传感器可以改simulation改为0
  #define SIMULATION 1

  #if SIMULATION
  static float sim_temp = 25.5f;
  static float sim_humi = 50.0f;
  #endif


  /* Infinite loop */
  for(;;)
  {
#if SIMULATION
    sim_temp += 0.5f;
    if (sim_temp > 40.0f) {
      sim_temp = 20.0f;
    }

    sim_humi += 1.0f;
    if (sim_humi > 90.0f) {
      sim_humi = 40.0f;
    }

    sensor_data.temperature = sim_temp;
    sensor_data.humidity = sim_humi;
    sensor_data.is_valid = true;
#else
    //真实数据
    KTH2R_ReadData(&sensor_data);
#endif

    //通过消息队列推送(非阻塞)
    if(sensorQueueHandle != NULL)
    {
      osMessageQueuePut(sensorQueueHandle,&sensor_data,0,0);

    }

    //1000ms采集一次
    osDelay(1000);

  }
  /* USER CODE END StartSensorTask */
}

/* ==================================================================
 * 巴法云 MQTT 连接辅助
 *
 * 关键点：ESP-AT 回的 "OK" 只代表"指令格式被接受"，连接/订阅的结果是
 * 随后异步打印的事件：+MQTTCONNECTED / +MQTTSUB:...
 * 只等 OK 就往下走，SUB 往往在 TCP 尚未就绪时被拒，而原代码不检查、不重试，
 * 巴法云端看到的就是一直"订阅者 离线"。
 * ================================================================== */

/* ================================================================== *
 * AT 层调试输出：全部走 USART1(115200)，阻塞发送。
 * 任务里发几十字节 ≈ 几毫秒，不影响 1s 的轮询周期，
 * 但省掉了 printf 重定向带来的 semihosting 卡死风险。
 * ================================================================== */
static void ESP_Log(const char *s)
{
  if (s == NULL) return;
  HAL_UART_Transmit(&AT_LOG_UART, (uint8_t *)s, (uint16_t)strlen(s), 50);
}

/* 把模块最近一次原始应答打到调试口（截前 192 字节）。
 * 里面带 \r\n 也无所谓，串口助手里原样换行，正好看清模块到底回了什么。
 * 同时截一份"可打印副本"给屏幕用：不接串口也能看到模块到底回没回、
 * 回的是 OK 还是 ERROR —— 这是区分"模块沉默"和"模块拒绝"的唯一直接证据 */
static void ESP_DumpResp(void)
{
  uint16_t n = ESP8266_GetRxLen();
  uint16_t i;
  uint16_t start;
  uint8_t  k = 0;

  /* 取缓冲区【尾部】而不是开头。
   * 失败时真正要看的永远是"模块最后回了什么"：
   *   - AT+CWJAP 超时 15 秒，期间模块在自动重连，前面的内容全是旧日志，
   *     取开头只会看到一堆过期的 WIFI CONNECTED，看不到最后那条应答
   *   - 缓冲可能涨到几 KB，开头 192 字节和当前故障毫无关系 */
  start = (n > 192U) ? (uint16_t)(n - 192U) : 0U;

  ESP_Log("\r\n[AT<] ");
  if (n > start)
  {
    HAL_UART_Transmit(&AT_LOG_UART, &esp_rx_buf[start], (uint16_t)(n - start), 100);
  }
  ESP_Log("\r\n");

  /* 取尾部 20 个可打印字符给屏幕，控制字符折叠成一个空格 */
  for (i = start; (i < n) && (k < 20U); i++)
  {
    uint8_t c = esp_rx_buf[i];

    if ((c >= 0x20U) && (c < 0x7FU))
    {
      g_at_tail[k++] = (char)c;
    }
    else if ((k > 0U) && (g_at_tail[k - 1U] != ' '))
    {
      g_at_tail[k++] = ' ';
    }
  }

  if (k == 0U)
  {
    /* 这里只能说明"调用这一刻缓冲区是空的"，不能等同于"模块没应答"。
     * 原因有两条：ESP8266_SendCmd 一进函数就 ClearBuf，应答也还没到就被抓拍；
     * 而且 DumpResp 只在少数失败路径被调用，成功路径根本不刷新这个快照。
     * 原来写成 "<NO REPLY>" 会把"没抓到"误读成"模块哑了" —— 明明 MQTT 已经
     * 在线（USERCFG/CONN/SUB 全回了 OK），屏幕却一直挂着 NO REPLY。
     * 改成 <no data> 表示"本次抓拍为空"，真正的无应答由 g_mqtt_fail_stage
     * 的 NO_AT 判定，两者不再混为一谈 */
    g_at_tail[0] = '<'; g_at_tail[1] = 'n'; g_at_tail[2] = 'o';
    g_at_tail[3] = ' '; g_at_tail[4] = 'd'; g_at_tail[5] = 'a';
    g_at_tail[6] = 't'; g_at_tail[7] = 'a'; g_at_tail[8] = '>';
    g_at_tail[9] = '\0';
  }
  else
  {
    g_at_tail[k] = '\0';
  }
}

/* 打印一个十进制诊断值（串口错误计数等） */
static void ESP_LogNum(const char *tag, uint32_t v)
{
  char b[40];

  snprintf(b, sizeof(b), "%s%u\r\n", tag, (unsigned)v);
  ESP_Log(b);
}

/* 在接收缓冲区里等两个关键字之一出现（传 NULL 表示不关心该关键字） */
static bool ESP_Wait_Any(const char *tok0, const char *tok1, uint32_t timeout_ms)
{
  uint32_t start = osKernelGetTickCount();

  while ((osKernelGetTickCount() - start) < timeout_ms)
  {
    if ((tok0 != NULL) && (strstr((const char *)esp_rx_buf, tok0) != NULL)) return true;
    if ((tok1 != NULL) && (strstr((const char *)esp_rx_buf, tok1) != NULL)) return true;
    osDelay(10);
  }
  return false;
}

/* 发一条 AT 指令并判别结果：
 *   1 = 收到期望应答   2 = 模块回 ERROR（被拒）   0 = 超时无应答
 * 比 ESP8266_SendCmd 多一个"被拒"的区分，排查时能分清是没反应还是被拒绝 */
static uint8_t ESP_Cmd_Check(const char *cmd, const char *ack, uint32_t timeout_ms)
{
  const char *p_ack;
  const char *p_err;

  ESP_Log("[AT>] ");
  ESP_Log(cmd);

  ESP8266_SendCmd(cmd, NULL, 0);       /* 只发不等：内部先清缓冲，然后立即返回 */

  if (!ESP_Wait_Any(ack, "ERROR", timeout_ms))
  {
    ESP_Log("[AT<] TIMEOUT, no response from module\r\n");
    ESP_DumpResp();
    return 0U;
  }

  /* 两者都出现时按"谁在前"判定。
   * 原来只判断 ack 是否存在，一旦缓冲里残留了上一条指令的 ERROR，
   * 这条明明成功也会被判成失败 —— MQTT 会一直重连不上 */
  p_ack = strstr((const char *)esp_rx_buf, ack);
  p_err = strstr((const char *)esp_rx_buf, "ERROR");

  if ((p_ack != NULL) && ((p_err == NULL) || (p_ack < p_err)))
  {
    ESP_Log("[AT<] OK\r\n");
    return 1U;
  }

  ESP_Log("[AT<] ERROR\r\n");
  ESP_DumpResp();
  return 2U;
}

/* 完整的"连接 + 订阅"，返回 true 表示已连上且已订阅。
 *
 * 调用上下文：
 *   - 只从 MQTTTask 调用（g_mqtt_online == 0 时），不与其他任务共享；
 *   - 内部全是阻塞式 AT 等待，最长 MQTT_CONN_TIMEOUT + 若干次重试；
 *   - 调用期间 USART3 归 MQTTTask 独占，OTA 不可能同时进来。
 *
 * 对全局变量的副作用：
 *   成功 -> g_mqtt_fail_stage = NONE；esp_rx_buf 里保留最后应答；
 *           调用方（StartMQTTTask）再把 g_mqtt_online 置 1；
 *   失败 -> 只设置 g_mqtt_fail_stage（USERCFG/CONN/SUB），不碰 online；
 *           由调用方决定 3s 后重试。
 */
static bool MQTT_Connect_And_Subscribe(void)
{
  char cmd[256];
  uint8_t i;

  ESP_Log("\r\n==== MQTT connect ====\r\n");
  /* 每轮重连都报一次串口错误计数：
   * 这个数一直在涨，说明 USART3 反复溢出（模块回得快、CPU 忙），
   * 那 MQTT 连不上就不是配置问题，而是接收链路在丢数据 */
  ESP_LogNum("[UART3] err cnt = ", g_uart3_err_cnt);

  /* 先清掉上一次的客户端实例。巴法云同一个 UID 只允许一个客户端在线，
   * 残留连接会把新连接顶掉，现象就是控制台反复"离线" */
  ESP8266_SendCmd("AT+MQTTCLEAN=0\r\n", "OK", 1000);
  /* 给模块 300ms 真正释放 link 0。MQTTCLEAN 回 OK 只代表命令收到，
   * 立刻发 USERCFG 会偶发 ERROR（配置项还没解绑完） */
  osDelay(300);

  /* 1. 客户端参数：ClientID 必须填私钥(UID)，用户名/密码留空。
   *
   * 这两条指令名不通用，刷的固件不同就得用不同的：
   *   ESP-AT v2.x（ESP-12F/ESP32 官方固件） -> AT+MQTTUSERCFG
   *   安信可 MQTT 固件 1471（ESP-01S 1MB）  -> AT+MQTTCFG
   * 先试前者，被拒再试后者。两种固件都能跑，以后换模块不用改代码，
   * 也省掉"刷完发现指令名不对、又连不上"的一轮来回 */
  for (i = 0; i < 2U; i++)
  {
    const char *verb = (i == 0U) ? "AT+MQTTUSERCFG" : "AT+MQTTCFG";
    uint8_t try_n;

    snprintf(cmd, sizeof(cmd),
             "%s=0,1,\"%s\",\"\",\"\",0,0,\"\"\r\n", verb, BEMFA_UID);

    for (try_n = 0; try_n < 2U; try_n++)
    {
      if (ESP_Cmd_Check(cmd, "OK", 1500U) == 1U)
      {
        break;
      }
      osDelay(300);
    }

    if (try_n < 2U)
    {
      ESP_Log("[MQTT] user-cfg accepted\r\n");
      break;
    }
    ESP_Log("[MQTT] cfg verb rejected, try the other name\r\n");
  }

  if (i == 2U)
  {
    /* 两种指令名都被拒：这个 AT 固件确实不带 MQTT 指令集。
     * 这是硬门槛（AT 1.7.4.0 就是这种），代码改不出来，必须刷 MQTT 固件 */
    g_mqtt_fail_stage = MQTT_STAGE_USERCFG;
    ESP_Log("[MQTT] USERCFG/MQTTCFG both rejected -> fw has NO MQTT\r\n");
    return false;
  }

  /* 2. 连 Broker：等 +MQTTCONNECTED，不能只等 OK
   * （OK 只表示指令被接受，真正连上要等异步事件） */
  snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%u,1\r\n",
           MQTT_BROKER_ADDR, (unsigned)MQTT_BROKER_PORT);
  ESP_Log("[AT>] ");
  ESP_Log(cmd);
  ESP8266_SendCmd(cmd, NULL, 0);
  if (!ESP_Wait_Any("+MQTTCONNECTED", NULL, MQTT_CONN_TIMEOUT))
  {
    g_mqtt_fail_stage = MQTT_STAGE_CONN;
    ESP_Log("[MQTT] wait +MQTTCONNECTED timeout\r\n");
    ESP_DumpResp();
    return false;      /* UID 被占用 / 网络不通 / 端口写错，都会落到这里 */
  }
  ESP_Log("[MQTT] +MQTTCONNECTED\r\n");

  /* 3. 订阅主题：巴法云就是靠这条订阅把设备判为"在线" */
  snprintf(cmd, sizeof(cmd), "AT+MQTTSUB=0,\"%s\",0\r\n", BEMFA_TOPIC);
  if ((ESP_Cmd_Check(cmd, "OK", 3000U) != 1U) &&
      (strstr((const char *)esp_rx_buf, "+MQTTSUB:") == NULL))
  {
    g_mqtt_fail_stage = MQTT_STAGE_SUB;
    ESP_Log("[MQTT] SUB rejected\r\n");
    return false;      /* 部分 AT 版本订阅成功只回 +MQTTSUB，不回 OK */
  }

  g_mqtt_fail_stage = MQTT_STAGE_NONE;
  /* 把 AT 应答快照也刷成成功态。
   * g_at_tail 只在 ESP_DumpResp() 里更新，而那只在失败路径被调用 ——
   * 一旦某次失败留下旧内容，之后所有指令都成功也不会再刷新，
   * 屏幕第二行就永远冻在历史残影上。这里显式改写成 OK */
  g_at_tail[0] = 'O'; g_at_tail[1] = 'K'; g_at_tail[2] = '\0';
  ESP_Log("[MQTT] subscribed, online\r\n");
  return true;
}

/* USER CODE BEGIN Header_StartMQTTTask */
/**
* @brief Function implementing the MQTTTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartMQTTTask */
void StartMQTTTask(void *argument)
{
  /* USER CODE BEGIN StartMQTTTask */

  char at_cmd[256];
  uint32_t last_resub_tick = 0;

  /* 等 NetworkTask 把 WiFi 连上。
   * 原条件写的是 g_ota_state < WIFI_CONNECTED，但 ERROR 是枚举里最后一个值(=5)，
   * 失败时反而判定为"已就绪"，属于条件写反 —— 这里显式等 CONNECTED */
  while (g_ota_state != OTA_STATE_WIFI_CONNECTED)
  {
    osDelay(500);
  }

  ESP_Log("\r\n[NET] WiFi ready, start MQTT\r\n");

  /* 开机先报 AT 固件版本。ESP8266 有一批很老的 AT 固件（AT v0.21 一类）
   * 根本不含 MQTT 指令集，那种模块发 AT+MQTTUSERCFG 只会回 ERROR，
   * 代码再怎么改也连不上。这一行就是判断"是不是硬门槛"的直接证据 */
  ESP_Log("[AT>] AT+GMR\r\n");
  /* 上电后 ESP-AT 会自动重连已保存的 AP，串口这时候在吐 WIFI CONNECTED /
   * +STA_CONNECTED 这类主动上报，1000ms 等不到 OK 很常见。
   * 等不到就再发一次，避免把"模块正忙"误判成"模块哑了" */
  if (!ESP8266_SendCmd("AT+GMR\r\n", "OK", 2500))
  {
    ESP8266_SendCmd("AT+GMR\r\n", "OK", 2500);
  }
  ESP_DumpResp();

  /* ================= 本任务主循环的职责顺序（每轮约 1s） =================
   *   0. 若 g_mqtt_online == 0：完整重连 + 订阅；失败 3s 后重试
   *   1. 检查 +MQTTSUBRECV + "upgrade"：触发 OTA（同步阻塞，独占 USART3）
   *   2. 检查 +MQTTDISCONNECTED：置离线，下一轮重连
   *   3. 每 30s 补订阅（幂等，兜住模块静默重连后订阅丢失）
   *   4. 缓冲 > 3/4：清空，防止 esp_rx_len 封顶后再也收不到数据
   *
   * 顺序不能换：必须"先确保在线，再处理 OTA/断线/补订阅"；
   * OTA 分支最后 continue，避免同轮继续跑补订阅去抢串口。
   * ==================================================================== */
  for(;;)
  {
    /* 0. 未在线就先连接 + 订阅。连不上就 3 秒后重试，
     *    状态一直显示 "MQTT: offline"，方便判断卡在哪一步 */
    if (g_mqtt_online == 0U)
    {
      if (MQTT_Connect_And_Subscribe())
      {
        g_mqtt_online    = 1U;
        last_resub_tick  = osKernelGetTickCount();
      }
      else
      {
        ESP_Log("[MQTT] connect failed, retry in 3s\r\n");
        osDelay(3000);
        continue;
      }
    }

    /* ===================== OTA 触发点 =====================
     *
     * 云端往订阅主题发一条 "upgrade"，模块会主动上报：
     *   +MQTTSUBRECV:0,"<topic>",<len>,upgrade
     *
     * 【为什么在这里做 OTA，而不是单开一个任务】
     * ESP-01S 只有一路 UART，MQTT 心跳、AT 指令、HTTP 下载**共用同一个串口**。
     * 如果 OTA 放独立任务，就必须和 MQTT 任务抢串口 —— 那要加互斥锁、
     * 要处理"发到一半被插队"的问题，复杂度陡增。
     * 放在 MQTT 任务里同步执行，反而天然获得了**串口独占**：
     * 任务被 OTA 占住的这段时间，保活、补订阅、缓冲清理全都不会跑。
     *
     * 【代价】
     * 下载期间 MQTT 不保活，云端会显示离线 —— 这是可接受的，
     * 因为升级本来就该断开业务连接。失败后会主动重连回来。
     *
     * 【状态流转】
     *   WIFI_CONNECTED --收到 upgrade--> DOWNLOADING
     *        |                                |
     *        |                       成功: 写标志 + NVIC_SystemReset()
     *        |                       失败: ERROR -> ESP8266_Init() -> WIFI_CONNECTED
     *        +--------------------------------+
     */
    /* 收到消息时，ESP8266 会主动上报：
     *   +MQTTSUBRECV:0,"<topic>",<len>,upgrade
     *
     * 注意：这里用 strstr 直接扫 esp_rx_buf，依赖“缓冲区末尾可读且最终有 0”
     * 这一隐含前提（ClearBuf 会 memset，ISR 最多写到 2046）。
     * OTA 解析器内部已经改成带长度的 OTA_BufFind；如果以后要严格化，
     * 这里也应换成“按 esp_rx_len 做有界查找”，避免匹配到已消费的旧报文。 */
    if ((strstr((const char *)esp_rx_buf, "+MQTTSUBRECV:") != NULL) &&
        (strstr((const char *)esp_rx_buf, "upgrade") != NULL))
    {
      /* 第一步：主动断开 MQTT，把 link 0 和串口彻底让出来。
       * 顺序不能反 —— 必须先 ClearBuf 再发指令，否则缓冲里的
       * 旧数据会被当成 MQTTCLEAN 的应答（SendCmd 内部也会清一次，这里是双保险） */
      ESP_8266_ClearBuf();
      ESP8266_SendCmd("AT+MQTTCLEAN=0\r\n", "OK", 1000);
      g_mqtt_online = 0U;

      /* 给模块 500ms 收尾。MQTTCLEAN 回 OK 只代表"命令被接受"，
       * 底层那条 TCP 往往还在关闭过程中。立刻发下一条 AT 会偶发 ERROR，
       * 表现为 OTA 在第一步（CIPMODE/CIPMUX）就退出 */
      osDelay(500);

      /* 打一个起点标记：屏幕第二行会显示 "AT: ota0:begin"。
       * 之后每一步失败都会改写它（ota1..ota8），没接串口也能定位卡在哪一步。
       * 这是整个调试体系的第一层：设备状态外显 */
      snprintf(g_at_tail, sizeof(g_at_tail), "ota0:begin");

      //切换状态并触发HTTP OTA下载
      g_ota_state = OTA_STATE_DOWNLOADING;
      /* 服务器地址、端口、固件路径统一由 app_config.h 提供。
       *
       * 路径是固定名 "/A_Project.bin"，**不带版本号** ——
       * 这是"只推不烧"的关键：设备永远请求同一个 URL，
       * 服务端覆盖 ota_release/A_Project.bin 即生效。
       * 早期用的是 "/A_Project_v3.0.bin" 这种带版本号的名字，
       * 每发一版都要改这一行 + 重编译 + 重烧基线固件，
       * 那就不是远程升级，只是换了个方式烧录。 */
      if (!ESP8266_HTTP_OTA_Download(OTA_SERVER_IP, OTA_SERVER_PORT, OTA_FIRMWARE_PATH))
      {
        /* 下载失败不会复位，要把串口从 HTTP 流程里收回来再重连 MQTT，
         * 否则模块可能停在半连接状态（link 开着、CIPMODE 被自动清 0），
         * 之后所有 AT 指令都无响应。
         * 注意这里**不清 OTA 标志** —— 标志只在下载成功后才写，
         * 失败路径从来没写过，所以 W25Q64 里不会有半成品，
         * 下次上电 Bootloader 读不到合法标志，照样跳旧 APP */
        g_ota_state = OTA_STATE_ERROR;
        ESP8266_Init();
        g_ota_state = OTA_STATE_WIFI_CONNECTED;
      }
      g_mqtt_online = 0U;
      continue;
    }

    /* 断线：ESP-AT 会打印 +MQTTDISCONNECTED。模块自己会重连 TCP，
     * 但不会恢复订阅，所以这里让状态归零，由第 0 步做一次完整重连+重订阅 */
    if (strstr((const char *)esp_rx_buf, "+MQTTDISCONNECTED") != NULL)
    {
      ESP_8266_ClearBuf();
      g_mqtt_online = 0U;
      ESP_Log("[MQTT] broker disconnected\r\n");
      osDelay(1000);
      continue;
    }

    /* 保活补订阅：每 30 秒重发一次 SUB。
     * SUB 本身是幂等的，却能兜住"模块静默重连后订阅丢失"这类情况，
     * 巴法云只认订阅者，订阅丢了就显示离线 */
    if ((osKernelGetTickCount() - last_resub_tick) > MQTT_RESUB_PERIOD)
    {
      last_resub_tick = osKernelGetTickCount();
      snprintf(at_cmd, sizeof(at_cmd), "AT+MQTTSUB=0,\"%s\",0\r\n", BEMFA_TOPIC);
      if ((ESP_Cmd_Check(at_cmd, "OK", 3000U) == 1U) ||
          (strstr((const char *)esp_rx_buf, "+MQTTSUB:") != NULL))
      {
        g_mqtt_online = 1U;
      }
      else
      {
        g_mqtt_online     = 0U;   /* 补订阅失败按掉线处理，下一轮重新连接 */
        g_mqtt_fail_stage = MQTT_STAGE_SUB;
        ESP_Log("[MQTT] resubscribe failed\r\n");
      }
    }

    /* 缓冲保护：esp_rx_len 涨到上限后串口中断就不再收字符了，
     * 表现是"设备还连着但再也收不到任何指令"，必须定期清 */
    if (ESP8266_GetRxLen() > (ESP_RX_BUF_SIZE * 3U / 4U))
    {
      ESP_8266_ClearBuf();
    }

    osDelay(1000);
  }
  /* USER CODE END StartMQTTTask */
}

/* USER CODE BEGIN Header_StartNetworkTask */
/**
* @brief Function implementing the NetworkTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartNetworkTask */
void StartNetworkTask(void *argument)
{
  /* USER CODE BEGIN StartNetworkTask */

  //等待电源稳定后初始化esp8266
  osDelay(1000);
  g_ota_state = OTA_STATE_WIFI_CONNECTING;
  ESP8266_Init();

  /* 上电第一次握手：AT 不通就别往联网流程里走。
   * 这一步专门用来区分"链路层问题"和"MQTT 配置问题"：
   *   模块不回 AT  -> 供电/接线/波特率/模块已死（noAT）
   *   模块回 AT    -> 链路没问题，后面的失败才可能是固件或云端配置
   * ESP-01S 峰值电流 300mA 以上，USB 供电或板载 LDO 带不动时，
   * 表现就是 WiFi 偶尔能连上、随后彻底沉默 */
  while (!ESP8266_SendCmd("AT\r\n", "OK", 1000))
  {
    g_mqtt_fail_stage = MQTT_STAGE_NO_AT;
    ESP_Log("[NET] AT no response -> check 3V3 / TX-RX / baud\r\n");
    ESP_DumpResp();
    osDelay(1000);
    ESP8266_Init();
  }
  g_mqtt_fail_stage = MQTT_STAGE_NONE;
  ESP_Log("[NET] AT ok\r\n");

  if (ESP8266_JoinAP("emp", "ay666888"))
  {
    g_ota_state =OTA_STATE_WIFI_CONNECTED;
    //osDelay(2000);
    //g_ota_state =OTA_STATE_DOWNLOADING;
    //if (ESP8266_HTTP_OTA_Download("192.168.137.13",80,"/firmware.bin"))
    //{
    //  g_ota_state = OTA_STATE_VERIFY_OK;
    //}
    //else
    //{
    //  g_ota_state = OTA_STATE_ERROR;
    //}  
  }
  else
  {
    /* 连不上必须重试。原来失败一次就置 ERROR 然后永久停住，
     * 路由器重启/信号弱一时连不上，设备就再也回不来，只能手动复位 */
    ESP_Log("[NET] JoinAP failed\r\n");
    /* 失败原因必须打出来。ESP8266_JoinAP 内部是 CWMODE -> CWJAP 两条指令，
     * 哪一条失败、模块回的是 ERROR 还是 busy p... 还是根本没有应答，
     * 看这一行原文就能区分，不用猜。ESP-AT 忙的时候会回 "busy p..."，
     * 那是"指令排队满了"而不是"配网失败"，两者处理方式完全不同 */
    ESP_DumpResp();

    for (;;)
    {
      g_ota_state = OTA_STATE_ERROR;
      osDelay(5000);
      g_ota_state = OTA_STATE_WIFI_CONNECTING;

      if (ESP8266_JoinAP("emp", "ay666888"))
      {
        g_ota_state = OTA_STATE_WIFI_CONNECTED;
        break;
      }

      ESP_Log("[NET] JoinAP retry failed\r\n");
      ESP_DumpResp();
    }
  }


  /* Infinite loop */
  for(;;)
  {
    // 当联网成功且收到升级触发条件时，执行 HTTP OTA 下载
    // 测试时，你可以定义一个标志位 trigger_ota = 1 来触发
    osDelay(1000);
  }
  /* USER CODE END StartNetworkTask */
}

/* USER CODE BEGIN Header_StartDogTask */
/**
* @brief Function implementing the DogTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartDogTask */
void StartDogTask(void *argument)
{
  /* USER CODE BEGIN StartDogTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1000);
  }
  /* USER CODE END StartDogTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

