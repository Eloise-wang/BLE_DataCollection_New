/*
 * bsp_flash.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 */

#include "bsp_fs.h"

#include "bsp_uart.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "pin_mux.h"
#include "fsl_clock.h"
#include "fsl_os_abstraction.h"
#include "fsl_lpspi_nor_flash.h"
#include "fsl_lpspi_mem_adapter.h"
#include "lfs.h"

extern uint32_t BOARD_GetLpspiClock(void);
extern uint32_t BOARD_GetNorFlashBaudrate(void);

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

static bsp_fs_diag_t s_diag;

static uint8_t s_readCache[256U];
static uint8_t s_progCache[256U];
static uint8_t s_lookahead[64U];
static uint8_t s_eraseVerifyBuf[32U];
static uint8_t s_progVerifyBuf[256U];

#ifndef BSP_FS_IO_VERIFY_ENABLE
#define BSP_FS_IO_VERIFY_ENABLE 1
#endif

static nor_config_t s_norCfg;
static nor_handle_t s_norHandle;
static lpspi_memory_config_t s_memCfg = {
    .bytesInPageSize   = 256U,
    .bytesInSectorSize = 4096U,
    .bytesInMemorySize = (8U * 1024U * 1024U),
};

#ifndef BSP_FS_LOG_ENABLE
#define BSP_FS_LOG_ENABLE 0
#endif

#ifndef BSP_FS_BLOCK_COUNT_OVERRIDE
#define BSP_FS_BLOCK_COUNT_OVERRIDE 0U
#endif

#ifndef BSP_FS_AUTO_FORMAT_ON_CORRUPT
#define BSP_FS_AUTO_FORMAT_ON_CORRUPT 0
#endif

#ifndef BSP_FS_AUTO_FORMAT_ON_MOUNT_FAIL
#define BSP_FS_AUTO_FORMAT_ON_MOUNT_FAIL 0
#endif

#ifndef BSP_FS_VERIFY_ALL_ERASE_ON_FORMAT
#define BSP_FS_VERIFY_ALL_ERASE_ON_FORMAT 0
#endif

#if BSP_FS_LOG_ENABLE
static void bsp_fs_log(const char *fmt, ...)
{
    if (fmt == NULL)
    {
        return;
    }

    char msg[192];

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (n <= 0)
    {
        return;
    }

    if ((uint32_t)n >= (uint32_t)sizeof(msg))
    {
        msg[sizeof(msg) - 1U] = '\0';
    }

    BSP_UART_Print("[FS] %s\r\n", msg);
}
#else
#define bsp_fs_log(...) do { } while (0)
#endif

static const char *bsp_fs_lfs_err_str(int err)
{
    switch (err)
    {
        case LFS_ERR_OK: return "OK";
        case LFS_ERR_IO: return "IO";
        case LFS_ERR_CORRUPT: return "CORRUPT";
        case LFS_ERR_NOENT: return "NOENT";
        case LFS_ERR_EXIST: return "EXIST";
        case LFS_ERR_NOTDIR: return "NOTDIR";
        case LFS_ERR_ISDIR: return "ISDIR";
        case LFS_ERR_NOTEMPTY: return "NOTEMPTY";
        case LFS_ERR_BADF: return "BADF";
        case LFS_ERR_FBIG: return "FBIG";
        case LFS_ERR_INVAL: return "INVAL";
        case LFS_ERR_NOSPC: return "NOSPC";
        case LFS_ERR_NOMEM: return "NOMEM";
        case LFS_ERR_NOATTR: return "NOATTR";
        case LFS_ERR_NAMETOOLONG: return "NAMETOOLONG";
        default: return "?";
    }
}

static bool bsp_fs_spi_cmd_only(LPSPI_Type *base, const uint8_t *cmd, size_t cmdSize)
{
    if ((base == NULL) || (cmd == NULL) || (cmdSize == 0U))
    {
        return false;
    }

    spi_mem_xfer_t xfer;
    xfer.cmd = (uint8_t *)(uintptr_t)cmd;
    xfer.data = NULL;
    xfer.cmdSize = cmdSize;
    xfer.dataSize = 0U;
    xfer.mode = kSpiMem_Xfer_CommandOnly;

    return (LPSPI_MemXfer(&xfer, base) == kStatus_Success);
}

static status_t bsp_fs_nor_read_raw(nor_handle_t *nor, uint32_t address, uint8_t *buffer, uint32_t length, bool fastRead)
{
    if ((nor == NULL) || (nor->driverBaseAddr == NULL) || (buffer == NULL) || (length == 0U))
    {
        return kStatus_InvalidArgument;
    }

    return LPSPI_MemRead(address, buffer, length, fastRead, (LPSPI_Type *)nor->driverBaseAddr);
}

static void bsp_fs_flash_recover(LPSPI_Type *base)
{
    if (base == NULL)
    {
        return;
    }

    const uint8_t cmd_release_pd = 0xABU;
    (void)bsp_fs_spi_cmd_only(base, &cmd_release_pd, 1U);

    const uint8_t cmd_rst_en = 0x66U;
    const uint8_t cmd_rst = 0x99U;
    (void)bsp_fs_spi_cmd_only(base, &cmd_rst_en, 1U);
    (void)bsp_fs_spi_cmd_only(base, &cmd_rst, 1U);
}

static void bsp_fs_log_lfs_stat_locked(const char *tag, const char *path)
{
    if ((tag == NULL) || (path == NULL))
    {
        return;
    }

    struct lfs_info info;
    const int err = lfs_stat(&s_lfs, path, &info);
    if (err == 0)
    {
        bsp_fs_log("%s stat ok: %s type=%u size=%u", tag, path, (unsigned)info.type, (unsigned)info.size);
        return;
    }
    bsp_fs_log("%s stat fail: %s err=%d,%s", tag, path, err, bsp_fs_lfs_err_str(err));
}

static void bsp_fs_log_path_ctx_locked(const char *tag, const char *path)
{
    if ((tag == NULL) || (path == NULL))
    {
        return;
    }

    bsp_fs_log("%s path: %s", tag, path);
    bsp_fs_log_lfs_stat_locked(tag, path);

    const char *slash = strrchr(path, '/');
    if ((slash != NULL) && (slash != path))
    {
        char parent[128];
        const size_t n = (size_t)(slash - path);
        const size_t copyN = (n < (sizeof(parent) - 1U)) ? n : (sizeof(parent) - 1U);
        memcpy(parent, path, copyN);
        parent[copyN] = '\0';
        bsp_fs_log_lfs_stat_locked(tag, parent);
    }
    else
    {
        bsp_fs_log("%s parent skip", tag);
    }
}

static int bsp_fs_wait_nor_ready(nor_handle_t *nor, uint32_t timeout_ms)
{
    if (nor == NULL)
    {
        return -1;
    }

    for (uint32_t waited = 0U; waited < timeout_ms; waited++)
    {
        bool busy = false;
        const status_t st = Nor_Flash_Is_Busy(nor, &busy);
        if (st != kStatus_Success)
        {
            return -1;
        }
        if (!busy)
        {
            return 0;
        }
        OSA_TimeDelay(1U);
    }
    return -1;
}

static status_t bsp_fs_prepare_flash_io_locked(void)
{
    BOARD_InitExtFlashPins();

    CLOCK_EnableClock(kCLOCK_Lpspi1);
    CLOCK_SetIpSrc(kCLOCK_Lpspi1, kCLOCK_IpSrcFro192M);
    CLOCK_SetIpSrcDiv(kCLOCK_Lpspi1, kSCG_SysClkDivBy1);
    CLOCK_EnableClockLPMode(kCLOCK_Lpspi1, kCLOCK_IpClkControl_fun1);

    bsp_fs_flash_recover(LPSPI1);

    s_norCfg.memControlConfig = &s_memCfg;
    s_norCfg.driverBaseAddr   = NULL;
    const status_t st = Nor_Flash_Init(&s_norCfg, &s_norHandle);
    bsp_fs_log("Nor_Flash_Init(re) status=%d", (int)st);
    if (st == kStatus_Success)
    {
        if (s_norHandle.bytesInPageSize != 0U)
        {
            s_memCfg.bytesInPageSize = s_norHandle.bytesInPageSize;
        }
        if (s_norHandle.bytesInSectorSize != 0U)
        {
            s_memCfg.bytesInSectorSize = s_norHandle.bytesInSectorSize;
        }
        if (s_norHandle.bytesInMemorySize != 0U)
        {
            s_memCfg.bytesInMemorySize = s_norHandle.bytesInMemorySize;
        }

        if (s_lfsCfg.block_size != 0U)
        {
            s_ctx.nor = &s_norHandle;
            s_lfsCfg.prog_size  = s_memCfg.bytesInPageSize;
            s_lfsCfg.block_size = s_memCfg.bytesInSectorSize;
            s_lfsCfg.cache_size = s_memCfg.bytesInPageSize;
        }
    }

    return st;
}

static uint32_t bsp_fs_get_fs_size(void)
{
    if (s_memCfg.bytesInSectorSize == 0U)
    {
        return 0U;
    }
    if (s_memCfg.bytesInMemorySize < s_memCfg.bytesInSectorSize)
    {
        return 0U;
    }
    return s_memCfg.bytesInMemorySize - s_memCfg.bytesInSectorSize;
}

static uint32_t bsp_fs_get_effective_fs_size_bytes(void)
{
    const uint32_t sector = s_memCfg.bytesInSectorSize;
    if (sector == 0U)
    {
        return 0U;
    }

    uint32_t blocks = bsp_fs_get_fs_size() / sector;
    if (BSP_FS_BLOCK_COUNT_OVERRIDE != 0U)
    {
        blocks = BSP_FS_BLOCK_COUNT_OVERRIDE;
    }

    return blocks * sector;
}

static uint32_t bsp_fs_get_crashlog_base(void)
{
    return bsp_fs_get_fs_size();
}

static int bsp_fs_lock(uint32_t timeout_ms)
{
    if (!s_mutexCreated)
    {
        if (KOSA_StatusSuccess == OSA_MutexCreate((osa_mutex_handle_t)s_fsMutexHandle))
        {
            s_mutexCreated = true;
        }
        else
        {
            return -1;
        }
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

static int bsp_fs_verify_erased_sector(uint32_t addr, uint32_t sectorSize);

static int bsp_lfs_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    const bsp_fs_ctx_t *ctx = (const bsp_fs_ctx_t *)c->context;
    const uint32_t addr = ctx->base_offset + ((uint32_t)block * c->block_size) + (uint32_t)off;
    const status_t st = bsp_fs_nor_read_raw(ctx->nor, addr, (uint8_t *)buffer, (uint32_t)size, false);
    if (st != kStatus_Success)
    {
        bsp_fs_log("IO read failed: st=%d addr=0x%08X block=%u off=%u size=%u",
                   (int)st, (unsigned)addr, (unsigned)block, (unsigned)off, (unsigned)size);
        return LFS_ERR_IO;
    }
    return 0;
}

static int bsp_lfs_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    const bsp_fs_ctx_t *ctx = (const bsp_fs_ctx_t *)c->context;
    const uint32_t addr = ctx->base_offset + ((uint32_t)block * c->block_size) + (uint32_t)off;
    const status_t st = Nor_Flash_Program(ctx->nor, addr, (uint8_t *)(uintptr_t)buffer, (uint32_t)size);
    if (st != kStatus_Success)
    {
        s_diag.io_error++;
        bsp_fs_log("IO prog failed: st=%d addr=0x%08X block=%u off=%u size=%u",
                   (int)st, (unsigned)addr, (unsigned)block, (unsigned)off, (unsigned)size);
        return LFS_ERR_IO;
    }

    if (bsp_fs_wait_nor_ready(ctx->nor, 2000U) != 0)
    {
        s_diag.io_error++;
        bsp_fs_log("IO prog wait ready timeout: addr=0x%08X size=%u", (unsigned)addr, (unsigned)size);
        return LFS_ERR_IO;
    }

#if BSP_FS_IO_VERIFY_ENABLE
    if ((size <= (lfs_size_t)sizeof(s_progVerifyBuf)) && (buffer != NULL))
    {
        const uint8_t *expected = (const uint8_t *)buffer;
        if (bsp_fs_nor_read_raw(ctx->nor, addr, s_progVerifyBuf, (uint32_t)size, false) != kStatus_Success)
        {
            s_diag.io_error++;
            bsp_fs_log("IO prog verify read failed: addr=0x%08X size=%u", (unsigned)addr, (unsigned)size);
            return LFS_ERR_IO;
        }
        if (memcmp(s_progVerifyBuf, expected, (size_t)size) != 0)
        {
            s_diag.prog_verify_mismatch++;
            bsp_fs_log("IO prog verify mismatch: addr=0x%08X block=%u off=%u size=%u",
                       (unsigned)addr, (unsigned)block, (unsigned)off, (unsigned)size);

            if (bsp_fs_nor_read_raw(ctx->nor, addr, s_progVerifyBuf, (uint32_t)size, false) == kStatus_Success)
            {
                if (memcmp(s_progVerifyBuf, expected, (size_t)size) == 0)
                {
                    s_diag.prog_verify_recovered++;
                    return 0;
                }
            }

            const status_t rst = Nor_Flash_Program(ctx->nor, addr, (uint8_t *)(uintptr_t)expected, (uint32_t)size);
            if (rst == kStatus_Success)
            {
                if (bsp_fs_wait_nor_ready(ctx->nor, 2000U) == 0)
                {
                    if (bsp_fs_nor_read_raw(ctx->nor, addr, s_progVerifyBuf, (uint32_t)size, false) == kStatus_Success)
                    {
                        if (memcmp(s_progVerifyBuf, expected, (size_t)size) == 0)
                        {
                            s_diag.prog_verify_recovered++;
                            return 0;
                        }
                    }
                }
                bsp_fs_log("IO prog verify retry failed: addr=0x%08X size=%u", (unsigned)addr, (unsigned)size);
            }
            s_diag.io_error++;
            return LFS_ERR_IO;
        }
    }
#endif
    return 0;
}

static int bsp_lfs_erase(const struct lfs_config *c, lfs_block_t block)
{
    const bsp_fs_ctx_t *ctx = (const bsp_fs_ctx_t *)c->context;
    const uint32_t addr = ctx->base_offset + ((uint32_t)block * c->block_size);
    const status_t st = Nor_Flash_Erase_Sector(ctx->nor, addr);
    if (st != kStatus_Success)
    {
        s_diag.io_error++;
        bsp_fs_log("IO erase failed: st=%d addr=0x%08X block=%u", (int)st, (unsigned)addr, (unsigned)block);
        return LFS_ERR_IO;
    }

    if (bsp_fs_wait_nor_ready(ctx->nor, 5000U) != 0)
    {
        s_diag.io_error++;
        bsp_fs_log("IO erase wait ready timeout: addr=0x%08X block=%u", (unsigned)addr, (unsigned)block);
        return LFS_ERR_IO;
    }
#if BSP_FS_IO_VERIFY_ENABLE
    if (bsp_fs_verify_erased_sector(addr, c->block_size) != 0)
    {
        s_diag.erase_verify_failed++;
        bsp_fs_log("IO erase verify failed: addr=0x%08X block=%u", (unsigned)addr, (unsigned)block);
        const status_t rst = Nor_Flash_Erase_Sector(ctx->nor, addr);
        if (rst == kStatus_Success)
        {
            if (bsp_fs_wait_nor_ready(ctx->nor, 5000U) == 0)
            {
                if (bsp_fs_verify_erased_sector(addr, c->block_size) == 0)
                {
                    s_diag.erase_verify_recovered++;
                    return 0;
                }
            }
            bsp_fs_log("IO erase verify retry failed: addr=0x%08X block=%u", (unsigned)addr, (unsigned)block);
        }
        s_diag.io_error++;
        return LFS_ERR_IO;
    }
#endif
    return 0;
}

static int bsp_lfs_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

static int bsp_fs_verify_erased_sector(uint32_t addr, uint32_t sectorSize)
{
    if (sectorSize < (uint32_t)sizeof(s_eraseVerifyBuf))
    {
        return -1;
    }

    if (bsp_fs_nor_read_raw(&s_norHandle, addr, s_eraseVerifyBuf, (uint32_t)sizeof(s_eraseVerifyBuf), false) != kStatus_Success)
    {
        bsp_fs_log("verify read failed: addr=0x%08X", (unsigned)addr);
        return -1;
    }
    for (uint32_t i = 0U; i < (uint32_t)sizeof(s_eraseVerifyBuf); i++)
    {
        if (s_eraseVerifyBuf[i] != 0xFFU)
        {
            bsp_fs_log("verify not erased: addr=0x%08X +%u =0x%02X",
                       (unsigned)addr, (unsigned)i, (unsigned)s_eraseVerifyBuf[i]);
            return -1;
        }
    }

    const uint32_t tailAddr = addr + sectorSize - (uint32_t)sizeof(s_eraseVerifyBuf);
    if (bsp_fs_nor_read_raw(&s_norHandle, tailAddr, s_eraseVerifyBuf, (uint32_t)sizeof(s_eraseVerifyBuf), false) != kStatus_Success)
    {
        bsp_fs_log("verify read failed: addr=0x%08X", (unsigned)tailAddr);
        return -1;
    }
    for (uint32_t i = 0U; i < (uint32_t)sizeof(s_eraseVerifyBuf); i++)
    {
        if (s_eraseVerifyBuf[i] != 0xFFU)
        {
            bsp_fs_log("verify not erased: addr=0x%08X +%u =0x%02X",
                       (unsigned)tailAddr, (unsigned)i, (unsigned)s_eraseVerifyBuf[i]);
            return -1;
        }
    }

    return 0;
}

static int bsp_fs_mkconsistent_locked(void)
{
#ifndef LFS_READONLY
    const int err = lfs_fs_mkconsistent(&s_lfs);
    if (err != 0)
    {
        bsp_fs_log("lfs_fs_mkconsistent failed: err=%d,%s", err, bsp_fs_lfs_err_str(err));
    }
    return err;
#else
    return 0;
#endif
}

static int bsp_fs_mount_with_repair_locked(void)
{
    (void)bsp_fs_prepare_flash_io_locked();

    int err = lfs_mount(&s_lfs, &s_lfsCfg);
    if (err == 0)
    {
        bsp_fs_log("lfs_mount ok");
        const int cerr = bsp_fs_mkconsistent_locked();
        if (cerr == 0)
        {
            return 0;
        }
        err = cerr;
    }
    else
    {
        bsp_fs_log("lfs_mount failed: err=%d,%s", err, bsp_fs_lfs_err_str(err));
    }

#if BSP_FS_AUTO_FORMAT_ON_MOUNT_FAIL
    bsp_fs_log("try format due to err=%d,%s (AUTO_FORMAT_ON_MOUNT_FAIL=1)", err, bsp_fs_lfs_err_str(err));
    {
        const uint32_t fsSize = bsp_fs_get_effective_fs_size_bytes();
        const uint32_t sector = s_memCfg.bytesInSectorSize;
        if ((fsSize != 0U) && (sector != 0U))
        {
            bsp_fs_log("physical erase before format: bytes=%u sector=%u", (unsigned)fsSize, (unsigned)sector);
            for (uint32_t addr = 0U; addr < fsSize; addr += sector)
            {
                const status_t est = Nor_Flash_Erase_Sector(&s_norHandle, addr);
                if (est != kStatus_Success)
                {
                    bsp_fs_log("physical erase failed: st=%d addr=0x%08X", (int)est, (unsigned)addr);
                    return LFS_ERR_IO;
                }
                const bool need_verify = (BSP_FS_VERIFY_ALL_ERASE_ON_FORMAT != 0) ||
                                         (addr < (2U * sector)) ||
                                         (((addr / sector) & 0xFFU) == 0xFFU) ||
                                         ((addr + sector) >= fsSize);
                if (need_verify)
                {
                    if (bsp_fs_verify_erased_sector(addr, sector) != 0)
                    {
                        bsp_fs_log("erase verify failed at sector addr=0x%08X", (unsigned)addr);
                        return LFS_ERR_IO;
                    }
                }
                if (((addr / sector) & 0x7FU) == 0x7FU)
                {
                    bsp_fs_log("physical erase progress: %u/%u", (unsigned)(addr / sector + 1U), (unsigned)(fsSize / sector));
                }
            }
        }
    }
    const int ferr = lfs_format(&s_lfs, &s_lfsCfg);
    bsp_fs_log("lfs_format: err=%d,%s", ferr, bsp_fs_lfs_err_str(ferr));
    if (ferr != 0)
    {
    }

    err = lfs_mount(&s_lfs, &s_lfsCfg);
    bsp_fs_log("lfs_mount after format: err=%d,%s", err, bsp_fs_lfs_err_str(err));
    if (err == 0)
    {
        const int cerr = bsp_fs_mkconsistent_locked();
        if (cerr != 0)
        {
            err = cerr;
        }
    }
    return err;
#else
    return err;
#endif
}

static int bsp_fs_repair_on_corrupt_locked(void)
{
    (void)bsp_fs_prepare_flash_io_locked();

    const int cerr = bsp_fs_mkconsistent_locked();
    bsp_fs_log("mkconsistent result: err=%d,%s", cerr, bsp_fs_lfs_err_str(cerr));

    const int uerr = lfs_unmount(&s_lfs);
    bsp_fs_log("lfs_unmount for remount: err=%d,%s", uerr, bsp_fs_lfs_err_str(uerr));

    int merr = lfs_mount(&s_lfs, &s_lfsCfg);
    bsp_fs_log("lfs_mount after remount: err=%d,%s", merr, bsp_fs_lfs_err_str(merr));
    if (merr == 0)
    {
        const int cerr2 = bsp_fs_mkconsistent_locked();
        bsp_fs_log("mkconsistent after remount: err=%d,%s", cerr2, bsp_fs_lfs_err_str(cerr2));
        return cerr2;
    }

#if BSP_FS_AUTO_FORMAT_ON_CORRUPT
    bsp_fs_log("remount failed, try format (AUTO_FORMAT_ON_CORRUPT=1)");
    const uint32_t fsSize = bsp_fs_get_effective_fs_size_bytes();
    const uint32_t sector = s_memCfg.bytesInSectorSize;
    if ((fsSize != 0U) && (sector != 0U))
    {
        bsp_fs_log("physical erase before format: bytes=%u sector=%u", (unsigned)fsSize, (unsigned)sector);
        for (uint32_t addr = 0U; addr < fsSize; addr += sector)
        {
            const status_t est = Nor_Flash_Erase_Sector(&s_norHandle, addr);
            if (est != kStatus_Success)
            {
                bsp_fs_log("physical erase failed: st=%d addr=0x%08X", (int)est, (unsigned)addr);
                return LFS_ERR_IO;
            }
            const bool need_verify = (BSP_FS_VERIFY_ALL_ERASE_ON_FORMAT != 0) ||
                                     (addr < (2U * sector)) ||
                                     (((addr / sector) & 0xFFU) == 0xFFU) ||
                                     ((addr + sector) >= fsSize);
            if (need_verify)
            {
                if (bsp_fs_verify_erased_sector(addr, sector) != 0)
                {
                    bsp_fs_log("erase verify failed at sector addr=0x%08X", (unsigned)addr);
                    return LFS_ERR_IO;
                }
            }
            if (((addr / sector) & 0x7FU) == 0x7FU)
            {
                bsp_fs_log("physical erase progress: %u/%u", (unsigned)(addr / sector + 1U), (unsigned)(fsSize / sector));
            }
        }
    }

    const int ferr = lfs_format(&s_lfs, &s_lfsCfg);
    bsp_fs_log("lfs_format: err=%d,%s", ferr, bsp_fs_lfs_err_str(ferr));
    if (ferr != 0)
    {
        return ferr;
    }

    merr = lfs_mount(&s_lfs, &s_lfsCfg);
    bsp_fs_log("lfs_mount after format: err=%d,%s", merr, bsp_fs_lfs_err_str(merr));
    if (merr != 0)
    {
        return merr;
    }

    return bsp_fs_mkconsistent_locked();
#else
    return merr;
#endif
}

bool BSP_FS_Init(void)
{
    if (s_initialized)
    {
        bsp_fs_log("Init already done: mounted=%u", (unsigned)s_mounted);
        if (s_mounted)
        {
            return true;
        }
        if ((s_norHandle.driverBaseAddr == NULL) || (s_lfsCfg.block_size == 0U) || (s_lfsCfg.block_count == 0U))
        {
            return false;
        }

        if (bsp_fs_lock(5000U) != 0)
        {
            return false;
        }
        const int err = bsp_fs_mount_with_repair_locked();
        bsp_fs_unlock();

        s_mounted = (err == 0);
        bsp_fs_log("Remount attempt done: mounted=%u", (unsigned)s_mounted);
        return s_mounted;
    }

    bsp_fs_log("Init start");

    BOARD_InitExtFlashPins();

    CLOCK_EnableClock(kCLOCK_Lpspi1);
    CLOCK_SetIpSrc(kCLOCK_Lpspi1, kCLOCK_IpSrcFro192M);
    CLOCK_SetIpSrcDiv(kCLOCK_Lpspi1, kSCG_SysClkDivBy1);
    CLOCK_EnableClockLPMode(kCLOCK_Lpspi1, kCLOCK_IpClkControl_fun1);

    bsp_fs_log("LPSPI1 clk=%u baud=%u",
               (unsigned)BOARD_GetLpspiClock(),
               (unsigned)BOARD_GetNorFlashBaudrate());

    bsp_fs_flash_recover(LPSPI1);

    if (!s_mutexCreated)
    {
        if (KOSA_StatusSuccess == OSA_MutexCreate((osa_mutex_handle_t)s_fsMutexHandle))
        {
            s_mutexCreated = true;
        }
    }

    bsp_fs_log("Flash cfg: page=%u sector=%u mem=%u",
               (unsigned)s_memCfg.bytesInPageSize,
               (unsigned)s_memCfg.bytesInSectorSize,
               (unsigned)s_memCfg.bytesInMemorySize);

    s_norCfg.memControlConfig = &s_memCfg;
    s_norCfg.driverBaseAddr   = NULL;
    const status_t norInitSt = Nor_Flash_Init(&s_norCfg, &s_norHandle);
    if (norInitSt != kStatus_Success)
    {
        bsp_fs_log("Nor_Flash_Init failed: status=%d", (int)norInitSt);
        s_initialized = false;
        s_mounted     = false;
        return false;
    }
    if (s_norHandle.bytesInPageSize != 0U)
    {
        s_memCfg.bytesInPageSize = s_norHandle.bytesInPageSize;
    }
    if (s_norHandle.bytesInSectorSize != 0U)
    {
        s_memCfg.bytesInSectorSize = s_norHandle.bytesInSectorSize;
    }
    if (s_norHandle.bytesInMemorySize != 0U)
    {
        s_memCfg.bytesInMemorySize = s_norHandle.bytesInMemorySize;
    }

    bsp_fs_log("Flash ready: page=%u sector=%u mem=%u fs_size=%u crashlog_base=0x%08X",
               (unsigned)s_memCfg.bytesInPageSize,
               (unsigned)s_memCfg.bytesInSectorSize,
               (unsigned)s_memCfg.bytesInMemorySize,
               (unsigned)bsp_fs_get_fs_size(),
               (unsigned)bsp_fs_get_crashlog_base());

    if (bsp_fs_lock(5000U) == 0)
    {
        bsp_fs_unlock();
    }

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
    s_lfsCfg.block_count    = bsp_fs_get_fs_size() / s_memCfg.bytesInSectorSize;
    if (BSP_FS_BLOCK_COUNT_OVERRIDE != 0U)
    {
        s_lfsCfg.block_count = (lfs_size_t)BSP_FS_BLOCK_COUNT_OVERRIDE;
    }
    s_lfsCfg.cache_size     = s_memCfg.bytesInPageSize;
    s_lfsCfg.lookahead_size = (uint32_t)sizeof(s_lookahead);
    s_lfsCfg.block_cycles   = 500U;
    s_lfsCfg.read_buffer    = s_readCache;
    s_lfsCfg.prog_buffer    = s_progCache;
    s_lfsCfg.lookahead_buffer = s_lookahead;

    bsp_fs_log("LFS cfg: read=%u prog=%u block=%u count=%u cache=%u lookahead=%u cycles=%u",
               (unsigned)s_lfsCfg.read_size,
               (unsigned)s_lfsCfg.prog_size,
               (unsigned)s_lfsCfg.block_size,
               (unsigned)s_lfsCfg.block_count,
               (unsigned)s_lfsCfg.cache_size,
               (unsigned)s_lfsCfg.lookahead_size,
               (unsigned)s_lfsCfg.block_cycles);

    if (bsp_fs_lock(5000U) != 0)
    {
        bsp_fs_log("Init lock failed");
        s_initialized = false;
        s_mounted     = false;
        return false;
    }

    const int err = bsp_fs_mount_with_repair_locked();

    bsp_fs_unlock();

    s_initialized = true;
    s_mounted     = (err == 0);
    bsp_fs_log("Init done: mounted=%u", (unsigned)s_mounted);
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
            const int uerr = lfs_unmount(&s_lfs);
            bsp_fs_log("lfs_unmount: err=%d", uerr);
        }
        bsp_fs_unlock();
    }

    (void)Nor_Flash_DeInit(&s_norHandle);
    s_mounted     = false;
    s_initialized = false;
    bsp_fs_log("Deinit done");
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
        bsp_fs_log("Mkdir lock failed: %s", path);
        return -1;
    }

    int err = lfs_mkdir(&s_lfs, path);
    if (err == LFS_ERR_CORRUPT)
    {
        bsp_fs_log("Mkdir got CORRUPT, try repair");
        const int rerr = bsp_fs_repair_on_corrupt_locked();
        bsp_fs_log("Repair result: err=%d,%s", rerr, bsp_fs_lfs_err_str(rerr));
        if (rerr == 0)
        {
            err = lfs_mkdir(&s_lfs, path);
        }
        else
        {
            s_mounted = false;
        }
    }
    bsp_fs_unlock();

    if ((err == 0) || (err == LFS_ERR_EXIST))
    {
        bsp_fs_log("Mkdir: %s ok (err=%d,%s)", path, err, bsp_fs_lfs_err_str(err));
        return 0;
    }
    bsp_fs_log("Mkdir: %s failed (err=%d,%s)", path, err, bsp_fs_lfs_err_str(err));
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
        bsp_fs_log("Remove lock failed: %s", path);
        return -1;
    }
    const int err = lfs_remove(&s_lfs, path);
    bsp_fs_unlock();

    if ((err == 0) || (err == LFS_ERR_NOENT))
    {
        bsp_fs_log("Remove: %s ok (err=%d,%s)", path, err, bsp_fs_lfs_err_str(err));
        return 0;
    }
    bsp_fs_log("Remove: %s failed (err=%d,%s)", path, err, bsp_fs_lfs_err_str(err));
    return err;
}

bool BSP_FS_Format(void)
{
    if (!s_initialized)
    {
        (void)BSP_FS_Init();
    }

    if ((s_norHandle.driverBaseAddr == NULL) || (s_lfsCfg.block_size == 0U) || (s_lfsCfg.block_count == 0U))
    {
        bsp_fs_log("Format aborted: flash/lfs cfg not ready (drv=%u block=%u count=%u)",
                   (unsigned)(s_norHandle.driverBaseAddr != NULL),
                   (unsigned)s_lfsCfg.block_size,
                   (unsigned)s_lfsCfg.block_count);
        return false;
    }

    BOARD_InitExtFlashPins();
    CLOCK_EnableClock(kCLOCK_Lpspi1);
    CLOCK_SetIpSrc(kCLOCK_Lpspi1, kCLOCK_IpSrcFro192M);
    CLOCK_SetIpSrcDiv(kCLOCK_Lpspi1, kSCG_SysClkDivBy1);
    CLOCK_EnableClockLPMode(kCLOCK_Lpspi1, kCLOCK_IpClkControl_fun1);
    bsp_fs_flash_recover((LPSPI_Type *)s_norHandle.driverBaseAddr);

    bsp_fs_log("Format start: mounted=%u block=%u count=%u prog=%u cache=%u lookahead=%u",
               (unsigned)s_mounted,
               (unsigned)s_lfsCfg.block_size,
               (unsigned)s_lfsCfg.block_count,
               (unsigned)s_lfsCfg.prog_size,
               (unsigned)s_lfsCfg.cache_size,
               (unsigned)s_lfsCfg.lookahead_size);

    if (bsp_fs_lock(5000U) != 0)
    {
        bsp_fs_log("Format lock failed");
        return false;
    }

    int uerr = 0;
    if (s_mounted)
    {
        uerr = lfs_unmount(&s_lfs);
        bsp_fs_log("lfs_unmount before format: err=%d,%s", uerr, bsp_fs_lfs_err_str(uerr));
        s_mounted = false;
    }

    const uint32_t fsSize = bsp_fs_get_effective_fs_size_bytes();
    const uint32_t sector = s_memCfg.bytesInSectorSize;
    if ((fsSize != 0U) && (sector != 0U))
    {
        bsp_fs_log("physical erase before format: bytes=%u sector=%u", (unsigned)fsSize, (unsigned)sector);
        for (uint32_t addr = 0U; addr < fsSize; addr += sector)
        {
            const status_t est = Nor_Flash_Erase_Sector(&s_norHandle, addr);
            if (est != kStatus_Success)
            {
                bsp_fs_log("physical erase failed: st=%d addr=0x%08X", (int)est, (unsigned)addr);
                bsp_fs_unlock();
                return false;
            }
            const bool need_verify = (BSP_FS_VERIFY_ALL_ERASE_ON_FORMAT != 0) ||
                                     (addr < (2U * sector)) ||
                                     (((addr / sector) & 0xFFU) == 0xFFU) ||
                                     ((addr + sector) >= fsSize);
            if (need_verify)
            {
                if (bsp_fs_verify_erased_sector(addr, sector) != 0)
                {
                    bsp_fs_log("erase verify failed at sector addr=0x%08X", (unsigned)addr);
                    bsp_fs_unlock();
                    return false;
                }
            }
            if (((addr / sector) & 0x7FU) == 0x7FU)
            {
                bsp_fs_log("physical erase progress: %u/%u", (unsigned)(addr / sector + 1U), (unsigned)(fsSize / sector));
            }
        }
    }

    const int ferr = lfs_format(&s_lfs, &s_lfsCfg);
    const int merr = lfs_mount(&s_lfs, &s_lfsCfg);

    bsp_fs_unlock();

    s_mounted = ((ferr == 0) && (merr == 0));
    bsp_fs_log("Format done: format_err=%d,%s mount_err=%d,%s mounted=%u",
               ferr, bsp_fs_lfs_err_str(ferr),
               merr, bsp_fs_lfs_err_str(merr),
               (unsigned)s_mounted);
    if (ferr != 0)
    {
    }
    return s_mounted;
}

int BSP_FS_FileAppend(const char *path, const void *data, uint32_t size)
{
    if ((path == NULL) || (data == NULL) || (size == 0U) || (!s_mounted))
    {
        return -1;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        bsp_fs_log("FileAppend lock failed: %s", path);
        return -1;
    }

    lfs_file_t f;
    int err = lfs_file_open(&s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
    if (err == LFS_ERR_CORRUPT)
    {
        bsp_fs_log("FileAppend got CORRUPT, try repair");
        const int rerr = bsp_fs_repair_on_corrupt_locked();
        bsp_fs_log("Repair result: err=%d,%s", rerr, bsp_fs_lfs_err_str(rerr));
        if (rerr == 0)
        {
            err = lfs_file_open(&s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
        }
        else
        {
            s_mounted = false;
        }
    }

    if (err == LFS_ERR_NOENT)
    {
        s_diag.fileappend_open_noent++;
        bsp_fs_log("FileAppend open failed: %s err=%d,%s", path, err, bsp_fs_lfs_err_str(err));
        bsp_fs_log_path_ctx_locked("FileAppend noent", path);

        bsp_fs_flash_recover((LPSPI_Type *)s_norHandle.driverBaseAddr);
        const int rerr = lfs_file_open(&s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
        bsp_fs_log("FileAppend open retry: %s err=%d,%s", path, rerr, bsp_fs_lfs_err_str(rerr));
        err = rerr;
        if (err == 0)
        {
            s_diag.fileappend_open_noent_recovered++;
        }
    }
    else if (err == LFS_ERR_IO)
    {
        s_diag.fileappend_open_io++;
        s_diag.io_error++;
    }

    if (err == 0)
    {
        const lfs_ssize_t w = lfs_file_write(&s_lfs, &f, data, (lfs_size_t)size);
        if (w != (lfs_ssize_t)size)
        {
            err = (int)w;
        }
        const int cerr = lfs_file_close(&s_lfs, &f);
        if (cerr != 0)
        {
            bsp_fs_log("FileAppend close failed: %s err=%d,%s", path, cerr, bsp_fs_lfs_err_str(cerr));
            if (err == 0)
            {
                err = cerr;
            }
        }

        if ((err == LFS_ERR_CORRUPT) || (cerr == LFS_ERR_CORRUPT))
        {
            bsp_fs_log("FileAppend got CORRUPT, try repair+retry");
            const int rerr = bsp_fs_repair_on_corrupt_locked();
            bsp_fs_log("Repair result: err=%d,%s", rerr, bsp_fs_lfs_err_str(rerr));
            if (rerr == 0)
            {
                lfs_file_t rf;
                int re = lfs_file_open(&s_lfs, &rf, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
                if (re == 0)
                {
                    const lfs_ssize_t rw = lfs_file_write(&s_lfs, &rf, data, (lfs_size_t)size);
                    if (rw != (lfs_ssize_t)size)
                    {
                        re = (int)rw;
                    }
                    const int rc = lfs_file_close(&s_lfs, &rf);
                    if ((rc != 0) && (re == 0))
                    {
                        re = rc;
                    }
                }
                if (re == 0)
                {
                    err = 0;
                }
                else if (err == 0)
                {
                    err = re;
                }
            }
            else
            {
                s_mounted = false;
            }
        }
    }
    else
    {
        if (err == LFS_ERR_NOENT)
        {
            bsp_fs_log_path_ctx_locked("FileAppend noent(ret)", path);
        }
    }

    if (err == LFS_ERR_NOENT)
    {
        bsp_fs_log_path_ctx_locked("FileAppend noent(ret2)", path);
    }

    bsp_fs_unlock();
    bsp_fs_log("FileAppend: %s size=%u err=%d", path, (unsigned)size, err);
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
        bsp_fs_log("FileWriteTruncate lock failed: %s", path);
        return -1;
    }

    lfs_file_t f;
    int err = lfs_file_open(&s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err == LFS_ERR_CORRUPT)
    {
        bsp_fs_log("FileWriteTruncate got CORRUPT, try repair");
        const int rerr = bsp_fs_repair_on_corrupt_locked();
        bsp_fs_log("Repair result: err=%d,%s", rerr, bsp_fs_lfs_err_str(rerr));
        if (rerr == 0)
        {
            err = lfs_file_open(&s_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        }
        else
        {
            s_mounted = false;
        }
    }
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
    bsp_fs_log("FileWriteTruncate: %s size=%u err=%d", path, (unsigned)size, err);
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
        bsp_fs_log("FileReadAt lock failed: %s", path);
        return -1;
    }

    lfs_file_t f;
    int err = lfs_file_open(&s_lfs, &f, path, LFS_O_RDONLY);
    if (err == LFS_ERR_CORRUPT)
    {
        bsp_fs_log("FileReadAt got CORRUPT, try repair");
        const int rerr = bsp_fs_repair_on_corrupt_locked();
        bsp_fs_log("Repair result: err=%d,%s", rerr, bsp_fs_lfs_err_str(rerr));
        if (rerr == 0)
        {
            err = lfs_file_open(&s_lfs, &f, path, LFS_O_RDONLY);
        }
        else
        {
            s_mounted = false;
        }
    }
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
    bsp_fs_log("FileReadAt: %s off=%u size=%u ret=%d", path, (unsigned)offset, (unsigned)size, err);
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
        bsp_fs_log("FileSize lock failed: %s", path);
        return -1;
    }

    lfs_file_t f;
    int err = lfs_file_open(&s_lfs, &f, path, LFS_O_RDONLY);
    if (err == LFS_ERR_CORRUPT)
    {
        bsp_fs_log("FileSize got CORRUPT, try repair");
        const int rerr = bsp_fs_repair_on_corrupt_locked();
        bsp_fs_log("Repair result: err=%d,%s", rerr, bsp_fs_lfs_err_str(rerr));
        if (rerr == 0)
        {
            err = lfs_file_open(&s_lfs, &f, path, LFS_O_RDONLY);
        }
        else
        {
            s_mounted = false;
        }
    }
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
    if (err == 0)
    {
        bsp_fs_log("FileSize: %s size=%u", path, (unsigned)*out_size);
    }
    else
    {
        bsp_fs_log("FileSize: %s err=%d", path, err);
    }
    return err;
}

bool BSP_FS_CrashLog_Erase(void)
{
    if (!s_initialized)
    {
        (void)BSP_FS_Init();
    }
    if (!s_initialized)
    {
        bsp_fs_log("CrashLog_Erase aborted: init failed");
        return false;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        bsp_fs_log("CrashLog_Erase lock failed");
        return false;
    }
    BOARD_InitExtFlashPins();
    bsp_fs_flash_recover((LPSPI_Type *)s_norHandle.driverBaseAddr);

    const uint32_t addr = bsp_fs_get_crashlog_base();
    const status_t st = Nor_Flash_Erase_Sector(&s_norHandle, addr);
    bsp_fs_unlock();
    bsp_fs_log("CrashLog_Erase: addr=0x%08X status=%d", (unsigned)addr, (int)st);
    return (st == kStatus_Success);
}

bool BSP_FS_CrashLog_Write(const void *data, uint32_t size)
{
    if ((data == NULL) || (size == 0U))
    {
        return false;
    }

    if (!s_initialized)
    {
        (void)BSP_FS_Init();
    }
    if (!s_initialized)
    {
        bsp_fs_log("CrashLog_Write aborted: init failed");
        return false;
    }

    const uint32_t maxSize = s_memCfg.bytesInSectorSize;
    if (size > maxSize)
    {
        size = maxSize;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        bsp_fs_log("CrashLog_Write lock failed");
        return false;
    }
    BOARD_InitExtFlashPins();
    bsp_fs_flash_recover((LPSPI_Type *)s_norHandle.driverBaseAddr);

    const uint32_t addr = bsp_fs_get_crashlog_base();
    const status_t est = Nor_Flash_Erase_Sector(&s_norHandle, addr);
    if (est != kStatus_Success)
    {
        bsp_fs_unlock();
        bsp_fs_log("CrashLog_Write erase failed: addr=0x%08X status=%d", (unsigned)addr, (int)est);
        return false;
    }

    const status_t pst = Nor_Flash_Program(&s_norHandle, addr, (uint8_t *)(uintptr_t)data, size);
    bsp_fs_unlock();
    bsp_fs_log("CrashLog_Write: addr=0x%08X size=%u status=%d", (unsigned)addr, (unsigned)size, (int)pst);
    return (pst == kStatus_Success);
}

int BSP_FS_CrashLog_Read(uint32_t offset, void *out, uint32_t size)
{
    if ((out == NULL) || (size == 0U))
    {
        return -1;
    }

    if (!s_initialized)
    {
        (void)BSP_FS_Init();
    }
    if (!s_initialized)
    {
        bsp_fs_log("CrashLog_Read aborted: init failed");
        return -1;
    }

    if (offset >= s_memCfg.bytesInSectorSize)
    {
        return 0;
    }

    uint32_t remaining = s_memCfg.bytesInSectorSize - offset;
    if (size > remaining)
    {
        size = remaining;
    }

    if (bsp_fs_lock(5000U) != 0)
    {
        bsp_fs_log("CrashLog_Read lock failed");
        return -1;
    }
    BOARD_InitExtFlashPins();
    bsp_fs_flash_recover((LPSPI_Type *)s_norHandle.driverBaseAddr);

    const uint32_t addr = bsp_fs_get_crashlog_base() + offset;
    const status_t rst = Nor_Flash_Read(&s_norHandle, addr, (uint8_t *)out, size);
    bsp_fs_unlock();
    if (rst != kStatus_Success)
    {
        bsp_fs_log("CrashLog_Read failed: addr=0x%08X size=%u status=%d", (unsigned)addr, (unsigned)size, (int)rst);
        return -1;
    }
    bsp_fs_log("CrashLog_Read: addr=0x%08X size=%u ok", (unsigned)addr, (unsigned)size);
    return (int)size;
}

void BSP_FS_GetDiag(bsp_fs_diag_t *out, bool clear)
{
    if (out != NULL)
    {
        *out = s_diag;
    }
    if (clear)
    {
        memset(&s_diag, 0, sizeof(s_diag));
    }
}
