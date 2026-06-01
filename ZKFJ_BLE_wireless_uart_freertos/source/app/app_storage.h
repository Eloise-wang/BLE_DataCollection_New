/*
 * app_storage.h
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#ifndef APP_APP_STORAGE_H_
#define APP_APP_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define APP_STORAGE_PACKED __attribute__((packed))
#else
#define APP_STORAGE_PACKED
#endif

// 任务元数据结构体
typedef struct
{
    uint64_t task_id;                // 任务ID
    uint32_t start_timestamp_ms;     // 开始时间戳（毫秒）
    uint32_t sample_period_ms;       // 采样周期（毫秒）
    uint32_t duration_ms;            // 任务持续时间（毫秒）
    uint32_t record_size;            // 每个记录的大小（字节）
    uint32_t record_version;         // 记录版本号
} app_storage_task_meta_t;

typedef enum
{
    APP_STORAGE_PHASE_PRETEST = 0,
    APP_STORAGE_PHASE_FORMAL  = 1,
} app_storage_phase_t;

// 初始化存储
bool APP_Storage_Init(void);
bool APP_Storage_EraseAll(void);
// 开始任务
bool APP_Storage_BeginTask(uint64_t task_id, const app_storage_task_meta_t *meta);
bool APP_Storage_BeginTaskEx(uint64_t task_id, const app_storage_task_meta_t *meta, app_storage_phase_t phase);


// 追加写入数据
bool APP_Storage_AppendData(uint64_t task_id, const void *record, uint32_t record_size);
bool APP_Storage_AppendPreData(uint64_t task_id, const void *record, uint32_t record_size);
// 追加写入日志
bool APP_Storage_AppendLog(uint64_t task_id, const void *data, uint32_t size);
bool APP_Storage_LogPrintf(uint64_t task_id, const char *format, ...);

// 从日志文件偏移量读取数据
int APP_Storage_ReadLog(uint64_t task_id, uint32_t offset, void *out, uint32_t size);
// 获取日志大小
bool APP_Storage_GetLogSize(uint64_t task_id, uint32_t *out_size);

// 从文件偏移量读取数据
int APP_Storage_ReadData(uint64_t task_id, uint32_t offset, void *out, uint32_t size);
int APP_Storage_ReadPreData(uint64_t task_id, uint32_t offset, void *out, uint32_t size);
int APP_Storage_ReadMeta(uint64_t task_id, uint32_t offset, void *out, uint32_t size);

// 获取数据大小
bool APP_Storage_GetDataSize(uint64_t task_id, uint32_t *out_size);
bool APP_Storage_GetPreDataSize(uint64_t task_id, uint32_t *out_size);
bool APP_Storage_GetMetaSize(uint64_t task_id, uint32_t *out_size);

// 删除任务
bool APP_Storage_DeleteTask(uint64_t task_id);

// 遍历所有任务，返回找到的任务数量，out_ids 和 out_count 上限为 max_count
// 返回实际写入 out_ids 的任务数量（最大为 max_count）
uint8_t APP_Storage_ListTasks(uint64_t *out_ids, uint8_t max_count);

#ifdef __cplusplus
}
#endif

#endif /* APP_APP_STORAGE_H_ */
