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


// 任务事件位定义
#define TASK_EVENT_BIT_COLLECT_RUNNING   (1U << 0)
// 存储就绪事件位
#define TASK_EVENT_BIT_STORAGE_READY     (1U << 1)
// 队列背压丢包事件位
#define TASK_EVENT_BIT_QUEUE_DROPPED     (1U << 2)


// 初始化任务管道资源
void TASK_InitPipelineResources(void);
// 创建所有任务
void TASK_CreateAllTasks(void);


// 获取当前活动任务ID
uint32_t TASK_GetActiveTaskId(void);
// 设置当前活动任务ID
void TASK_SetActiveTaskId(uint32_t task_id);


// 获取采集任务是否启用
bool TASK_GetCollectEnabled(void);
// 设置采集任务是否启用
void TASK_SetCollectEnabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* TASK_TASK_MANAGER_H_ */
