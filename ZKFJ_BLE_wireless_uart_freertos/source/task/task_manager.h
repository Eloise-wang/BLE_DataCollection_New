#ifndef TASK_TASK_MANAGER_H_
#define TASK_TASK_MANAGER_H_

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "event_groups.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// 传感器数据队列
extern QueueHandle_t g_sensor_data_queue;
// 系统事件组
extern EventGroupHandle_t g_system_event_group;

#ifndef TASK_SENSOR_QUEUE_DEPTH
#define TASK_SENSOR_QUEUE_DEPTH 60U
#endif

#ifndef TASK_STACK_WDOG
#define TASK_STACK_WDOG 256U
#endif

#ifndef TASK_STACK_PROTO
#define TASK_STACK_PROTO 512U
#endif

#ifndef TASK_STACK_UART_CMD
#define TASK_STACK_UART_CMD 384U
#endif

#ifndef TASK_STACK_SENSOR
#define TASK_STACK_SENSOR 384U
#endif

#ifndef TASK_STACK_STORAGE
#define TASK_STACK_STORAGE 1024U
#endif

#ifndef TASK_STACK_LED
#define TASK_STACK_LED 256U
#endif


// 任务事件位定义
#define TASK_EVENT_BIT_COLLECT_RUNNING   (1U << 0)
// 存储就绪事件位
#define TASK_EVENT_BIT_STORAGE_READY     (1U << 1)
// 队列背压丢包事件位
#define TASK_EVENT_BIT_QUEUE_DROPPED     (1U << 2)
#define TASK_EVENT_BIT_COLLECT_DONE      (1U << 3)
#define TASK_EVENT_BIT_BLE_CONNECTED     (1U << 4)
#define TASK_EVENT_BIT_HISTORY_SENDING   (1U << 5)
#define TASK_EVENT_BIT_COLLECTION_PULSE  (1U << 6)


// 初始化任务管道资源
void TASK_InitPipelineResources(void);
// 创建所有任务
void TASK_CreateAllTasks(void);


// 获取当前活动任务ID
uint64_t TASK_GetActiveTaskId(void);
// 设置当前活动任务ID
void TASK_SetActiveTaskId(uint64_t task_id);


// 获取采集任务是否启用
bool TASK_GetCollectEnabled(void);
// 设置采集任务是否启用
void TASK_SetCollectEnabled(bool enabled);

void TASK_StartCollect(uint64_t task_id,
                       uint32_t host_epoch_s,
                       uint32_t duration_s,
                       uint32_t period_ms,
                       bool time_valid,
                       uint32_t forced_total_count);
bool TASK_StopCollect(uint64_t task_id);

uint32_t TASK_GetTaskGeneration(void);

bool TASK_IsPretest(void);

uint32_t TASK_GetCollectPeriodMs(void);
uint32_t TASK_GetCollectDurationS(void);
uint32_t TASK_GetCollectTotalCount(void);
uint32_t TASK_GetCollectRemainingCount(void);
uint32_t TASK_ConsumeOneSample(void);

bool TASK_GetTimeBase(uint32_t *host_epoch_s, uint32_t *local_base_ms);
uint32_t TASK_GetLocalBaseMs(void);

void TASK_MarkCollectDone(void);
bool TASK_IsCollectDone(void);

void TASK_SetBleConnected(bool connected);
void TASK_SetHistorySending(bool sending);
void TASK_RequestCollectionPulse(void);

#ifdef __cplusplus
}
#endif

#endif /* TASK_TASK_MANAGER_H_ */
