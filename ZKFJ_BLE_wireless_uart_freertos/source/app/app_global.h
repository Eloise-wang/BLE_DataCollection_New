/*
 * app_global.h
 *  function : 全局控制命令定义、整机数据结构体对齐
 *  Created on: 2026年5月22日
 *      Author: elois
 */


#ifndef APP_APP_GLOBAL_H_
#define APP_APP_GLOBAL_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define APP_PACKED __attribute__((packed))
#else
#define APP_PACKED
#endif

// 传感器数据结构体（精确20字节，满足2字节对齐）
typedef struct APP_PACKED
{
    uint32_t timestamp_s;
    uint16_t timestamp_ms;
    int16_t temperature_centi_c;
    uint16_t pressure_kpa;
    uint16_t liquid_level;
    uint32_t remaining_count;
    uint8_t liquid_access_state;
    uint8_t status;
    uint8_t reserved[2];  // 精确20字节，2字节对齐，跨页写入概率大幅降低（跨256字节边界的周期从18变为20）
} sensor_record_t;

typedef uint8_t app_cmd_t;

// 系统命令定义
#define CMD_SYS_STOP_COLLECT     ((app_cmd_t)0x01U)
#define CMD_SYS_START_COLLECT    ((app_cmd_t)0x02U)
#define CMD_SYS_READ_HISTORY     ((app_cmd_t)0x03U)
#define CMD_SYS_CLEAR_FLASH      ((app_cmd_t)0x04U)

#define APP_RECORD_STATUS_OK             ((uint8_t)0x00U)
#define APP_RECORD_STATUS_SENSOR_ERROR   ((uint8_t)0x01U)
#define APP_RECORD_STATUS_CAN_ERROR      ((uint8_t)0x02U)
#define APP_RECORD_STATUS_STORAGE_ERROR  ((uint8_t)0x04U)
#define APP_RECORD_STATUS_LIQUID_OFFLINE ((uint8_t)0x08U)

#ifdef __cplusplus
}
#endif

#endif /* APP_APP_GLOBAL_H_ */
