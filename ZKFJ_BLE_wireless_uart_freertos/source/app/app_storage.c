/*
 * app_storage.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#include "app_storage.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp_fs.h"

#define APP_STORAGE_DIR_MAX   32
#define APP_STORAGE_PATH_MAX  64
#define APP_STORAGE_LOG_BUF_MAX 256

static void app_storage_task_dir(char out[APP_STORAGE_DIR_MAX], uint64_t task_id)
{
    (void)snprintf(out, APP_STORAGE_DIR_MAX, "task_%016llX", (unsigned long long)task_id);
}

static void app_storage_task_path(char out[APP_STORAGE_PATH_MAX], uint64_t task_id, const char *file)
{
    char dir[APP_STORAGE_DIR_MAX];
    app_storage_task_dir(dir, task_id);
    (void)snprintf(out, APP_STORAGE_PATH_MAX, "%s/%s", dir, file);
}

bool APP_Storage_Init(void)
{
    return BSP_FS_Init();
}

bool APP_Storage_BeginTask(uint64_t task_id, const app_storage_task_meta_t *meta)
{
    char dir[APP_STORAGE_DIR_MAX];
    char metaPath[APP_STORAGE_PATH_MAX];

    if (!BSP_FS_IsMounted())
    {
        return false;
    }

    app_storage_task_dir(dir, task_id);
    if (BSP_FS_Mkdir(dir) != 0)
    {
        return false;
    }

    if (meta != NULL)
    {
        app_storage_task_path(metaPath, task_id, "meta.bin");
        if (BSP_FS_FileWriteTruncate(metaPath, meta, (uint32_t)sizeof(*meta)) != 0)
        {
            return false;
        }
    }

    return true;
}

bool APP_Storage_AppendData(uint64_t task_id, const void *record, uint32_t record_size)
{
    char dataPath[APP_STORAGE_PATH_MAX];

    if ((record == NULL) || (record_size == 0U) || (!BSP_FS_IsMounted()))
    {
        return false;
    }

    app_storage_task_path(dataPath, task_id, "data.bin");
    return (BSP_FS_FileAppend(dataPath, record, record_size) == 0);
}

bool APP_Storage_AppendLog(uint64_t task_id, const void *data, uint32_t size)
{
    char logPath[APP_STORAGE_PATH_MAX];

    if ((data == NULL) || (size == 0U) || (!BSP_FS_IsMounted()))
    {
        return false;
    }

    app_storage_task_path(logPath, task_id, "sys.log");
    return (BSP_FS_FileAppend(logPath, data, size) == 0);
}

bool APP_Storage_LogPrintf(uint64_t task_id, const char *format, ...)
{
    char buf[APP_STORAGE_LOG_BUF_MAX];
    va_list args;

    if ((format == NULL) || (!BSP_FS_IsMounted()))
    {
        return false;
    }

    va_start(args, format);
    const int n = vsnprintf(buf, (size_t)sizeof(buf), format, args);
    va_end(args);

    if (n <= 0)
    {
        return false;
    }

    uint32_t size = (uint32_t)n;
    if (size >= (uint32_t)sizeof(buf))
    {
        size = (uint32_t)sizeof(buf) - 1U;
    }

    if ((size > 0U) && (buf[size - 1U] != '\n') && (size < ((uint32_t)sizeof(buf) - 1U)))
    {
        buf[size] = '\n';
        size++;
    }

    return APP_Storage_AppendLog(task_id, buf, size);
}

int APP_Storage_ReadLog(uint64_t task_id, uint32_t offset, void *out, uint32_t size)
{
    char logPath[APP_STORAGE_PATH_MAX];

    if ((out == NULL) || (size == 0U) || (!BSP_FS_IsMounted()))
    {
        return -1;
    }

    app_storage_task_path(logPath, task_id, "sys.log");
    return BSP_FS_FileReadAt(logPath, offset, out, size);
}

bool APP_Storage_GetLogSize(uint64_t task_id, uint32_t *out_size)
{
    char logPath[APP_STORAGE_PATH_MAX];

    if ((out_size == NULL) || (!BSP_FS_IsMounted()))
    {
        return false;
    }

    app_storage_task_path(logPath, task_id, "sys.log");
    return (BSP_FS_FileSize(logPath, out_size) == 0);
}

int APP_Storage_ReadData(uint64_t task_id, uint32_t offset, void *out, uint32_t size)
{
    char dataPath[APP_STORAGE_PATH_MAX];

    if ((out == NULL) || (size == 0U) || (!BSP_FS_IsMounted()))
    {
        return -1;
    }

    app_storage_task_path(dataPath, task_id, "data.bin");
    return BSP_FS_FileReadAt(dataPath, offset, out, size);
}

bool APP_Storage_GetDataSize(uint64_t task_id, uint32_t *out_size)
{
    char dataPath[APP_STORAGE_PATH_MAX];

    if ((out_size == NULL) || (!BSP_FS_IsMounted()))
    {
        return false;
    }

    app_storage_task_path(dataPath, task_id, "data.bin");
    return (BSP_FS_FileSize(dataPath, out_size) == 0);
}

bool APP_Storage_DeleteTask(uint64_t task_id)
{
    char dir[APP_STORAGE_DIR_MAX];
    char p[APP_STORAGE_PATH_MAX];

    if (!BSP_FS_IsMounted())
    {
        return false;
    }

    app_storage_task_path(p, task_id, "data.bin");
    (void)BSP_FS_Remove(p);
    app_storage_task_path(p, task_id, "sys.log");
    (void)BSP_FS_Remove(p);
    app_storage_task_path(p, task_id, "meta.bin");
    (void)BSP_FS_Remove(p);

    app_storage_task_dir(dir, task_id);
    return (BSP_FS_Remove(dir) == 0);
}
