/*
 * app_storage.c
 *
 *  Created on: 2026年5月21日
 *      Author: elois
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

#define APP_STORE_MAGIC          0x474F4C53u
#define APP_STORE_VERSION        1u
#define APP_STORE_SLOT_COUNT     4u
#if defined(DEBUG)
#define APP_STORE_WRITE_VERIFY_ENABLE 1u
#else
#define APP_STORE_WRITE_VERIFY_ENABLE 0u
#endif

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint64_t task_id;
    app_storage_task_meta_t meta;
    uint32_t reserved[6];
    uint32_t header_crc;
} app_store_header_t;

typedef struct
{
    bool valid;
    uint8_t slot;
    uint64_t task_id;
    uint32_t pre_size;
    uint32_t data_size;
    uint32_t pre_next_erase;
    uint32_t data_next_erase;
} app_store_task_state_t;

static SemaphoreHandle_t s_flash_mutex;
static bool s_flash_inited;
static bool s_crc_inited;
static nor_config_t s_norCfg;
static nor_handle_t s_norHandle;
static lpspi_memory_config_t s_memCfg = {
    .bytesInPageSize   = 256u,
    .bytesInSectorSize = 4096u,
    .bytesInMemorySize = (8u * 1024u * 1024u),
};

static bool s_layout_ready;
static uint32_t s_sector_size;
static uint32_t s_mem_size;
static uint32_t s_usable_size;
static uint32_t s_slot_size;
static uint32_t s_pre_capacity;

static app_store_task_state_t s_active;

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
} app_store_err_t;

static app_store_err_t s_last_err;
static status_t s_last_status;
static uint32_t s_last_addr;
static uint32_t s_last_size;
static TickType_t s_last_begin_fail_log_tick;
static TickType_t s_last_append_fail_log_tick;
static uint32_t s_lp_disallow_count;

static void app_store_set_err(app_store_err_t err, status_t st, uint32_t addr, uint32_t size)
{
    s_last_err = err;
    s_last_status = st;
    s_last_addr = addr;
    s_last_size = size;
}

static void app_store_flash_recover_locked(void)
{
    BOARD_InitExtFlashPins();
    CLOCK_EnableClock(kCLOCK_Lpspi1);
    CLOCK_SetIpSrc(kCLOCK_Lpspi1, kCLOCK_IpSrcFro192M);
    CLOCK_SetIpSrcDiv(kCLOCK_Lpspi1, kSCG_SysClkDivBy1);
    CLOCK_EnableClockLPMode(kCLOCK_Lpspi1, kCLOCK_IpClkControl_fun1);

    s_norCfg.memControlConfig = &s_memCfg;
    s_norCfg.driverBaseAddr   = NULL;
    (void)Nor_Flash_Init(&s_norCfg, &s_norHandle);
}

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

static int app_store_wait_ready(uint32_t timeout_ms)
{
    for (uint32_t waited = 0u; waited < timeout_ms; waited++)
    {
        bool busy = false;
        const status_t st = Nor_Flash_Is_Busy(&s_norHandle, &busy);
        if (st != kStatus_Success)
        {
            app_store_set_err(APP_STORE_ERR_BUSY_QUERY, st, 0u, 0u);
            return -1;
        }
        if (!busy)
        {
            return 0;
        }
        OSA_TimeDelay(1u);
    }
    app_store_set_err(APP_STORE_ERR_BUSY_TIMEOUT, kStatus_Fail, 0u, timeout_ms);
    return -1;
}

static bool app_store_flash_init_locked(void)
{
    if (s_flash_inited)
    {
        return true;
    }

    s_layout_ready = false;
    app_store_set_err(APP_STORE_ERR_NONE, kStatus_Success, 0u, 0u);

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
        app_store_set_err(APP_STORE_ERR_INIT, st, 0u, 0u);
        return false;
    }

    if (s_norHandle.driverBaseAddr == NULL)
    {
        app_store_set_err(APP_STORE_ERR_INIT, kStatus_Fail, 0u, 0u);
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
    if ((s_sector_size == 0u) || (s_mem_size < s_sector_size))
    {
        app_store_set_err(APP_STORE_ERR_LAYOUT, kStatus_Fail, 0u, 0u);
        return false;
    }

    s_usable_size = s_mem_size - s_sector_size;
    s_slot_size   = (s_usable_size / APP_STORE_SLOT_COUNT);
    s_slot_size   = (s_slot_size / s_sector_size) * s_sector_size;
    s_pre_capacity = 4u * s_sector_size;
    if (s_pre_capacity > (s_slot_size / 8u))
    {
        s_pre_capacity = (s_slot_size / 8u);
        s_pre_capacity = (s_pre_capacity / s_sector_size) * s_sector_size;
    }

    s_layout_ready = (s_slot_size >= (2u * s_sector_size + s_pre_capacity));
    if (!s_layout_ready)
    {
        app_store_set_err(APP_STORE_ERR_LAYOUT, kStatus_Fail, 0u, 0u);
    }
    s_flash_inited = s_layout_ready;
    return s_layout_ready;
}

static uint32_t app_store_slot_base(uint8_t slot)
{
    return (uint32_t)slot * s_slot_size;
}

static uint32_t app_store_pre_base(uint8_t slot)
{
    return app_store_slot_base(slot) + s_sector_size;
}

static uint32_t app_store_data_base(uint8_t slot)
{
    return app_store_pre_base(slot) + s_pre_capacity;
}

static uint32_t app_store_pre_capacity(void)
{
    return s_pre_capacity;
}

static uint32_t app_store_data_capacity(void)
{
    const uint32_t used = s_sector_size + s_pre_capacity;
    if (s_slot_size <= used)
    {
        return 0u;
    }
    return s_slot_size - used;
}

static bool app_store_flash_read_locked(uint32_t addr, void *out, uint32_t size)
{
    if ((out == NULL) || (size == 0u))
    {
        return false;
    }
    if ((s_norHandle.driverBaseAddr == NULL) || (addr + size > s_usable_size))
    {
        app_store_set_err(APP_STORE_ERR_RANGE, kStatus_Fail, addr, size);
        return false;
    }
    const status_t st = Nor_Flash_Read(&s_norHandle, addr, (uint8_t *)out, size);
    if (st != kStatus_Success)
    {
        app_store_set_err(APP_STORE_ERR_READ, st, addr, size);
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
    if ((s_norHandle.driverBaseAddr == NULL) || (addr + size > s_usable_size))
    {
        app_store_set_err(APP_STORE_ERR_RANGE, kStatus_Fail, addr, size);
        return false;
    }
    for (uint32_t attempt = 0u; attempt < 2u; attempt++)
    {
        if (app_store_wait_ready(2000u) != 0)
        {
            return false;
        }

        const status_t st = Nor_Flash_Program(&s_norHandle, addr, (uint8_t *)(uintptr_t)data, size);
        if (st == kStatus_Success)
        {
            if (app_store_wait_ready(2000u) != 0)
            {
                return false;
            }
            goto verify;
        }

        app_store_set_err(APP_STORE_ERR_PROG, st, addr, size);
        app_store_flash_recover_locked();
        OSA_TimeDelay(1u);
    }
    return false;

verify:

#if APP_STORE_WRITE_VERIFY_ENABLE
    uint8_t buf[64];
    const uint8_t *src = (const uint8_t *)data;
    uint32_t off = 0u;
    while (off < size)
    {
        uint32_t chunk = size - off;
        if (chunk > (uint32_t)sizeof(buf))
        {
            chunk = (uint32_t)sizeof(buf);
        }
        if (!app_store_flash_read_locked(addr + off, buf, chunk))
        {
            return false;
        }
        if (memcmp(buf, src + off, chunk) != 0)
        {
            app_store_set_err(APP_STORE_ERR_PROG, kStatus_Fail, addr + off, chunk);
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
        app_store_set_err(APP_STORE_ERR_INIT, kStatus_Fail, addr, 0u);
        return false;
    }
    const uint32_t base = (addr / s_sector_size) * s_sector_size;
    if (base >= s_usable_size)
    {
        app_store_set_err(APP_STORE_ERR_RANGE, kStatus_Fail, base, s_sector_size);
        return false;
    }
    const status_t st = Nor_Flash_Erase_Sector(&s_norHandle, base);
    if (st != kStatus_Success)
    {
        app_store_set_err(APP_STORE_ERR_ERASE, st, base, s_sector_size);
        return false;
    }
    return (app_store_wait_ready(5000u) == 0);
}

static bool app_store_header_read_locked(uint8_t slot, app_store_header_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    (void)memset(out, 0, sizeof(*out));
    if (!app_store_flash_read_locked(app_store_slot_base(slot), out, (uint32_t)sizeof(*out)))
    {
        return false;
    }
    if (out->magic != APP_STORE_MAGIC)
    {
        app_store_set_err(APP_STORE_ERR_HEADER_MAGIC, kStatus_Fail, app_store_slot_base(slot), (uint32_t)sizeof(*out));
        return false;
    }
    if (out->version != APP_STORE_VERSION)
    {
        app_store_set_err(APP_STORE_ERR_HEADER_VERSION, kStatus_Fail, app_store_slot_base(slot), (uint32_t)sizeof(*out));
        return false;
    }

    const uint32_t crc_len = (uint32_t)(offsetof(app_store_header_t, header_crc) - offsetof(app_store_header_t, version));
    const uint32_t crc = BSP_CRC_Calculate(&out->version, crc_len, BSP_CRC_WIDTH_32);
    if (crc != out->header_crc)
    {
        app_store_set_err(APP_STORE_ERR_HEADER_CRC, kStatus_Fail, app_store_slot_base(slot), (uint32_t)sizeof(*out));
        return false;
    }
    return true;
}

static bool app_store_header_write_locked(uint8_t slot, uint64_t task_id, const app_storage_task_meta_t *meta)
{
    const uint32_t base = app_store_slot_base(slot);
    if (!app_store_flash_erase_sector_locked(base))
    {
        return false;
    }

    app_store_header_t h;
    (void)memset(&h, 0xFF, sizeof(h));
    h.magic   = APP_STORE_MAGIC;
    h.version = APP_STORE_VERSION;
    h.task_id = task_id;
    if (meta != NULL)
    {
        h.meta = *meta;
    }
    const uint32_t crc_len = (uint32_t)(offsetof(app_store_header_t, header_crc) - offsetof(app_store_header_t, version));
    h.header_crc = BSP_CRC_Calculate(&h.version, crc_len, BSP_CRC_WIDTH_32);

    const uint32_t off_after_magic = 4u;
    if (!app_store_flash_prog_locked(base + off_after_magic, ((const uint8_t *)&h) + off_after_magic, (uint32_t)sizeof(h) - off_after_magic))
    {
        return false;
    }

    if (!app_store_flash_prog_locked(base, &h.magic, (uint32_t)sizeof(h.magic)))
    {
        return false;
    }
    return true;
}

static uint32_t app_store_scan_committed_size_locked(uint32_t base, uint32_t capacity, uint32_t record_size)
{
    if ((record_size == 0u) || (capacity < record_size))
    {
        return 0u;
    }

    uint32_t pos = 0u;
    uint8_t buf[256];
    while (pos < capacity)
    {
        uint32_t chunk = capacity - pos;
        if (chunk > (uint32_t)sizeof(buf))
        {
            chunk = (uint32_t)sizeof(buf);
        }
        if (!app_store_flash_read_locked(base + pos, buf, chunk))
        {
            break;
        }

        uint32_t next = (pos / record_size) * record_size;
        if (next < pos)
        {
            next += record_size;
        }
        while ((next + 4u) <= (pos + chunk))
        {
            const uint32_t i = next - pos;
            bool erased = true;
            for (uint32_t k = 0u; k < 4u; k++)
            {
                if (buf[i + k] != 0xFFu)
                {
                    erased = false;
                    break;
                }
            }
            if (erased)
            {
                return next;
            }
            next += record_size;
        }

        pos += chunk;
    }

    return (capacity / record_size) * record_size;
}

static bool app_store_ensure_erased_range_locked(uint32_t *next_erase, uint32_t addr, uint32_t size)
{
    if ((next_erase == NULL) || (s_sector_size == 0u) || (size == 0u))
    {
        return false;
    }
    const uint32_t end = addr + size - 1u;
    while (*next_erase <= end)
    {
        if (!app_store_flash_erase_sector_locked(*next_erase))
        {
            return false;
        }
        *next_erase += s_sector_size;
    }
    return true;
}

static bool app_store_open_locked(uint64_t task_id, bool create, const app_storage_task_meta_t *meta)
{
    if (s_active.valid && (s_active.task_id == task_id))
    {
        return true;
    }

    app_store_set_err(APP_STORE_ERR_NONE, kStatus_Success, 0u, 0u);

    uint8_t found_slot = 0xFFu;
    app_store_header_t h;
    for (uint8_t slot = 0u; slot < (uint8_t)APP_STORE_SLOT_COUNT; slot++)
    {
        if (app_store_header_read_locked(slot, &h) && (h.task_id == task_id))
        {
            found_slot = slot;
            break;
        }
    }

    if ((found_slot != 0xFFu) && create)
    {
        if (!app_store_header_write_locked(found_slot, task_id, meta))
        {
            return false;
        }
        (void)app_store_flash_erase_sector_locked(app_store_pre_base(found_slot));
        (void)app_store_flash_erase_sector_locked(app_store_data_base(found_slot));
        if (s_active.valid && (s_active.task_id == task_id))
        {
            (void)memset(&s_active, 0, sizeof(s_active));
        }
    }
    else if ((found_slot == 0xFFu) && create)
    {
        for (uint8_t slot = 0u; slot < (uint8_t)APP_STORE_SLOT_COUNT; slot++)
        {
            if (!app_store_header_read_locked(slot, &h))
            {
                found_slot = slot;
                break;
            }
        }
        if (found_slot == 0xFFu)
        {
            found_slot = 0u;
        }

        if (!app_store_header_write_locked(found_slot, task_id, meta))
        {
            return false;
        }

        (void)app_store_flash_erase_sector_locked(app_store_pre_base(found_slot));
        (void)app_store_flash_erase_sector_locked(app_store_data_base(found_slot));
    }

    if (found_slot == 0xFFu)
    {
        app_store_set_err(APP_STORE_ERR_OPEN_NO_SLOT, kStatus_Fail, 0u, 0u);
        return false;
    }

    if (!app_store_header_read_locked(found_slot, &h))
    {
        if (s_last_err == APP_STORE_ERR_NONE)
        {
            app_store_set_err(APP_STORE_ERR_OPEN_HEADER_RECHECK, kStatus_Fail, app_store_slot_base(found_slot), (uint32_t)sizeof(h));
        }
        return false;
    }

    uint32_t record_size = h.meta.record_size;
    if ((record_size < 4u) || (record_size > 256u))
    {
        record_size = 18u;
    }

    const uint32_t pre_base = app_store_pre_base(found_slot);
    const uint32_t data_base = app_store_data_base(found_slot);
    const uint32_t pre_cap = app_store_pre_capacity();
    const uint32_t data_cap = app_store_data_capacity();

    s_active.valid = true;
    s_active.slot = found_slot;
    s_active.task_id = task_id;
    s_active.pre_size = app_store_scan_committed_size_locked(pre_base, pre_cap, record_size);
    s_active.data_size = app_store_scan_committed_size_locked(data_base, data_cap, record_size);

    s_active.pre_next_erase = pre_base + ((s_active.pre_size + s_sector_size - 1u) / s_sector_size) * s_sector_size;
    s_active.data_next_erase = data_base + ((s_active.data_size + s_sector_size - 1u) / s_sector_size) * s_sector_size;
    return true;
}

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

    for (uint8_t slot = 0u; slot < (uint8_t)APP_STORE_SLOT_COUNT; slot++)
    {
        const uint32_t base = app_store_slot_base(slot);
        (void)app_store_flash_erase_sector_locked(base);
        (void)app_store_flash_erase_sector_locked(app_store_pre_base(slot));
        (void)app_store_flash_erase_sector_locked(app_store_data_base(slot));
    }
    (void)memset(&s_active, 0, sizeof(s_active));
    app_store_unlock();
    return true;
}

bool APP_Storage_BeginTask(uint64_t task_id, const app_storage_task_meta_t *meta)
{
    return APP_Storage_BeginTaskEx(task_id, meta, APP_STORAGE_PHASE_FORMAL);
}

bool APP_Storage_BeginTaskEx(uint64_t task_id, const app_storage_task_meta_t *meta, app_storage_phase_t phase)
{
    (void)phase;

    app_store_lock();
    if (!app_store_flash_init_locked())
    {
        app_store_unlock();
        const TickType_t now = xTaskGetTickCount();
        if ((now - s_last_begin_fail_log_tick) > pdMS_TO_TICKS(2000U))
        {
            s_last_begin_fail_log_tick = now;
            const uint32_t hi = (uint32_t)(task_id >> 32);
            const uint32_t lo = (uint32_t)(task_id);
            BSP_UART_Print("[STO] BeginTask fail: task=%08X%08X err=%u st=%d addr=0x%08X size=%u inited=%u layout=%u sector=%u mem=%u usable=%u slot=%u pre=%u\r\n",
                           hi, lo, (unsigned)s_last_err, (int)s_last_status, (unsigned)s_last_addr, (unsigned)s_last_size,
                           (unsigned)s_flash_inited, (unsigned)s_layout_ready, (unsigned)s_sector_size, (unsigned)s_mem_size,
                           (unsigned)s_usable_size, (unsigned)s_slot_size, (unsigned)s_pre_capacity);
        }
        return false;
    }
    const bool ok = app_store_open_locked(task_id, true, meta);
    app_store_unlock();
    if (ok)
    {
        BSP_UART_Print("[STO] BeginTask ok\r\n");
    }
    else
    {
        const TickType_t now = xTaskGetTickCount();
        if ((now - s_last_begin_fail_log_tick) > pdMS_TO_TICKS(2000U))
        {
            s_last_begin_fail_log_tick = now;
            const uint32_t hi = (uint32_t)(task_id >> 32);
            const uint32_t lo = (uint32_t)(task_id);
            BSP_UART_Print("[STO] BeginTask fail: task=%08X%08X err=%u st=%d addr=0x%08X size=%u inited=%u layout=%u sector=%u mem=%u usable=%u slot=%u pre=%u\r\n",
                           hi, lo, (unsigned)s_last_err, (int)s_last_status, (unsigned)s_last_addr, (unsigned)s_last_size,
                           (unsigned)s_flash_inited, (unsigned)s_layout_ready, (unsigned)s_sector_size, (unsigned)s_mem_size,
                           (unsigned)s_usable_size, (unsigned)s_slot_size, (unsigned)s_pre_capacity);
        }
    }
    return ok;
}

bool APP_Storage_AppendData(uint64_t task_id, const void *record, uint32_t record_size)
{
    if ((record == NULL) || (record_size == 0u) || (record_size < 4u))
    {
        return false;
    }

    bool ok = false;
    uint32_t addr = 0u;
    uint32_t cap = 0u;
    uint32_t before_size = 0u;
    uint32_t next_erase = 0u;
    app_store_lock();
    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL))
    {
        const uint32_t base = app_store_data_base(s_active.slot);
        cap = app_store_data_capacity();
        before_size = s_active.data_size;
        next_erase = s_active.data_next_erase;
        if (s_active.data_size + record_size <= cap)
        {
            addr = base + s_active.data_size;
            if (app_store_ensure_erased_range_locked(&s_active.data_next_erase, addr, record_size))
            {
                ok = app_store_flash_prog_locked(addr + 4u, ((const uint8_t *)record) + 4u, record_size - 4u) &&
                     app_store_flash_prog_locked(addr, record, 4u);
                if (ok)
                {
                    s_active.data_size += record_size;
                }
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
            const uint32_t hi = (uint32_t)(task_id >> 32);
            const uint32_t lo = (uint32_t)(task_id);
            BSP_UART_Print("[STO] AppendData fail: task=%08X%08X err=%u st=%d addr=0x%08X size=%u data=%u cap=%u next=0x%08X\r\n",
                           hi, lo, (unsigned)s_last_err, (int)s_last_status, (unsigned)s_last_addr, (unsigned)s_last_size,
                           (unsigned)before_size, (unsigned)cap, (unsigned)next_erase);
        }
    }
    return ok;
}

bool APP_Storage_AppendPreData(uint64_t task_id, const void *record, uint32_t record_size)
{
    if ((record == NULL) || (record_size == 0u) || (record_size < 4u))
    {
        return false;
    }

    bool ok = false;
    uint32_t addr = 0u;
    uint32_t cap = 0u;
    uint32_t before_size = 0u;
    uint32_t next_erase = 0u;
    app_store_lock();
    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL))
    {
        const uint32_t base = app_store_pre_base(s_active.slot);
        cap = app_store_pre_capacity();
        before_size = s_active.pre_size;
        next_erase = s_active.pre_next_erase;
        if (s_active.pre_size + record_size <= cap)
        {
            addr = base + s_active.pre_size;
            if (app_store_ensure_erased_range_locked(&s_active.pre_next_erase, addr, record_size))
            {
                ok = app_store_flash_prog_locked(addr + 4u, ((const uint8_t *)record) + 4u, record_size - 4u) &&
                     app_store_flash_prog_locked(addr, record, 4u);
                if (ok)
                {
                    s_active.pre_size += record_size;
                }
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
            const uint32_t hi = (uint32_t)(task_id >> 32);
            const uint32_t lo = (uint32_t)(task_id);
            BSP_UART_Print("[STO] AppendPreData fail: task=%08X%08X err=%u st=%d addr=0x%08X size=%u pre=%u cap=%u next=0x%08X\r\n",
                           hi, lo, (unsigned)s_last_err, (int)s_last_status, (unsigned)s_last_addr, (unsigned)s_last_size,
                           (unsigned)before_size, (unsigned)cap, (unsigned)next_erase);
        }
    }
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
    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL))
    {
        if (offset < s_active.data_size)
        {
            uint32_t to_read = size;
            if (offset + to_read > s_active.data_size)
            {
                to_read = s_active.data_size - offset;
            }
            const uint32_t base = app_store_data_base(s_active.slot);
            if (app_store_flash_read_locked(base + offset, out, to_read))
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
    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL))
    {
        if (offset < s_active.pre_size)
        {
            uint32_t to_read = size;
            if (offset + to_read > s_active.pre_size)
            {
                to_read = s_active.pre_size - offset;
            }
            const uint32_t base = app_store_pre_base(s_active.slot);
            if (app_store_flash_read_locked(base + offset, out, to_read))
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
        app_store_header_t h;
        bool found = false;
        uint8_t found_slot = 0u;
        for (uint8_t slot = 0u; slot < (uint8_t)APP_STORE_SLOT_COUNT; slot++)
        {
            if (app_store_header_read_locked(slot, &h) && (h.task_id == task_id))
            {
                found = true;
                found_slot = slot;
                break;
            }
        }
        if (found)
        {
            const uint32_t meta_off = (uint32_t)offsetof(app_store_header_t, meta);
            const uint32_t meta_sz = (uint32_t)sizeof(app_storage_task_meta_t);
            if (offset < meta_sz)
            {
                uint32_t to_read = size;
                if (offset + to_read > meta_sz)
                {
                    to_read = meta_sz - offset;
                }
                const uint32_t base = app_store_slot_base(found_slot);
                if (app_store_flash_read_locked(base + meta_off + offset, out, to_read))
                {
                    ret = (int)to_read;
                }
            }
            else
            {
                ret = 0;
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
    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL))
    {
        *out_size = s_active.data_size;
        ok = true;
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
    if (app_store_flash_init_locked() && app_store_open_locked(task_id, false, NULL))
    {
        *out_size = s_active.pre_size;
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
        app_store_header_t h;
        bool found = false;
        for (uint8_t slot = 0u; slot < (uint8_t)APP_STORE_SLOT_COUNT; slot++)
        {
            if (app_store_header_read_locked(slot, &h) && (h.task_id == task_id))
            {
                found = true;
                break;
            }
        }
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
        app_store_header_t h;
        for (uint8_t slot = 0u; slot < (uint8_t)APP_STORE_SLOT_COUNT; slot++)
        {
            if (app_store_header_read_locked(slot, &h) && (h.task_id == task_id))
            {
                const uint32_t base = app_store_slot_base(slot);
                (void)app_store_flash_erase_sector_locked(base);
                (void)app_store_flash_erase_sector_locked(app_store_pre_base(slot));
                (void)app_store_flash_erase_sector_locked(app_store_data_base(slot));
                if (s_active.valid && (s_active.task_id == task_id))
                {
                    (void)memset(&s_active, 0, sizeof(s_active));
                }
                ok = true;
                break;
            }
        }
    }
    app_store_unlock();
    return ok;
}
