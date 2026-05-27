/*
 * bsp_flash.h
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#ifndef BSP_BSP_FS_H_
#define BSP_BSP_FS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化文件系统
bool BSP_FS_Init(void);
// 去初始化文件系统
void BSP_FS_Deinit(void);
//检查文件系统是否挂载
bool BSP_FS_IsMounted(void);

// 创建目录
int BSP_FS_Mkdir(const char *path);
// 删除目录
int BSP_FS_Remove(const char *path);

// 格式化文件系统（清空所有任务数据）
bool BSP_FS_Format(void);

typedef struct
{
    uint32_t fileappend_open_noent;
    uint32_t fileappend_open_noent_recovered;
    uint32_t fileappend_open_io;
    uint32_t prog_verify_mismatch;
    uint32_t prog_verify_recovered;
    uint32_t erase_verify_failed;
    uint32_t erase_verify_recovered;
    uint32_t io_error;
} bsp_fs_diag_t;

void BSP_FS_GetDiag(bsp_fs_diag_t *out, bool clear);

//追加写入文件
int BSP_FS_FileAppend(const char *path, const void *data, uint32_t size);
// 写入文件并截断
int BSP_FS_FileWriteTruncate(const char *path, const void *data, uint32_t size);
// 从文件偏移量读取数据
int BSP_FS_FileReadAt(const char *path, uint32_t offset, void *out, uint32_t size);
// 获取文件大小
int BSP_FS_FileSize(const char *path, uint32_t *out_size);

// 擦除崩溃日志文件
bool BSP_FS_CrashLog_Erase(void);
// 写入崩溃日志文件
bool BSP_FS_CrashLog_Write(const void *data, uint32_t size);
// 从崩溃日志文件偏移量读取数据
int BSP_FS_CrashLog_Read(uint32_t offset, void *out, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BSP_FS_H_ */
