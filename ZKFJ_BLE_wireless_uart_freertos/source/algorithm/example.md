# 构建RTOS内部通信管道（骨架搭建）

```txt
source/
├── bsp/                          # 底层驱动层（您已写完led, uart, wdog, crc, adc, can）
├── algorithm/                    # 算法层（滤器、温度/压力转换、CRC算法、CAN解码）
├── app/                          # APP业务逻辑层（纯业务，不直接创建RTOS任务循环）
│   ├── app_global.h              # 🌟 新增：全局控制命令定义、整机数据结构体对齐
│   ├── app_storage.h / .c        # 本地 LittleFS 存储业务（记录 meta.bin/data.bin）
│   └── app_transfer.h / .c       # 历史数据大包流控、蓝牙分包组织业务
└── task/                         # 🌟 TASK任务层（直接对接 FreeRTOS，运行 Task 循环）
    ├── task_manager.h / .c       # 核心：负责一键创建 5 大任务，初始化全局队列与事件组
    ├── task_sensor_collect.c     # 任务 1：温压+液位 周期同步采集任务实体
    ├── task_storage.c            # 任务 2：闪存异步背压存储任务实体
    ├── task_ble_business.c       # 任务 3：蓝牙命令解析与大包分包发送任务实体
    └── task_led.c                # 任务 4：根据系统事件组切换动态闪烁的 LED 任务实体
                                  # (注：看门狗守护任务可直接借用您写好的 app_wdog.c/h 挂载，但是目前app_wdog.c/h还没写，不着急)
```

| **任务名称**                            | **优先级**           | **触发机制**                   | **职责描述**                                                 |
| --------------------------------------- | -------------------- | ------------------------------ | ------------------------------------------------------------ |
| **`vWatchdogGuardTask`** (看门狗守护)   | **最高** (e.g., `5`) | 定时唤醒 (2秒)                 | 检查其他任务的健康标志位，全员到齐则喂狗，否则拒绝喂狗让芯片复位自救。 |
| **`vBleTask`** (蓝牙协议与通信)         | **高** (e.g., `4`)   | 事件驱动 (信号量/消息)         | 负责运行 NXP 蓝牙协议栈底层的消息泵（`BluetoothLEHost_HandleMessages`），并解析手机 APP 发来的读写指令。 |
| **`vSensorCollectTask`** (周期采样同步) | **中** (e.g., `3`)   | 时间驱动 (`vTaskDelayUntil`)   | 按照固定频率（如 1 秒）同时触发 ADC（温压）和 CAN（液位）读取，打包对齐后塞入存储队列。 |
| **`vLedTask`** (状态灯动态指示)         | **低** (e.g., `2`)   | 定时/事件控制                  | 负责整机的闪烁指示。例如：常规采集中慢闪（绿灯），蓝牙传输中快闪（蓝灯），存储异常常亮（红灯）。 |
| **`vStorageTask`** (闪存异步背压存储)   | **最低** (e.g., `1`) | 异步队列驱动 (`xQueueReceive`) | 挂起死等采样队列。一旦有数据，被动唤醒并一气呵成存入 LittleFS，防止 Flash 擦写卡顿影响采集实时性。 |

### 二、 核心文件详细设计与落地代码

为了让您能够直接写出骨架，下面为您奉上最核心的 3 个骨架文件设计。

#### 1. APP层基石：`app_global.h`（解决数据对齐与状态定义）

该文件用来存放全机通用的数据结构。**采集、存储、蓝牙、LED 任务都需要包这个头文件。**

C

```
#ifndef APP_APP_GLOBAL_H_
#define APP_APP_GLOBAL_H_

#include <stdint.h>

#if defined(__GNUC__)
#define APP_PACKED __attribute__((packed))
#else
#define APP_PACKED
#endif

/**
 * @brief 🌟 核心：长时间采集的单次标准记录（严格12字节对齐，禁止变长）
 */
typedef struct APP_PACKED
{
    uint32_t timestamp;     // 采样时间戳（系统运行 ticks 或 RTC 绝对秒数）
    int16_t  temperature;   // 转换后的温度（如扩大100倍的整数，2534 代表 25.34℃）
    uint16_t pressure;      // 转换后的压力（如单位 kPa）
    uint16_t liquid_level;   // CAN解析出的液位值
    uint8_t  status;        // 诊断状态掩码（0x00正常，0x01传感器错，0x02闪存快满）
    uint8_t  reserved;      // 保留填充位，确保结构体总长度为 12 字节（32位芯片访存最高效）
} sensor_record_t;

/* 蓝牙/串口下发的系统控制命令码 */
#define CMD_SYS_STOP_COLLECT     ((uint8_t)0x01U) // 停止采集
#define CMD_SYS_START_COLLECT    ((uint8_t)0x02U) // 开始采集
#define CMD_SYS_READ_HISTORY     ((uint8_t)0x03U) // 读取历史数据
#define CMD_SYS_CLEAR_FLASH      ((uint8_t)0x04U) // 格式化闪存

#endif /* APP_APP_GLOBAL_H_ */
```

#### 2. TASK层总管：`task_manager.h` 与 `task_manager.c`

这个文件是整个 FreeRTOS 骨架的“启动总开关”，它负责创建所有管道资源和 5 大任务实体。

**`task_manager.h`**

C

```
#ifndef TASK_TASK_MANAGER_H_
#define TASK_TASK_MANAGER_H_

#include "FreeRTOS.h"
#include "queue.h"
#include "event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 全局跨任务通信资源声明 */
extern QueueHandle_t      g_sensor_data_queue;
extern EventGroupHandle_t g_system_event_group;

/* 全局控制系统状态的事件 Bit 位定义 */
#define EVENT_BIT_COLLECT_RUNNING   (1U << 0) // 是否处于允许采集状态
#define EVENT_BIT_BLE_CONNECTED      (1U << 1) // 蓝牙是否处于连接状态
#define EVENT_BIT_BLE_SENDING_DATA   (1U << 2) // 蓝牙是否正在极速上传大块历史数据

/**
 * @brief 初始化所有的 RTOS 内部通信管道（队列、事件组）
 */
void TASK_InitPipelineResources(void);

/**
 * @brief 一键拉起系统 5 大核心任务
 */
void TASK_CreateAllTasks(void);

#ifdef __cplusplus
}
#endif
#endif /* TASK_TASK_MANAGER_H_ */
```

**`task_manager.c`**

C

```
#include "task_manager.h"
#include "fsl_debug_console.h"

/* 实例化全局资源 */
QueueHandle_t      g_sensor_data_queue = NULL;
EventGroupHandle_t g_system_event_group = NULL;

/* 声明各独立文件中的任务函数实体（extern 导入） */
extern void vWatchdogGuardTask(void *pvParameters);
extern void vSensorCollectTask(void *pvParameters);
extern void vStorageTask(void *pvParameters);
extern void vBleBusinessTask(void *pvParameters);
extern void vLedIndicatorTask(void *pvParameters);

void TASK_InitPipelineResources(void)
{
    // 1. 创建传感器记录数据队列，深度20（可缓存20秒的数据）
    g_sensor_data_queue = xQueueCreate(20, 12); // 单个记录 12 字节
    
    // 2. 创建系统控制事件组
    g_system_event_group = xEventGroupCreate();
    
    // 3. 上电默认开启定时采集状态
    if (g_system_event_group != NULL)
    {
        (void)xEventGroupSetBits(g_system_event_group, EVENT_BIT_COLLECT_RUNNING);
    }
    
    PRINTF("[TASK] Pipeline Resources Initialized.\r\n");
}

void TASK_CreateAllTasks(void)
{
    /* 严格按照优先级高低，依次拉起 5 大任务 */
    
    // 1. 看门狗守护任务：最高优先级 (e.g. 5)
    (void)xTaskCreate(vWatchdogGuardTask, "Task_Wdog", 128, NULL, 5, NULL);
    
    // 2. 蓝牙业务任务：高优先级 (e.g. 4)
    (void)xTaskCreate(vBleBusinessTask, "Task_Ble", 512, NULL, 4, NULL);
    
    // 3. 传感器周期采集同步任务：中优先级 (e.g. 3)
    (void)xTaskCreate(vSensorCollectTask, "Task_Sensor", 256, NULL, 3, NULL);
    
    // 4. LED 状态状态灯任务：低优先级 (e.g. 2)
    (void)xTaskCreate(vLedIndicatorTask, "Task_Led", 128, NULL, 2, NULL);
    
    // 5. Flash 存储背压任务：最低优先级 (e.g. 1)
    (void)xTaskCreate(vStorageTask, "Task_Storage", 512, NULL, 1, NULL);

    PRINTF("[TASK] All 5 Core Tasks Created Successfully.\r\n");
}
```

#### 3. 核心业务协作实体：`task_sensor_collect.c`（ADC与CAN同步采集）

这个文件将直接指导如何在单任务循环内对齐 ADC 与 CAN 的数据，并扔入存储管道。

C

```
#include "task_manager.h"
#include "app_global.h"
#include "app_wdog.h"       // 包含看门狗签到定义
#include "bsp_adc.h"
#include "bsp_can.h"
#include "algo_temperature.h"
#include "algo_pressure.h"
#include "algo_can_decode.h"

void vSensorCollectTask(void *pvParameters)
{
    sensor_record_t current_record;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xxFrequency = pdMS_TO_TICKS(1000); // 严格固定频率：1秒钟采一次

    PRINTF("[TASK] Sensor Collect Task Started.\r\n");

    for (;;)
    {
        // 按照 1 秒频率精确定时唤醒任务
        vTaskDelayUntil(&xLastWakeTime, xxFrequency);

        // 检查事件组：系统当前是否允许采集？
        EventBits_t uxBits = xEventGroupGetBits(g_system_event_group);
        if ((uxBits & EVENT_BIT_COLLECT_RUNNING) != 0U)
        {
            /* 🌟 第一步：在同一时间戳下，同步采集所有硬件数据 */
            current_record.timestamp = (uint32_t)xTaskGetTickCount(); // 使用系统Ticks作为相对时间戳
            
            uint16_t adc_temp_raw = BSP_ADC_ReadChannel(ADC_CH_TEMP);
            uint16_t adc_pres_raw = BSP_ADC_ReadChannel(ADC_CH_PRESS);
            uint32_t can_level_raw = BSP_CAN_GetLastReceivedMsgId(); // 假设从接收缓存捞CAN数据
            
            /* 🌟 第二步：调用算法层进行物理值转换 */
            current_record.temperature  = ALGO_ConvertRawToTemperature(adc_temp_raw);
            current_record.pressure     = ALGO_ConvertRawToPressure(adc_pres_raw);
            current_record.liquid_level  = ALGO_DecodeCanLiquidLevel(can_level_raw);
            current_record.status       = 0x00U; // 默认健康
            current_record.reserved     = 0x00U;

            /* 🌟 第三步：将对齐打包好的 12 字节记录扔进背压存储队列（不等待，防止Flash卡顿株连采集） */
            if (pdPASS != xQueueSend(g_sensor_data_queue, &current_record, 0U))
            {
                // 队列满，说明 Flash 此时极度卡顿（发生了背压），做相应诊断记录
                PRINTF("[WARN] Storage queue full, record dropped!\r\n");
            }
        }

        /* 🌟 第四步：顺利跑完一轮，向看门狗高优先级守护任务签到 */
        APP_WDOG_TaskSign(APP_WDOG_BIT_SENSOR);
    }
}
```

### 三、 接下来如何让整个骨架运转起来？

1. **新建文件**：按照上面的规划，在您的工程中建立 `task_manager.c/.h`、`task_sensor_collect.c` 等 4 个任务实体文件。

2. **在 `main.c` 中调用挂载**： 在您原来的 `main.c` 中，找到 `start_task` 函数或拉起蓝牙前的地方，直接加入两行：

   C

   ```
   /* 在启动调度器前，初始化内部管道并将5大任务拉起来 */
   TASK_InitPipelineResources();
   TASK_CreateAllTasks();
   ```

