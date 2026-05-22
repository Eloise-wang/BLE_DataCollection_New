/*
 * bsp_flash.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#include "bsp_fs.h"

#include <string.h>

#include "board.h"
#include "pin_mux.h"
#include "fsl_clock.h"
#include "fsl_os_abstraction.h"
#include "fsl_lpspi_nor_flash.h"
#include "lfs.h"

typedef struct
{
    nor_handle_t *nor;
    uint32_t base_offset;
} bsp_fs_ctx_t;

static bool s_initialized;
static bool s_mounted;
static OSA_MUTEX_HANDLE_DEFINE(s_fsMutexHandle);
static bool s_mutexCreated;

static lfs_t s_lfs;
static struct lfs_config s_lfsCfg;
static bsp_fs_ctx_t s_ctx;

static uint8_t s_readCache[256U];
static uint8_t s_progCache[256U];
static uint8_t s_lookahead[64U];

static nor_config_t s_norCfg;
static nor_handle_t s_norHandle;
static lpspi_memory_config_t s_memCfg = {
    .bytesInPageSize   = 256U,
    .bytesInSectorSize = 4096U,
    .bytesInMemorySize = (8U * 1024U * 1024U),
};

static int bsp_fs_lock(uint32_t timeout_ms)
{
    if (!s_mutexCreated)
    {
        return 0;
    }
    return (OSA_MutexLock((osa_mutex_handle_t)s_fsMutexHandle, timeout_ms) == KOSA_StatusSuccess) ? 0 : -1;
}

static void bsp_fs_unlock(void)
{
    if (s_mutexCreated)
    {
        (void)OSA_MutexUnlock((osa_mutex_handle_t)s_fsMutexHandle);
    }
}

static int bsp_lfs_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    const bsp_fs_ctx_t *ctx = (const bsp_fs_ctx_t *)c->context;
    const uint32_t addr = ctx->base_offset + ((uint32_t)block * c->block_size) + (uint32_t)off;
    if (Nor_Flash_Read(ctx->nor, addr, (uint8_t *)buffer, (uint32_t)size) != kStatus_Success)
    {
        return LFS_ERR_IO;
    }
    return 0;
}

static int bsp_lfs_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    const bsp_fs_ctx_t *ctx = (const bsp_fs_ctx_t *)c->context;
    const uint32_t addr = ctx->base_offset + ((uint32_t)block * c->block_size) + (uint32_t)off;
    if (Nor_Flash_Program(ctx->nor, addr, (uint8_t *)(uintptr_t)buffer, (uint32_t)size) != kStatus_Success)
    {
        return LFS_ERR_IO;
    }
    return 0;
}

static int bsp_lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    const bsp_fs_ctx_t *ctx = (const bsp_fs_ctx_t *)c->context;
    const uint32_t addr = ctx->base_offset + ((uint32_t)block * c->block_size);
    if (Nor_Flash_Erase_Sector(ctx->nor, addr) != kStatus_Success)
    {
        return LFS_ERR_IO;
    }
    return 0;
}

static int bsp_lfs_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

bool BSP_FS_Init(void)
{
    if (s_initialized)
    {
        return s_mounted;
    }

    BOARD_InitExtFlashPins();

    CLOCK_SetIpSrc(kCLOCK_Lpspi1, kCLOCK_IpSrcFro192M);
    CLOCK_EnableClockLPMode(kCLOCK_Lpspi1, kCLOCK_IpClkControl_fun1);

    if (!s_mutexCreated)
    {
        if (KOSA_StatusSuccess == OSA_MutexCreate((osa_mutex_handle_t)s_fsMutexHandle))
        {
            s_mutexCreated = true;
        }
    }

    s_norCfg.memControlConfig = &s_memCfg;
    s_norCfg.driverBaseAddr   = NULL;
    if (Nor_Flash_Init(&s_norCfg, &s_norHandle) != kStatus_Success)
    {
        s_initialized = true;
        s_mounted = false;
        return false;
    }
    s_norHandle.bytesInMemorySize = s_memCfg.bytesInMemorySize;

    (void)memset(&s_lfs, 0, sizeof(s_lfs));
    (void)memset(&s_lfsCfg, 0, sizeof(s_lfsCfg));

    s_ctx.nor         = &s_norHandle;
    s_ctx.base_offset = 0U;

    s_lfsCfg.context        = &s_ctx;
    s_lfsCfg.read           = bsp_lfs_read;
    s_lfsCfg.prog           = bsp_lfs_prog;
    s_lfsCfg.erase          = bsp_lfs_erase;
    s_lfsCfg.sync           = bsp_lfs_sync;
    s_lfsCfg.read_size      = 16U;
    s_lfsCfg.prog_size      = s_memCfg.bytesInPageSize;
    s_lfsCfg.block_size     = s_memCfg.bytesInSectorSize;
    s_lfsCfg.block_count    = s_memCfg.bytesInMemorySize / s_memCfg.bytesInSectorSize;
    s_lfsCfg.cache_size     = s_memCfg.bytesInPageSize;
    s_lfsCfg.lookahead_size = (uint32_t)sizeof(s_lookahead);
    s_lfsCfg.block_cycles   = 500U;
    s_lfsCfg.read_buffer    = s_readCache;
    s_lfsCfg.prog_buffer    = s_progCache;
    s_lfsCfg.lookahead_buffer = s_lookahead;

    if (bsp_fs_lock(5000U) != 0)
    {
        s_initialized = true;
        s_mounted = false;
        return false;
    }

    int err = lfs_mount(&s_lfs, &s_lfsCfg);
    if (err != 0)
    {
        (void)lfs_format(&s_lfs, &s_lfsCfg);
        err = lfs_mount(&s_lfs, &s_lfsCfg);
    }

    bsp_fs_unlock();

    s_initialized = true;
    s_mounted     = (err == 0);
    return s_mounted;
}

void BSP_FS_Deinit(void)
{
    if (!s_initialized)
    {
        return;
    }

    if (bsp_fs_lock(5000U) == 0)
    {
        if (s_mounted)
        {
            (void)lfs_unmount(&s_lfs);
        }
        bsp_fs_unlock();
    }

    (void)Nor_Flash_DeInit(&s_norHandle);
    s_mounted     = false;
    s_initialized = false;
}

bool BSP_FS_IsMounted(void)
{
    return s_mounted;
}

int BSP_FS_Mkdir(const char *path)
{
    if ((path == NULL) || (!s_mounted))
    {
        return -1;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        return -1;
    }

    const int err = lfs_mkdir(&s_lfs, path);
    bsp_fs_unlock();

    if ((err == 0) || (err == LFS_ERR_EXIST))
    {
        return 0;
    }
    return err;
}

int BSP_FS_Remove(const char *path)
{
    if ((path == NULL) || (!s_mounted))
    {
        return -1;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        return -1;
    }
    const int err = lfs_remove(&s_lfs, path);
    bsp_fs_unlock();

    if ((err == 0) || (err == LFS_ERR_NOENT))
    {
        return 0;
    }
    return err;
}

int BSP_FS_FileAppend(const char *path, const void *data, uint32_t size)
{
    if ((path == NULL) || (data == NULL) || (size == 0U) || (!s_mounted))
    {
        return -1;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        return -1;
    }

    lfs_file_t f;
    int err = lfs_file_open(&s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
    if (err == 0)
    {
        const lfs_ssize_t w = lfs_file_write(&s_lfs, &f, data, (lfs_size_t)size);
        if (w != (lfs_ssize_t)size)
        {
            err = (int)w;
        }
        (void)lfs_file_close(&s_lfs, &f);
    }

    bsp_fs_unlock();
    return err;
}

int BSP_FS_FileWriteTruncate(const char *path, const void *data, uint32_t size)
{
    if ((path == NULL) || (data == NULL) || (size == 0U) || (!s_mounted))
    {
        return -1;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        return -1;
    }

    lfs_file_t f;
    int err = lfs_file_open(&s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err == 0)
    {
        const lfs_ssize_t w = lfs_file_write(&s_lfs, &f, data, (lfs_size_t)size);
        if (w != (lfs_ssize_t)size)
        {
            err = (int)w;
        }
        (void)lfs_file_close(&s_lfs, &f);
    }

    bsp_fs_unlock();
    return err;
}

int BSP_FS_FileReadAt(const char *path, uint32_t offset, void *out, uint32_t size)
{
    if ((path == NULL) || (out == NULL) || (size == 0U) || (!s_mounted))
    {
        return -1;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        return -1;
    }

    lfs_file_t f;
    int err = lfs_file_open(&s_lfs, &f, path, LFS_O_RDONLY);
    if (err == 0)
    {
        const lfs_soff_t s = lfs_file_seek(&s_lfs, &f, (lfs_soff_t)offset, LFS_SEEK_SET);
        if (s < 0)
        {
            err = (int)s;
        }
        else
        {
            const lfs_ssize_t r = lfs_file_read(&s_lfs, &f, out, (lfs_size_t)size);
            err = (int)r;
        }
        (void)lfs_file_close(&s_lfs, &f);
    }

    bsp_fs_unlock();
    return err;
}

int BSP_FS_FileSize(const char *path, uint32_t *out_size)
{
    if ((path == NULL) || (out_size == NULL) || (!s_mounted))
    {
        return -1;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        return -1;
    }

    lfs_file_t f;
    int err = lfs_file_open(&s_lfs, &f, path, LFS_O_RDONLY);
    if (err == 0)
    {
        const lfs_soff_t s = lfs_file_size(&s_lfs, &f);
        if (s < 0)
        {
            err = (int)s;
        }
        else
        {
            *out_size = (uint32_t)s;
            err = 0;
        }
        (void)lfs_file_close(&s_lfs, &f);
    }

    bsp_fs_unlock();
    return err;
}
