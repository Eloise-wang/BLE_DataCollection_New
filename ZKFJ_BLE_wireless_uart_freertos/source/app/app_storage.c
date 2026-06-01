/*
 * app_storage.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
 *
 *  Storage Architecture v2:
 *  - Sector 0:        Super block (64B metadata + atomic write support)
 *  - Sector 1-2:      Header table (2x64 = 128 task slots, wear-leveled pair)
 *  - Sector 3+:       Sequential data pool (shared by all tasks)
 *
 *  Each task header stores: task_id, meta, data_pool_offset, data_size, pre_size.
 *  The data pool grows sequentially; deleted tasks free their space.
 *  A garbage-collection pass can reclaim fragmented free space.
 */

#include "app_storage.h"

#include <stddef.h>
#include <string.h>

#include "bsp_uart.h"
#include "bsp_crc.h"

#include "board.h"
#include "fsl_clock.h"
#include "fsl_lpspi_mem_adapter.h"
#include "fsl_lpspi_nor_flash.h"
#include "fsl_os_abstraction.h"
#include "pin_mux.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#if defined(gAppLowpowerEnabled_d) && (gAppLowpowerEnabled_d > 0)
#include "PWR_Interface.h"
#endif

/* ---------- Constants ---------- */

#define APP_STORE_MAGIC             0x474F4C54u   /* "STOR" */
#define APP_STORE_VERSION           2u

#if defined(DEBUG)
#define APP_STORE_WRITE_VERIFY_ENABLE 1u
#else
#define APP_STORE_WRITE_VERIFY_ENABLE 1u
#endif

#define APP_STORE_TASK_SLOTS        64u           /* headers per sector */
#define APP_STORE_HEADER_SIZE       64u           /* bytes per task header */
#define APP_STORE_HEAD_SECTORS      2u            /* header table sector pair */
#define APP_STORE_SUPER_SECTOR     0u
#define APP_STORE_SUPER_ADDR       0u

/* ---------- NOR Flash config ---------- */

static lpspi_memory_config_t s_memCfg = {
    .bytesInPageSize   = 256u,
    .bytesInSectorSize = 4096u,
    .bytesInMemorySize = (8u * 1024u * 1024u),
};

/* ---------- Layout computed at init ---------- */

static bool s_layout_ready;
static uint32_t s_sector_size;
static uint32_t s_mem_size;
static uint32_t s_super_sector_addr;
static uint32_t s_head_sector_addr[APP_STORE_HEAD_SECTORS];
static uint32_t s_data_pool_addr;
static uint32_t s_data_pool_size;

/* ---------- Flash driver state ---------- */

static nor_config_t s_norCfg;
static nor_handle_t s_norHandle;
static bool s_flash_inited;
static bool s_crc_inited;

/* ---------- Active task cache ---------- */

typedef struct
{
    bool valid;
    uint8_t head_sector;   /* which header sector (0 or 1) */
    uint8_t head_index;    /* index within that sector */
    uint64_t task_id;
    uint32_t data_offset;  /* offset within data pool */
    uint32_t data_size;
    uint32_t pre_size;
    uint32_t pre_offset;   /* offset within data pool for pre-test data */
} app_store_active_t;

static app_store_active_t s_active;

/* ---------- Error reporting ---------- */

typedef enum
{
    APP_STORE_ERR_NONE = 0,
    APP_STORE_ERR_INIT = 1,
    APP_STORE_ERR_LAYOUT = 2,
    APP_STORE_ERR_RANGE = 3,
    APP_STORE_ERR_READ = 4,
    APP_STORE_ERR_PROG = 5,
    APP_STORE_ERR_ERASE = 6,
    APP_STORE_ERR_BUSY_QUERY = 7,
    APP_STORE_ERR_BUSY_TIMEOUT = 8,
    APP_STORE_ERR_HEADER_MAGIC = 9,
    APP_STORE_ERR_HEADER_VERSION = 10,
    APP_STORE_ERR_HEADER_CRC = 11,
    APP_STORE_ERR_OPEN_NO_SLOT = 12,
    APP_STORE_ERR_OPEN_HEADER_RECHECK = 13,
    APP_STORE_ERR_VERIFY_MISMATCH = 14,
    APP_STORE_ERR_VERIFY_READ_UNSTABLE = 15,
    APP_STORE_ERR_SUPER_CRC = 16,
    APP_STORE_ERR_SUPER_MAGIC = 17,
    APP_STORE_ERR_NO_SPACE = 18,
    APP_STORE_ERR_POOL_FULL = 19,
} app_store_err_t;

static app_store_err_t s_last_err;
static status_t s_last_status;
static uint32_t s_last_addr;
static uint32_t s_last_size;
static TickType_t s_last_begin_fail_log_tick;
static TickType_t s_last_append_fail_log_tick;
static TickType_t s_last_verify_mismatch_log_tick;
static uint32_t s_lp_disallow_count;

/* ---------- Super block & Header structures ---------- */
/* Packed to ensure exact 64-byte layout regardless of compiler padding */

APP_STORAGE_PACKED typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t head_sector;      /* 0 or 1: which header sector is active */
    uint32_t wear_count;       /* incremented on each super-block write */
    uint32_t pool_write_ptr;   /* next free offset in data pool */
    uint32_t pool_size;        /* total data pool size in bytes */
    uint32_t reserved[9];
    uint32_t crc;
} app_store_super_t;

APP_STORAGE_PACKED typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint64_t task_id;
    app_storage_task_meta_t meta;
    uint32_t status;           /* 0=free, 1=active, 2=deleted */
    uint32_t data_offset;       /* offset in data pool */
    uint32_t data_size;        /* committed data bytes */
    uint32_t pre_offset;       /* offset in data pool for pre-test data */
    uint32_t pre_size;         /* committed pre-test data bytes */
    uint32_t reserved[4];
    uint32_t crc;
} app_store_header_t;

_Static_assert(sizeof(app_store_super_t)  == 64u, "super_t must be 64 bytes");
_Static_assert(sizeof(app_store_header_t) == 64u, "header_t must be 64 bytes");

#define APP_STORE_HEAD_FREE   0u
#define APP_STORE_HEAD_ACTIVE 1u
#define APP_STORE_HEAD_DELETED 2u

/* ---------- Mutex & low-power guard ---------- */

static SemaphoreHandle_t s_flash_mutex;

static void app_store_lock(void)
{
    if ((s_flash_mutex == NULL) && (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        s_flash_mutex = xSemaphoreCreateMutex();
    }
    if (s_flash_mutex != NULL)
    {
        (void)xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
    }

#if defined(gAppLowpowerEnabled_d) && (gAppLowpowerEnabled_d > 0)
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        if (s_lp_disallow_count == 0u)
        {
            PWR_DisallowDeviceToSleep();
        }
        s_lp_disallow_count++;
    }
#endif
}

static void app_store_unlock(void)
{
#if defined(gAppLowpowerEnabled_d) && (gAppLowpowerEnabled_d > 0)
    if ((xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) && (s_lp_disallow_count != 0u))
    {
        s_lp_disallow_count--;
        if (s_lp_disallow_count == 0u)
        {
            PWR_AllowDeviceToSleep();
        }
    }
#endif

    if (s_flash_mutex != NULL)
    {
        (void)xSemaphoreGive(s_flash_mutex);
    }
}

/* ---------- Flash low-level helpers ---------- */

static void app_store_flash_recover_locked(void)
{
    if (s_norHandle.driverBaseAddr != NULL)
    {
        (void)Nor_Flash_DeInit(&s_norHandle);
    }

    BOARD_InitExtFlashPins();
    CLOCK_EnableClock(kCLOCK_Lpspi1);
    CLOCK_SetIpSrc(kCLOCK_Lpspi1, kCLOCK_IpSrcFro192M);
    CLOCK_SetIpSrcDiv(kCLOCK_Lpspi1, kSCG_SysClkDivBy1);
    CLOCK_EnableClockLPMode(kCLOCK_Lpspi1, kCLOCK_IpClkControl_fun1);

    s_norCfg.memControlConfig = &s_memCfg;
    s_norCfg.driverBaseAddr   = NULL;
    (void)Nor_Flash_Init(&s_norCfg, &s_norHandle);
    OSA_TimeDelay(1u);
}

static int app_store_wait_ready(uint32_t timeout_ms)
{
    for (uint32_t waited = 0u; waited < timeout_ms; waited++)
    {
        bool busy = false;
        const status_t st = Nor_Flash_Is_Busy(&s_norHandle, &busy);
        if (st != kStatus_Success)
        {
            s_last_err  = APP_STORE_ERR_BUSY_QUERY;
            s_last_status = st;
            return -1;
        }
        if (!busy)
        {
            return 0;
        }
        OSA_TimeDelay(1u);
    }
    s_last_err    = APP_STORE_ERR_BUSY_TIMEOUT;
    s_last_status = kStatus_Fail;
    return -1;
}

static bool app_store_flash_init_locked(void)
{
    if (s_flash_inited)
    {
        return true;
    }

    s_layout_ready = false;
    s_last_err     = APP_STORE_ERR_NONE;
    s_last_status  = kStatus_Success;

    BOARD_InitExtFlashPins();
    CLOCK_EnableClock(kCLOCK_Lpspi1);
    CLOCK_SetIpSrc(kCLOCK_Lpspi1, kCLOCK_IpSrcFro192M);
    CLOCK_SetIpSrcDiv(kCLOCK_Lpspi1, kSCG_SysClkDivBy1);
    CLOCK_EnableClockLPMode(kCLOCK_Lpspi1, kCLOCK_IpClkControl_fun1);

    s_norCfg.memControlConfig = &s_memCfg;
    s_norCfg.driverBaseAddr   = NULL;
    const status_t st = Nor_Flash_Init(&s_norCfg, &s_norHandle);
    if (st != kStatus_Success)
    {
        s_last_err    = APP_STORE_ERR_INIT;
        s_last_status = st;
        return false;
    }

    if (s_norHandle.driverBaseAddr == NULL)
    {
        s_last_err    = APP_STORE_ERR_INIT;
        s_last_status = kStatus_Fail;
        return false;
    }

    if (s_norHandle.bytesInPageSize != 0u)
    {
        s_memCfg.bytesInPageSize = s_norHandle.bytesInPageSize;
    }
    if (s_norHandle.bytesInSectorSize != 0u)
    {
        s_memCfg.bytesInSectorSize = s_norHandle.bytesInSectorSize;
    }
    if (s_norHandle.bytesInMemorySize != 0u)
    {
        s_memCfg.bytesInMemorySize = s_norHandle.bytesInMemorySize;
    }

    s_sector_size = s_memCfg.bytesInSectorSize;
    s_mem_size    = s_memCfg.bytesInMemorySize;
    if ((s_sector_size == 0u) || (s_mem_size < (3u * s_sector_size)))
    {
        s_last_err = APP_STORE_ERR_LAYOUT;
        return false;
    }

    /* Compute layout */
    s_super_sector_addr   = 0u;
    s_head_sector_addr[0] = s_sector_size;
    s_head_sector_addr[1] = s_sector_size * 2u;
    s_data_pool_addr       = s_sector_size * (2u + APP_STORE_HEAD_SECTORS);
    s_data_pool_size       = s_mem_size - s_data_pool_addr;

    /* Pool must be large enough to hold at least one record per task slot */
    const uint32_t min_pool = APP_STORE_TASK_SLOTS * 20u;
    if (s_data_pool_size < min_pool)
    {
        s_last_err = APP_STORE_ERR_LAYOUT;
        return false;
    }

    s_layout_ready = true;
    s_flash_inited = true;
    return true;
}

static bool app_store_flash_read_locked(uint32_t addr, void *out, uint32_t size)
{
    if ((out == NULL) || (size == 0u))
    {
        return false;
    }
    if ((s_norHandle.driverBaseAddr == NULL) || (addr + size > s_mem_size))
    {
        s_last_err    = APP_STORE_ERR_RANGE;
        s_last_addr   = addr;
        s_last_size   = size;
        return false;
    }
    const status_t st = Nor_Flash_Read(&s_norHandle, addr, (uint8_t *)out, size);
    if (st != kStatus_Success)
    {
        s_last_err    = APP_STORE_ERR_READ;
        s_last_status = st;
        s_last_addr   = addr;
        s_last_size   = size;
        return false;
    }
    return true;
}

static bool app_store_flash_prog_locked(uint32_t addr, const void *data, uint32_t size)
{
    if ((data == NULL) || (size == 0u))
    {
        return false;
    }
    if ((s_norHandle.driverBaseAddr == NULL) || (addr + size > s_mem_size))
    {
        s_last_err    = APP_STORE_ERR_RANGE;
        s_last_addr   = addr;
        s_last_size   = size;
        return false;
    }

    const uint32_t max_attempts = 6u;
    for (uint32_t attempt = 0u; attempt < max_attempts; attempt++)
    {
        if (app_store_wait_ready(2000u) != 0)
        {
            app_store_flash_recover_locked();
            OSA_TimeDelay(1u << (attempt < 5u ? attempt : 5u));
            continue;
        }

        const status_t st = Nor_Flash_Program(&s_norHandle, addr, (uint8_t *)(uintptr_t)data, size);
        if (st == kStatus_Success)
        {
            if (app_store_wait_ready(2000u) != 0)
            {
                app_store_flash_recover_locked();
                OSA_TimeDelay(1u << (attempt < 5u ? attempt : 5u));
                continue;
            }
            goto verify;
        }

        s_last_err    = APP_STORE_ERR_PROG;
        s_last_status = st;
        s_last_addr   = addr;
        s_last_size   = size;
        app_store_flash_recover_locked();
        OSA_TimeDelay(1u << (attempt < 5u ? attempt : 5u));
    }
    return false;

verify:
#if APP_STORE_WRITE_VERIFY_ENABLE
    uint8_t buf[64];
    const uint8_t *src = (const uint8_t *)data;
    uint32_t off = 0u;
    while (off < size)
    {
        uint32_t chunk = (size - off > (uint32_t)sizeof(buf)) ? (uint32_t)sizeof(buf) : (size - off);
        if (!app_store_flash_read_locked(addr + off, buf, chunk))
        {
            return false;
        }
        if (memcmp(buf, src + off, chunk) != 0)
        {
            const TickType_t now = xTaskGetTickCount();
            uint8_t buf2[sizeof(buf)];
            uint8_t buf3[sizeof(buf)];
            status_t st2 = kStatus_Fail;
            status_t st3 = kStatus_Fail;
            bool read2_ok = false;
            bool read3_ok = false;
            bool ok_on_retry = false;

            if (app_store_flash_read_locked(addr + off, buf2, chunk))
            {
                st2 = kStatus_Success;
                read2_ok = true;
                if (memcmp(buf2, src + off, chunk) == 0)
                {
                    ok_on_retry = true;
                }
            }
            if (!ok_on_retry && app_store_flash_read_locked(addr + off, buf3, chunk))
            {
                st3 = kStatus_Success;
                read3_ok = true;
                if (memcmp(buf3, src + off, chunk) == 0)
                {
                    ok_on_retry = true;
                }
            }

            if (ok_on_retry)
            {
                if ((now - s_last_verify_mismatch_log_tick) > pdMS_TO_TICKS(2000U))
                {
                    s_last_verify_mismatch_log_tick = now;
                    BSP_UART_Print("[STO] VerifyRetryOk addr=0x%08X\r\n", (unsigned)(addr + off));
                }
                off += chunk;
                continue;
            }

            bool read_unstable = (read2_ok && (memcmp(buf, buf2, chunk) != 0)) ||
                                 (read2_ok && read3_ok && (memcmp(buf2, buf3, chunk) != 0));

            if ((now - s_last_verify_mismatch_log_tick) > pdMS_TO_TICKS(2000U))
            {
                s_last_verify_mismatch_log_tick = now;
                BSP_UART_Print("[STO] VerifyMismatch addr=0x%08X size=%u unstable=%u\r\n",
                               (unsigned)(addr + off), (unsigned)chunk, (unsigned)read_unstable);
            }

            s_last_err    = read_unstable ? APP_STORE_ERR_VERIFY_READ_UNSTABLE : APP_STORE_ERR_VERIFY_MISMATCH;
            s_last_status = kStatus_Fail;
            s_last_addr   = addr + off;
            s_last_size   = chunk;
            return false;
        }
        off += chunk;
    }
#endif
    return true;
}

static bool app_store_flash_erase_sector_locked(uint32_t addr)
{
    if ((s_norHandle.driverBaseAddr == NULL) || (s_sector_size == 0u))
    {
        s_last_err    = APP_STORE_ERR_INIT;
        s_last_status = kStatus_Fail;
        return false;
    }
    const uint32_t base = (addr / s_sector_size) * s_sector_size;
    if (base >= s_mem_size)
    {
        s_last_err    = APP_STORE_ERR_RANGE;
        s_last_status = kStatus_Fail;
        return false;
    }

    const uint32_t max_attempts = 4u;
    for (uint32_t attempt = 0u; attempt < max_attempts; attempt++)
    {
        if (app_store_wait_ready(5000u) != 0)
        {
            app_store_flash_recover_locked();
            OSA_TimeDelay(2u << (attempt < 3u ? attempt : 3u));
            continue;
        }

        const status_t st = Nor_Flash_Erase_Sector(&s_norHandle, base);
        if (st == kStatus_Success)
        {
            return (app_store_wait_ready(5000u) == 0);
        }

        s_last_err    = APP_STORE_ERR_ERASE;
        s_last_status = st;
        app_store_flash_recover_locked();
        OSA_TimeDelay(2u << (attempt < 3u ? attempt : 3u));
    }
    return false;
}

/* ---------- Super block helpers ---------- */

static bool app_store_super_read_locked(app_store_super_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    (void)memset(out, 0, sizeof(*out));
    if (!app_store_flash_read_locked(APP_STORE_SUPER_ADDR, out, (uint32_t)sizeof(*out)))
    {
        return false;
    }
    if (out->magic != APP_STORE_MAGIC)
    {
        s_last_err  = APP_STORE_ERR_SUPER_MAGIC;
        return false;
    }
    const uint32_t crc_len = (uint32_t)(offsetof(app_store_super_t, crc) - offsetof(app_store_super_t, version));
    const uint32_t crc = BSP_CRC_Calculate(&out->version, crc_len, BSP_CRC_WIDTH_32);
    if (crc != out->crc)
    {
        s_last_err = APP_STORE_ERR_SUPER_CRC;
        return false;
    }
    return true;
}

static bool app_store_super_write_locked(const app_store_super_t *super)
{
    app_store_super_t s = *super;
    s.magic   = APP_STORE_MAGIC;
    s.version = APP_STORE_VERSION;
    s.crc = BSP_CRC_Calculate(&s.version,
                              (uint32_t)(offsetof(app_store_super_t, crc) - offsetof(app_store_super_t, version)),
                              BSP_CRC_WIDTH_32);

    /* Atomic write: write everything after magic first, then magic last.
     * This prevents reading a partially-written super block as valid. */
    const uint32_t off_after_magic = 4u;
    if (!app_store_flash_prog_locked(APP_STORE_SUPER_ADDR + off_after_magic,
                                     ((const uint8_t *)&s) + off_after_magic,
                                     (uint32_t)sizeof(s) - off_after_magic))
    {
        return false;
    }
    if (!app_store_flash_prog_locked(APP_STORE_SUPER_ADDR, &s.magic, (uint32_t)sizeof(s.magic)))
    {
        return false;
    }
    return true;
}

static bool app_store_super_init_locked(app_store_super_t *out_super)
{
    app_store_super_t super;
    (void)memset(&super, 0xFF, sizeof(super));
    super.head_sector   = 0u;
    super.wear_count    = 0u;
    super.pool_write_ptr = 0u;
    super.pool_size     = s_data_pool_size;
    super.reserved[0]   = 0u;
    super.reserved[1]   = 0u;
    super.reserved[2]   = 0u;
    super.reserved[3]   = 0u;
    super.reserved[4]   = 0u;

    /* Erase super sector and write super block */
    if (!app_store_flash_erase_sector_locked(s_super_sector_addr))
    {
        return false;
    }
    if (!app_store_super_write_locked(&super))
    {
        return false;
    }
    *out_super = super;
    return true;
}

static bool app_store_super_get_locked(app_store_super_t *out)
{
    if (app_store_super_read_locked(out))
    {
        return true;
    }
    /* Super block corrupted — try to recover from the alternate sector */
    if ((s_last_err == APP_STORE_ERR_SUPER_MAGIC) || (s_last_err == APP_STORE_ERR_SUPER_CRC))
    {
        /* Try alternate header sector as a fallback for pool_write_ptr.
         * If that also fails, re-initialize. */
        if (out != NULL)
        {
            out->head_sector    = 0u;
            out->wear_count    = 0u;
            out->pool_write_ptr = 0u;
            out->pool_size     = s_data_pool_size;
        }
        return app_store_super_init_locked(out);
    }
    return false;
}

/* ---------- Header table helpers ---------- */

static uint32_t app_store_head_addr(uint8_t sector, uint8_t index)
{
    return s_head_sector_addr[sector & 1u] + (uint32_t)index * APP_STORE_HEADER_SIZE;
}

static bool app_store_head_read_locked(uint8_t sector, uint8_t index, app_store_header_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    (void)memset(out, 0, sizeof(*out));
    const uint32_t addr = app_store_head_addr(sector, index);
    if (!app_store_flash_read_locked(addr, out, (uint32_t)sizeof(*out)))
    {
        return false;
    }
    if (out->magic != APP_STORE_MAGIC)
    {
        return false;
    }
    if (out->version != APP_STORE_VERSION)
    {
        return false;
    }
    const uint32_t crc_len = (uint32_t)(offsetof(app_store_header_t, crc) - offsetof(app_store_header_t, version));
    const uint32_t crc = BSP_CRC_Calculate(&out->version, crc_len, BSP_CRC_WIDTH_32);
    if (crc != out->crc)
    {
        return false;
    }
    return true;
}

static bool app_store_head_write_locked(uint8_t sector, uint8_t index, const app_store_header_t *h)
{
    app_store_header_t s = *h;
    s.magic   = APP_STORE_MAGIC;
    s.version = APP_STORE_VERSION;
    s.crc = BSP_CRC_Calculate(&s.version,
                              (uint32_t)(offsetof(app_store_header_t, crc) - offsetof(app_store_header_t, version)),
                              BSP_CRC_WIDTH_32);

    /* Write payload first, then magic byte last (atomic write) */
    const uint32_t addr = app_store_head_addr(sector, index);
    const uint32_t off_after_magic = 4u;
    if (!app_store_flash_prog_locked(addr + off_after_magic,
                                    ((const uint8_t *)&s) + off_after_magic,
                                    (uint32_t)sizeof(s) - off_after_magic))
    {
        return false;
    }
    if (!app_store_flash_prog_locked(addr, &s.magic, (uint32_t)sizeof(s.magic)))
    {
        return false;
    }
    return true;
}

static bool app_store_head_erase_sector_locked(uint8_t sector)
{
    return app_store_flash_erase_sector_locked(s_head_sector_addr[sector & 1u]);
}

static bool app_store_head_scan_all_locked(uint8_t active_sector, uint64_t task_id,
                                            uint8_t *out_sector, uint8_t *out_index,
                                            bool *out_found)
{
    *out_found = false;
    for (uint8_t sec = 0u; sec < APP_STORE_HEAD_SECTORS; sec++)
    {
        for (uint8_t idx = 0u; idx < APP_STORE_TASK_SLOTS; idx++)
        {
            app_store_header_t h;
            if (app_store_head_read_locked(sec, idx, &h))
            {
                if (h.task_id == task_id)
                {
                    *out_sector  = sec;
                    *out_index   = idx;
                    *out_found   = true;
                    return true;
                }
            }
        }
    }
    (void)active_sector;  /* unused in current scan */
    return true;
}

static bool app_store_head_find_free_locked(uint8_t *out_sector, uint8_t *out_index)
{
    *out_sector = 0xFFu;
    *out_index  = 0xFFu;

    /* Search in the active sector first */
    app_store_super_t super;
    if (!app_store_super_get_locked(&super))
    {
        return false;
    }
    uint8_t active = (super.head_sector <= 1u) ? super.head_sector : 0u;

    for (uint8_t pass = 0u; pass < 2u; pass++)
    {
        const uint8_t sec = (pass == 0u) ? active : ((active == 0u) ? 1u : 0u);
        for (uint8_t idx = 0u; idx < APP_STORE_TASK_SLOTS; idx++)
        {
            app_store_header_t h;
            if (!app_store_head_read_locked(sec, idx, &h))
            {
                /* Unused slot (no valid magic/CRC) */
                *out_sector = sec;
                *out_index  = idx;
                return true;
            }
            if (h.status == APP_STORE_HEAD_FREE || h.status == APP_STORE_HEAD_DELETED)
            {
                *out_sector = sec;
                *out_index  = idx;
                return true;
            }
        }
    }

    /* All slots full */
    return true;
}

static bool app_store_head_update_active_locked(uint8_t sector, uint8_t index,
                                                const app_store_header_t *h)
{
    return app_store_head_write_locked(sector, index, h);
}

static uint8_t app_store_head_count_locked(void)
{
    uint8_t count = 0u;
    for (uint8_t sec = 0u; sec < APP_STORE_HEAD_SECTORS; sec++)
    {
        for (uint8_t idx = 0u; idx < APP_STORE_TASK_SLOTS; idx++)
        {
            app_store_header_t h;
            if (app_store_head_read_locked(sec, idx, &h) && (h.status == APP_STORE_HEAD_ACTIVE))
            {
                count++;
            }
        }
    }
    return count;
}

/* ---------- Data pool helpers ---------- */

static bool app_store_pool_write_ptr_advance_locked(uint32_t new_ptr)
{
    app_store_super_t super;
    if (!app_store_super_get_locked(&super))
    {
        return false;
    }
    if (new_ptr > super.pool_size)
    {
        s_last_err = APP_STORE_ERR_POOL_FULL;
        return false;
    }
    super.pool_write_ptr = new_ptr;
    return app_store_super_write_locked(&super);
}

static bool app_store_ensure_pool_erased_locked(uint32_t addr, uint32_t size)
{
    /* Check if the range is already erased; if not, erase the sector(s) it spans */
    uint8_t buf[32];
    uint32_t off = 0u;
    while (off < size)
    {
        uint32_t chunk = (size - off > (uint32_t)sizeof(buf)) ? (uint32_t)sizeof(buf) : (size - off);
        if (!app_store_flash_read_locked(addr + off, buf, chunk))
        {
            return false;
        }
        bool all_ff = true;
        for (uint32_t k = 0u; k < chunk; k++)
        {
            if (buf[k] != 0xFFu)
            {
                all_ff = false;
                break;
            }
        }
        if (!all_ff)
        {
            /* Data is not 0xFF; need to erase the sector containing this address */
            if (!app_store_flash_erase_sector_locked(addr + off))
            {
                return false;
            }
        }
        off += chunk;
    }
    return true;
}

/* ---------- Core open/create logic ---------- */

static bool app_store_open_locked(uint64_t task_id, bool create,
                                  const app_storage_task_meta_t *meta,
                                  app_storage_phase_t phase)
{
    if (s_active.valid && (s_active.task_id == task_id))
    {
        return true;
    }

    s_last_err   = APP_STORE_ERR_NONE;
    s_last_status = kStatus_Success;

    /* Scan all headers for this task_id */
    uint8_t found_sector = 0xFFu;
    uint8_t found_index  = 0xFFu;
    bool found = false;
    if (!app_store_head_scan_all_locked(0u, task_id, &found_sector, &found_index, &found))
    {
        return false;
    }

    if (found)
    {
        /* Task already exists */
        if (create)
        {
            /* Re-opening existing task for data collection.
             * Do NOT erase data — we may be resuming a task.
             * Just update header status to ACTIVE and reload into s_active. */
            app_store_header_t h;
            if (!app_store_head_read_locked(found_sector, found_index, &h))
            {
                s_last_err = APP_STORE_ERR_OPEN_HEADER_RECHECK;
                return false;
            }
            h.status = APP_STORE_HEAD_ACTIVE;
            /* pretest: reset pre_size so new pretest data overwrites old.
             * formal: keep all sizes so new formal data resumes from where it left off. */
            if (phase == APP_STORAGE_PHASE_PRETEST)
            {
                h.pre_size = 0u;
            }
            if (!app_store_head_update_active_locked(found_sector, found_index, &h))
            {
                return false;
            }
            s_active.valid        = true;
            s_active.head_sector  = found_sector;
            s_active.head_index   = found_index;
            s_active.task_id      = task_id;
            s_active.data_offset  = h.data_offset;
            s_active.data_size    = h.data_size;
            s_active.pre_offset   = h.pre_offset;
            s_active.pre_size     = (phase == APP_STORAGE_PHASE_PRETEST) ? 0u : h.pre_size;
        }
        else
        {
            /* Read-only open */
            app_store_header_t h;
            if (!app_store_head_read_locked(found_sector, found_index, &h))
            {
                s_last_err = APP_STORE_ERR_OPEN_HEADER_RECHECK;
                return false;
            }
            s_active.valid        = true;
            s_active.head_sector  = found_sector;
            s_active.head_index   = found_index;
            s_active.task_id      = task_id;
            s_active.data_offset  = h.data_offset;
            s_active.data_size    = h.data_size;
            s_active.pre_offset   = h.pre_offset;
            s_active.pre_size     = h.pre_size;
        }
        return true;
    }

    /* Task not found */
    if (!create)
    {
        s_last_err = APP_STORE_ERR_OPEN_NO_SLOT;
        return false;
    }

    /* Find a free slot */
    uint8_t free_sector = 0xFFu;
    uint8_t free_index  = 0xFFu;
    if (!app_store_head_find_free_locked(&free_sector, &free_index))
    {
        return false;
    }
    if (free_sector == 0xFFu)
    {
        s_last_err = APP_STORE_ERR_OPEN_NO_SLOT;
        return false;
    }

    /* Get current pool write pointer for data_offset assignment */
    app_store_super_t super;
    if (!app_store_super_get_locked(&super))
    {
        return false;
    }
    uint32_t data_offset = super.pool_write_ptr;

    /* Build new header */
    app_store_header_t h;
    (void)memset(&h, 0, sizeof(h));
    h.task_id       = task_id;
    h.status        = APP_STORE_HEAD_ACTIVE;
    h.data_offset   = data_offset;
    h.data_size     = 0u;
    h.pre_offset    = data_offset;   /* Pre-test and formal data share the pool for now */
    h.pre_size      = 0u;
    if (meta != NULL)
    {
        h.meta = *meta;
    }

    if (!app_store_head_update_active_locked(free_sector, free_index, &h))
    {
        return false;
    }

    /* Note: pool_write_ptr is advanced on first AppendData, not here.
     * This ensures we don't waste pool space for tasks that are created but never collect data. */

    s_active.valid       = true;
    s_active.head_sector = free_sector;
    s_active.head_index  = free_index;
    s_active.task_id     = task_id;
    s_active.data_offset = data_offset;
    s_active.data_size   = 0u;
    s_active.pre_offset  = data_offset;
    s_active.pre_size    = 0u;

    return true;
}

/* ---------- Public API ---------- */

bool APP_Storage_Init(void)
{
    bool ok = false;

    if (!s_crc_inited)
    {
        BSP_CRC_Init();
        s_crc_inited = true;
    }

    if ((s_flash_mutex == NULL) && (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        s_flash_mutex = xSemaphoreCreateMutex();
    }

    app_store_lock();
    ok = app_store_flash_init_locked();
    if (ok)
    {
        app_store_super_t super;
        ok = app_store_super_get_locked(&super);
        if (ok)
        {
            BSP_UART_Print("[STO] Init ok: pool=%lu bytes, head_sec=%u\r\n",
                           (unsigned long)super.pool_size, (unsigned)super.head_sector);
        }
    }
    app_store_unlock();
    return ok;
}

bool APP_Storage_EraseAll(void)
{
    app_store_lock();
    if (!app_store_flash_init_locked())
    {
        app_store_unlock();
        return false;
    }

    /* Erase all sectors */
    (void)app_store_flash_erase_sector_locked(s_super_sector_addr);
    for (uint8_t i = 0u; i < APP_STORE_HEAD_SECTORS; i++)
    {
        (void)app_store_flash_erase_sector_locked(s_head_sector_addr[i]);
    }
    /* Erase data pool sectors in chunks */
    uint32_t pool_addr = s_data_pool_addr;
    while (pool_addr < s_mem_size)
    {
        (void)app_store_flash_erase_sector_locked(pool_addr);
        pool_addr += s_sector_size;
    }

    /* Re-initialize super block */
    (void)app_store_super_init_locked(&(app_store_super_t){0});
    (void)memset(&s_active, 0, sizeof(s_active));

    app_store_unlock();
    BSP_UART_Print("[STO] EraseAll done\r\n");
    return true;
}

bool APP_Storage_BeginTask(uint64_t task_id, const app_storage_task_meta_t *meta)
{
    return APP_Storage_BeginTaskEx(task_id, meta, APP_STORAGE_PHASE_FORMAL);
}

bool APP_Storage_BeginTaskEx(uint64_t task_id, const app_storage_task_meta_t *meta, app_storage_phase_t phase)
{
    app_store_lock();
    if (!app_store_flash_init_locked())
    {
        app_store_unlock();
        const TickType_t now = xTaskGetTickCount();
        if ((now - s_last_begin_fail_log_tick) > pdMS_TO_TICKS(2000U))
        {
            s_last_begin_fail_log_tick = now;
            BSP_UART_Print("[STO] BeginTask fail: task=%08X%08X err=%u\r\n",
                           (unsigned)(task_id >> 32), (unsigned)task_id, (unsigned)s_last_err);
        }
        return false;
    }

    const bool ok = app_store_open_locked(task_id, true, meta, phase);
    app_store_unlock();

    if (ok)
    {
        BSP_UART_Print("[STO] BeginTask ok id=%08X%08X\r\n",
                       (unsigned)(task_id >> 32), (unsigned)task_id);
    }
    else
    {
        const TickType_t now = xTaskGetTickCount();
        if ((now - s_last_begin_fail_log_tick) > pdMS_TO_TICKS(2000U))
        {
            s_last_begin_fail_log_tick = now;
            BSP_UART_Print("[STO] BeginTask fail: task=%08X%08X err=%u\r\n",
                           (unsigned)(task_id >> 32), (unsigned)task_id, (unsigned)s_last_err);
        }
    }
    return ok;
}

bool APP_Storage_AppendData(uint64_t task_id, const void *record, uint32_t record_size)
{
    if ((record == NULL) || (record_size < 4u) || (record_size > 256u))
    {
        return false;
    }

    bool ok = false;
    app_store_lock();

    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL, APP_STORAGE_PHASE_FORMAL))
    {
        app_store_super_t super;
        if (!app_store_super_get_locked(&super))
        {
            app_store_unlock();
            return false;
        }

        /* Current write position in pool = data_offset + data_size */
        const uint32_t write_pos = s_active.data_offset + s_active.data_size;
        const uint32_t after_pos  = write_pos + record_size;

        if (after_pos > super.pool_size)
        {
            s_last_err = APP_STORE_ERR_POOL_FULL;
            app_store_unlock();
            const TickType_t now = xTaskGetTickCount();
            if ((now - s_last_append_fail_log_tick) > pdMS_TO_TICKS(2000U))
            {
                s_last_append_fail_log_tick = now;
                BSP_UART_Print("[STO] AppendData fail: pool full\r\n");
            }
            return false;
        }

        /* Ensure sectors are erased for this write range */
        if (!app_store_ensure_pool_erased_locked(s_data_pool_addr + write_pos, record_size))
        {
            app_store_unlock();
            return false;
        }

        /* Write record: head 16B, then tail 4B (cross-page safety) */
        const uint32_t pool_addr = s_data_pool_addr + write_pos;
        ok = app_store_flash_prog_locked(pool_addr, record, 16u);
        if (ok && record_size > 16u)
        {
            OSA_TimeDelay(2u);
            ok = app_store_flash_prog_locked(pool_addr + 16u, ((const uint8_t *)record) + 16u, record_size - 16u);
        }

        if (ok)
        {
            s_active.data_size += record_size;

            /* Advance global pool write pointer if this is the furthest we've written */
            if (after_pos > super.pool_write_ptr)
            {
                super.pool_write_ptr = after_pos;
                (void)app_store_super_write_locked(&super);
            }

            /* Update header with new data_size (async-friendly: best-effort) */
            app_store_header_t h;
            if (app_store_head_read_locked(s_active.head_sector, s_active.head_index, &h))
            {
                h.data_size = s_active.data_size;
                (void)app_store_head_update_active_locked(s_active.head_sector, s_active.head_index, &h);
            }
        }
    }

    app_store_unlock();

    if (!ok)
    {
        const TickType_t now = xTaskGetTickCount();
        if ((now - s_last_append_fail_log_tick) > pdMS_TO_TICKS(2000U))
        {
            s_last_append_fail_log_tick = now;
            BSP_UART_Print("[STO] AppendData fail: task=%08X%08X err=%u\r\n",
                           (unsigned)(task_id >> 32), (unsigned)task_id, (unsigned)s_last_err);
        }
    }
    return ok;
}

bool APP_Storage_AppendPreData(uint64_t task_id, const void *record, uint32_t record_size)
{
    if ((record == NULL) || (record_size < 4u) || (record_size > 256u))
    {
        return false;
    }

    bool ok = false;
    app_store_lock();

    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL, APP_STORAGE_PHASE_FORMAL))
    {
        app_store_super_t super;
        if (!app_store_super_get_locked(&super))
        {
            app_store_unlock();
            return false;
        }

        const uint32_t write_pos = s_active.pre_offset + s_active.pre_size;
        const uint32_t after_pos  = write_pos + record_size;

        if (after_pos > super.pool_size)
        {
            s_last_err = APP_STORE_ERR_POOL_FULL;
            app_store_unlock();
            return false;
        }

        if (!app_store_ensure_pool_erased_locked(s_data_pool_addr + write_pos, record_size))
        {
            app_store_unlock();
            return false;
        }

        const uint32_t pool_addr = s_data_pool_addr + write_pos;
        ok = app_store_flash_prog_locked(pool_addr, record, 16u);
        if (ok && record_size > 16u)
        {
            OSA_TimeDelay(2u);
            ok = app_store_flash_prog_locked(pool_addr + 16u, ((const uint8_t *)record) + 16u, record_size - 16u);
        }

        if (ok)
        {
            s_active.pre_size += record_size;

            if (after_pos > super.pool_write_ptr)
            {
                super.pool_write_ptr = after_pos;
                (void)app_store_super_write_locked(&super);
            }

            app_store_header_t h;
            if (app_store_head_read_locked(s_active.head_sector, s_active.head_index, &h))
            {
                h.pre_size = s_active.pre_size;
                (void)app_store_head_update_active_locked(s_active.head_sector, s_active.head_index, &h);
            }
        }
    }

    app_store_unlock();
    return ok;
}

bool APP_Storage_AppendLog(uint64_t task_id, const void *data, uint32_t size)
{
    (void)task_id;
    (void)data;
    (void)size;
    return false;
}

bool APP_Storage_LogPrintf(uint64_t task_id, const char *format, ...)
{
    (void)task_id;
    (void)format;
    return false;
}

int APP_Storage_ReadLog(uint64_t task_id, uint32_t offset, void *out, uint32_t size)
{
    (void)task_id;
    (void)offset;
    (void)out;
    (void)size;
    return -1;
}

bool APP_Storage_GetLogSize(uint64_t task_id, uint32_t *out_size)
{
    (void)task_id;
    if (out_size != NULL)
    {
        *out_size = 0u;
    }
    return true;
}

int APP_Storage_ReadData(uint64_t task_id, uint32_t offset, void *out, uint32_t size)
{
    if ((out == NULL) || (size == 0u))
    {
        return -1;
    }

    int ret = -1;
    app_store_lock();

    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL, APP_STORAGE_PHASE_FORMAL))
    {
        if (offset < s_active.data_size)
        {
            uint32_t to_read = size;
            if (offset + to_read > s_active.data_size)
            {
                to_read = s_active.data_size - offset;
            }
            const uint32_t addr = s_data_pool_addr + s_active.data_offset + offset;
            if (app_store_flash_read_locked(addr, out, to_read))
            {
                ret = (int)to_read;
            }
        }
        else
        {
            ret = 0;
        }
    }

    app_store_unlock();
    return ret;
}

int APP_Storage_ReadPreData(uint64_t task_id, uint32_t offset, void *out, uint32_t size)
{
    if ((out == NULL) || (size == 0u))
    {
        return -1;
    }

    int ret = -1;
    app_store_lock();

    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL, APP_STORAGE_PHASE_FORMAL))
    {
        if (offset < s_active.pre_size)
        {
            uint32_t to_read = size;
            if (offset + to_read > s_active.pre_size)
            {
                to_read = s_active.pre_size - offset;
            }
            const uint32_t addr = s_data_pool_addr + s_active.pre_offset + offset;
            if (app_store_flash_read_locked(addr, out, to_read))
            {
                ret = (int)to_read;
            }
        }
        else
        {
            ret = 0;
        }
    }

    app_store_unlock();
    return ret;
}

int APP_Storage_ReadMeta(uint64_t task_id, uint32_t offset, void *out, uint32_t size)
{
    if ((out == NULL) || (size == 0u))
    {
        return -1;
    }

    int ret = -1;
    app_store_lock();

    if (app_store_flash_init_locked())
    {
        uint8_t found_sector = 0xFFu;
        uint8_t found_index  = 0xFFu;
        bool found = false;
        (void)app_store_head_scan_all_locked(0u, task_id, &found_sector, &found_index, &found);

        if (found)
        {
            app_store_header_t h;
            if (app_store_head_read_locked(found_sector, found_index, &h))
            {
                const uint32_t meta_off = (uint32_t)offsetof(app_store_header_t, meta);
                const uint32_t meta_sz  = (uint32_t)sizeof(h.meta);
                if (offset < meta_sz)
                {
                    uint32_t to_read = size;
                    if (offset + to_read > meta_sz)
                    {
                        to_read = meta_sz - offset;
                    }
                    const uint32_t addr = app_store_head_addr(found_sector, found_index) + meta_off + offset;
                    if (app_store_flash_read_locked(addr, out, to_read))
                    {
                        ret = (int)to_read;
                    }
                }
                else
                {
                    ret = 0;
                }
            }
        }
        else
        {
            ret = 0;
        }
    }

    app_store_unlock();
    return ret;
}

bool APP_Storage_GetDataSize(uint64_t task_id, uint32_t *out_size)
{
    if (out_size == NULL)
    {
        return false;
    }
    bool ok = false;
    app_store_lock();

    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL, APP_STORAGE_PHASE_FORMAL))
    {
        *out_size = s_active.data_size;
        ok = true;
    }
    else
    {
        *out_size = 0u;
        ok = true;  /* Not found is not an error for size query */
    }

    app_store_unlock();
    return ok;
}

bool APP_Storage_GetPreDataSize(uint64_t task_id, uint32_t *out_size)
{
    if (out_size == NULL)
    {
        return false;
    }
    bool ok = false;
    app_store_lock();

    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL, APP_STORAGE_PHASE_FORMAL))
    {
        *out_size = s_active.pre_size;
        ok = true;
    }
    else
    {
        *out_size = 0u;
        ok = true;
    }

    app_store_unlock();
    return ok;
}

bool APP_Storage_GetMetaSize(uint64_t task_id, uint32_t *out_size)
{
    if (out_size == NULL)
    {
        return false;
    }

    bool ok = false;
    app_store_lock();

    if (app_store_flash_init_locked())
    {
        uint8_t found_sector = 0xFFu;
        uint8_t found_index  = 0xFFu;
        bool found = false;
        (void)app_store_head_scan_all_locked(0u, task_id, &found_sector, &found_index, &found);
        *out_size = found ? (uint32_t)sizeof(app_storage_task_meta_t) : 0u;
        ok = true;
    }

    app_store_unlock();
    return ok;
}

bool APP_Storage_DeleteTask(uint64_t task_id)
{
    bool ok = false;
    app_store_lock();

    if (app_store_flash_init_locked())
    {
        uint8_t found_sector = 0xFFu;
        uint8_t found_index  = 0xFFu;
        bool found = false;
        if (app_store_head_scan_all_locked(0u, task_id, &found_sector, &found_index, &found) && found)
        {
            app_store_header_t h;
            if (app_store_head_read_locked(found_sector, found_index, &h))
            {
                h.status = APP_STORE_HEAD_DELETED;
                ok = app_store_head_update_active_locked(found_sector, found_index, &h);
            }
        }

        if (s_active.valid && (s_active.task_id == task_id))
        {
            (void)memset(&s_active, 0, sizeof(s_active));
        }
    }

    app_store_unlock();
    return ok;
}

uint8_t APP_Storage_ListTasks(uint64_t *out_ids, uint8_t max_count)
{
    if ((out_ids == NULL) || (max_count == 0U))
    {
        return 0U;
    }

    uint8_t count = 0U;

    app_store_lock();
    if (app_store_flash_init_locked())
    {
        for (uint8_t sec = 0u; sec < APP_STORE_HEAD_SECTORS; sec++)
        {
            for (uint8_t idx = 0u; idx < APP_STORE_TASK_SLOTS; idx++)
            {
                app_store_header_t h;
                if (app_store_head_read_locked(sec, idx, &h) && (h.status == APP_STORE_HEAD_ACTIVE))
                {
                    if (count < max_count)
                    {
                        out_ids[count] = h.task_id;
                        count++;
                    }
                }
            }
        }
    }
    app_store_unlock();

    return count;
}
