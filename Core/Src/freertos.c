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
#include "modbus_rtu.h"'
#include "esp8266_ota.h"
#include <stdio.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

// void vApplicationTickHook(void)
// {
//     lv_tick_inc(1);
// }

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

osMessageQueueId_t sensorQueueHandle; //传感器数据队列句柄
//全局状态，供给ui展示联网和升级state
volatile OTA_State_t g_ota_state = OTA_STATE_IDLE;

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
  .stack_size = 512 * 4,
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
  //启动网络任务处理eso8266与远程OTA
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


 // 1. 设置背景
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x1B1F24), 0);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

  // 2. 标题
  lv_obj_t *title_label = lv_label_create(lv_scr_act());
  lv_label_set_text(title_label, "Edge Gateway V2.0");
  lv_obj_set_style_text_color(title_label, lv_color_hex(0x58A6FF), 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

  // 3. 状态标签
  uint16_t flash_id = W25Q64_ReadID();
  lv_obj_t *status_label = lv_label_create(lv_scr_act());
  char status_str[32];
  snprintf(status_str, sizeof(status_str), "Flash: 0x%04X | RS485: ON", flash_id);
  lv_label_set_text(status_label, status_str);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0x8B949E), 0);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 38);

  // 4. 温度标签
  lv_obj_t *temp_label = lv_label_create(lv_scr_act());
  lv_label_set_text(temp_label, "Temp: --.- C");
  lv_obj_set_style_text_color(temp_label, lv_color_hex(0xF85149), 0);
  lv_obj_align(temp_label, LV_ALIGN_CENTER, 0, -15);

  // 5. 湿度标签
  lv_obj_t *humi_label = lv_label_create(lv_scr_act());
  lv_label_set_text(humi_label, "Humi: --.- %");
  lv_obj_set_style_text_color(humi_label, lv_color_hex(0x3FB950), 0);
  lv_obj_align(humi_label, LV_ALIGN_CENTER, 0, 20);

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
        snprintf(val_buf, sizeof(val_buf), "Temp: %d.%d C", t / 10, t % 10);
        lv_label_set_text(temp_label, val_buf);

        snprintf(val_buf, sizeof(val_buf), "Humi: %d.%d %%", h / 10, h % 10);
        lv_label_set_text(humi_label, val_buf);
      }
      else
      {
        lv_label_set_text(temp_label, "Temp: Error");
        lv_label_set_text(humi_label, "Humi: Offline");
      }
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
  /* Infinite loop */
  for(;;)
  {
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

  if (ESP8266_JoinAP("emp", "ay666888"))
  {
      g_ota_state = OTA_STATE_WIFI_CONNECTED;
  }
  else
  {
      g_ota_state = OTA_STATE_ERROR;
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

