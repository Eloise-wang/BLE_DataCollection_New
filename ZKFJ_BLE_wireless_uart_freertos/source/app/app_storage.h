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

// 采集状态（持久化于任务 header 的 reserved[1]）：
//   UNKNOWN   = 0xFFFFFFFF，空槽/旧固件遗留，视为不可续采
//   RUNNING   = 采集进行中，复位后需续采
//   COMPLETED = 已被 APP 停止或自然采完，不再续采（STOP 等同完成）
typedef enum
{
    APP_STORE_STATE_UNKNOWN   = 0xFFFFFFFFu,
    APP_STORE_STATE_RUNNING   = 1u,
    APP_STORE_STATE_COMPLETED = 2u,
} app_store_state_t;

// 进度回调类型，current=当前进度(1-100), 0=空闲/完成
typedef void (*app_storage_erase_progress_fn)(uint8_t progress_percent);

// 初始化存储
bool APP_Storage_Init(void);

// 擦除任务后台线程（调用此函数创建后台任务）
void APP_Storage_EraseTask(void *pvParameters);

// 擦除所有数据（异步，立即返回）
bool APP_Storage_EraseAllAsync(app_storage_erase_progress_fn callback);

// 删除单个任务（异步，立即返回）
bool APP_Storage_DeleteTaskAsync(uint64_t task_id, app_storage_erase_progress_fn callback);
// 开始任务
bool APP_Storage_BeginTask(uint64_t task_id, const app_storage_task_meta_t *meta);
bool APP_Storage_BeginTaskEx(uint64_t task_id, const app_storage_task_meta_t *meta, app_storage_phase_t phase);
// 开始任务（扩展：额外持久化采集状态与绝对时间基准 host_epoch_s）
bool APP_Storage_BeginTaskEx2(uint64_t task_id, const app_storage_task_meta_t *meta, app_storage_phase_t phase,
                              app_store_state_t state, uint32_t host_epoch_s);

// 更新任务的采集状态（仅重写 header，不动数据）。用于 STOP / 自然采完落盘 COMPLETED
bool APP_Storage_SetTaskState(uint64_t task_id, app_store_state_t state);

// 持久化"中断本次采集的复位原因/复位计数"到 header（reserved[3]/[4]）。
// 续采时调用，记录"上次采集是被什么打断的"，可跨多次复位从 flash 查询。
bool APP_Storage_SetTaskInterruptReason(uint64_t task_id, uint32_t reset_status, uint32_t reset_count);
// 读取某任务持久化的中断复位原因/计数（reserved[3]/[4]）。未记录过时输出 0xFFFFFFFF。
bool APP_Storage_GetTaskInterruptReason(uint64_t task_id, uint32_t *out_status, uint32_t *out_count);

// 查找待续采任务：返回 state==RUNNING 且 seq 最大的任务；
// 输出其 task_id / meta / 状态 / 绝对时间基准 host_epoch_s。未找到返回 false
bool APP_Storage_FindResumable(uint64_t *out_task_id, app_storage_task_meta_t *out_meta,
                               app_store_state_t *out_state, uint32_t *out_host_epoch_s);


// 追加写入数据
bool APP_Storage_AppendData(uint64_t task_id, const void *record, uint32_t record_size);
bool APP_Storage_AppendPreData(uint64_t task_id, const void *record, uint32_t record_size);
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
