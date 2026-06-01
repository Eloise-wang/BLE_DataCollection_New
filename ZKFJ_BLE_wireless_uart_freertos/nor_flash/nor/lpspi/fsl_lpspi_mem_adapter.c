/*
 * Copyright 2022 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "board.h"
#include "fsl_common.h"
#include "fsl_device_registers.h"
#include "fsl_lpspi_mem_adapter.h"
#if (FSL_FEATURE_SOC_LPSPI_COUNT > 0)
#include "fsl_lpspi.h"
#endif

#include "bsp_uart.h"
#include "fsl_os_abstraction.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
enum
{
    kFlashCmd_ReadId          = 0x9F,
    kFlashCmd_ReadStatus      = 0x05,
    kFlashCmd_ReadMemory24Bit = 0x03,
    kFlashCmd_FastRead        = 0x0B,

    kFlashCmd_WriteEnable  = 0x06,
    kFlashCmd_WriteDisable = 0x04,
    kFlashCmd_PageProgram  = 0x02,

    kFlashCmd_ErasePage = 0x81,
    kFlashCmd_Erase4K   = 0x20,
    kFlashCmd_Erase32K  = 0x52,
    kFlashCmd_Erase64K  = 0xD8,
    kFlashCmd_EraseAll  = 0x60,
};

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
extern void BOARD_LpspiPcsPinControl(bool isSelected);
extern void BOARD_LpspiIomuxConfig(spi_pin_mode_t pinMode);

static status_t LPSPI_MemWaitBusy(LPSPI_Type *base);
static status_t LPSPI_MemWaitModuleIdle(LPSPI_Type *base);
static void LPSPI_MemDbgLog(const char *stage, status_t st, uint8_t cmd0, uint32_t addr, uint32_t len, LPSPI_Type *base);
static status_t LPSPI_MemReadStatus(uint8_t *outStatus, LPSPI_Type *base);

typedef struct
{
    LPSPI_Type *base;
    uint32_t lastMs;
    uint32_t suppressed;
    bool everPrinted;
} lpspi_dbg_state_t;

#ifndef LPSPI_DBG_STATE_COUNT
#if defined(FSL_FEATURE_SOC_LPSPI_COUNT) && (FSL_FEATURE_SOC_LPSPI_COUNT > 0)
#define LPSPI_DBG_STATE_COUNT (FSL_FEATURE_SOC_LPSPI_COUNT)
#else
#define LPSPI_DBG_STATE_COUNT (4u)
#endif
#endif

static lpspi_dbg_state_t s_lpspiDbgState[LPSPI_DBG_STATE_COUNT];
static void LPSPI_MemDbgLog(const char *stage, status_t st, uint8_t cmd0, uint32_t addr, uint32_t len, LPSPI_Type *base)
{
#if BSP_UART_PRINT_ENABLE
    const uint32_t now = OSA_TimeGetMsec();
    uint32_t suppressed = 0u;
    bool shouldPrint = false;

    OSA_SR_ALLOC();
    OSA_ENTER_CRITICAL();
    uint32_t idx = 0u;
    bool found = false;
    for (uint32_t i = 0u; i < (uint32_t)(sizeof(s_lpspiDbgState) / sizeof(s_lpspiDbgState[0])); i++)
    {
        if (s_lpspiDbgState[i].base == base)
        {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found)
    {
        bool inserted = false;
        for (uint32_t i = 0u; i < (uint32_t)(sizeof(s_lpspiDbgState) / sizeof(s_lpspiDbgState[0])); i++)
        {
            if (s_lpspiDbgState[i].base == NULL)
            {
                idx = i;
                s_lpspiDbgState[i].base = base;
                inserted = true;
                break;
            }
        }
        if (!inserted)
        {
            uint32_t oldestIdx = 0u;
            uint32_t oldestMs = s_lpspiDbgState[0].lastMs;
            for (uint32_t i = 1u; i < (uint32_t)(sizeof(s_lpspiDbgState) / sizeof(s_lpspiDbgState[0])); i++)
            {
                if (s_lpspiDbgState[i].lastMs <= oldestMs)
                {
                    oldestMs = s_lpspiDbgState[i].lastMs;
                    oldestIdx = i;
                }
            }
            idx = oldestIdx;
            s_lpspiDbgState[idx].base = base;
            s_lpspiDbgState[idx].lastMs = 0u;
            s_lpspiDbgState[idx].suppressed = 0u;
            s_lpspiDbgState[idx].everPrinted = false;
        }
    }

    lpspi_dbg_state_t *s = &s_lpspiDbgState[idx];
    if (!s->everPrinted)
    {
        shouldPrint = true;
        s->everPrinted = true;
        s->lastMs = now;
        suppressed = s->suppressed;
        s->suppressed = 0u;
    }
    else if ((now - s->lastMs) < 2000U)
    {
        s->suppressed++;
        shouldPrint = false;
    }
    else
    {
        shouldPrint = true;
        s->lastMs = now;
        suppressed = s->suppressed;
        s->suppressed = 0u;
    }
    OSA_EXIT_CRITICAL();

    if (!shouldPrint)
    {
        return;
    }

    const uint32_t sr = (base != NULL) ? LPSPI_GetStatusFlags(base) : 0u;
    BSP_UART_Print("[LPSPI] %s st=%d cmd=0x%02X addr=0x%06lX len=%lu sr=0x%08lX sup=%lu\r\n",
                   stage, (int)st, (unsigned)cmd0, (unsigned long)addr, (unsigned long)len, (unsigned long)sr,
                   (unsigned long)suppressed);
#else
    (void)stage;
    (void)st;
    (void)cmd0;
    (void)addr;
    (void)len;
    (void)base;
#endif
}

/*******************************************************************************
 * Codes
 ******************************************************************************/
status_t LPSPI_MemInit(spi_master_config_t *config, LPSPI_Type *base)
{
    status_t status = kStatus_Fail;
    do
    {
        if (config == NULL)
        {
            status = kStatus_InvalidArgument;
            break;
        }

        BOARD_LpspiIomuxConfig(kSpiIomux_SpiMode);

        lpspi_master_config_t lpspiMasterCfg;
        LPSPI_MasterGetDefaultConfig(&lpspiMasterCfg);

        lpspiMasterCfg.baudRate                      = config->baudRate;
        lpspiMasterCfg.pcsToSckDelayInNanoSec        = 1000000000U / lpspiMasterCfg.baudRate;
        lpspiMasterCfg.lastSckToPcsDelayInNanoSec    = 1000000000U / lpspiMasterCfg.baudRate;
        lpspiMasterCfg.betweenTransferDelayInNanoSec = 1000000000U / lpspiMasterCfg.baudRate;

        LPSPI_Type *lpspiInstance = base;
        BOARD_LpspiPcsPinControl(false);

        LPSPI_MasterInit(lpspiInstance, &lpspiMasterCfg, config->clockFreq);
        status = kStatus_Success;
    } while (false);

    return status;
}

#if defined(__ICCARM__)
#pragma optimize = speed
#endif
status_t LPSPI_MemXfer(spi_mem_xfer_t *xfer, LPSPI_Type *base)
{
    status_t status = kStatus_Fail;

    do
    {
        if (xfer == NULL)
        {
            status = kStatus_InvalidArgument;
            break;
        }

#if (FSL_FEATURE_SOC_LPSPI_COUNT > 0)
        const uint8_t cmd0 = (xfer->cmd != NULL && xfer->cmdSize != 0u) ? xfer->cmd[0] : 0u;
        uint32_t addr24 = 0u;
        if (xfer->cmd != NULL && xfer->cmdSize >= 4u)
        {
            addr24 = ((uint32_t)xfer->cmd[1] << 16) | ((uint32_t)xfer->cmd[2] << 8) | ((uint32_t)xfer->cmd[3]);
        }

        BOARD_LpspiPcsPinControl(false);
        LPSPI_FlushFifo(base, true, true);
        LPSPI_ClearStatusFlags(base, (uint32_t)kLPSPI_AllStatusFlag);

        BOARD_LpspiPcsPinControl(true);

        switch (xfer->mode)
        {
            case kSpiMem_Xfer_CommandOnly:
            {
                lpspi_transfer_t txXfer;
                txXfer.txData      = xfer->cmd;
                txXfer.dataSize    = xfer->cmdSize;
                txXfer.rxData      = NULL;
                txXfer.configFlags = (uint32_t)kLPSPI_MasterPcs0 | (uint32_t)kLPSPI_MasterPcsContinuous;
                status             = LPSPI_MemWaitModuleIdle(base);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("WaitIdle(CmdOnly)", status, cmd0, addr24, xfer->cmdSize, base);
                    break;
                }
                status = LPSPI_MasterTransferBlocking(base, &txXfer);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("CmdOnly", status, cmd0, addr24, xfer->cmdSize, base);
                }
            }
            break;
            case kSpiMem_Xfer_CommandWriteData:
            {
                lpspi_transfer_t cmdXfer;
                cmdXfer.txData      = xfer->cmd;
                cmdXfer.dataSize    = xfer->cmdSize;
                cmdXfer.rxData      = NULL;
                cmdXfer.configFlags = (uint32_t)kLPSPI_MasterPcs0 | (uint32_t)kLPSPI_MasterPcsContinuous;
                lpspi_transfer_t dataXfer;
                dataXfer.txData      = xfer->data;
                dataXfer.dataSize    = xfer->dataSize;
                dataXfer.rxData      = NULL;
                dataXfer.configFlags = (uint32_t)kLPSPI_MasterPcs0 | (uint32_t)kLPSPI_MasterPcsContinuous;
                status               = LPSPI_MemWaitModuleIdle(base);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("WaitIdle(CmdW)", status, cmd0, addr24, xfer->cmdSize, base);
                    break;
                }
                status = LPSPI_MasterTransferBlocking(base, &cmdXfer);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("CmdXfer(W)", status, cmd0, addr24, xfer->cmdSize, base);
                    break;
                }
                status = LPSPI_MemWaitModuleIdle(base);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("WaitIdle(DataW)", status, cmd0, addr24, xfer->dataSize, base);
                    break;
                }
                status = LPSPI_MasterTransferBlocking(base, &dataXfer);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("DataXfer(W)", status, cmd0, addr24, xfer->dataSize, base);
                }
            }
            break;
            case kSpiMem_Xfer_CommandReadData:
            {
                lpspi_transfer_t cmdXfer;
                cmdXfer.txData      = xfer->cmd;
                cmdXfer.dataSize    = xfer->cmdSize;
                cmdXfer.rxData      = NULL;
                cmdXfer.configFlags = (uint32_t)kLPSPI_MasterPcs0 | (uint32_t)kLPSPI_MasterPcsContinuous;
                lpspi_transfer_t dataXfer;
                dataXfer.txData      = NULL;
                dataXfer.dataSize    = xfer->dataSize;
                dataXfer.rxData      = xfer->data;
                dataXfer.configFlags = (uint32_t)kLPSPI_MasterPcs0 | (uint32_t)kLPSPI_MasterPcsContinuous;
                status               = LPSPI_MemWaitModuleIdle(base);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("WaitIdle(CmdR)", status, cmd0, addr24, xfer->cmdSize, base);
                    break;
                }
                status = LPSPI_MasterTransferBlocking(base, &cmdXfer);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("CmdXfer(R)", status, cmd0, addr24, xfer->cmdSize, base);
                    break;
                }
                status = LPSPI_MemWaitModuleIdle(base);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("WaitIdle(DataR)", status, cmd0, addr24, xfer->dataSize, base);
                    break;
                }
                status = LPSPI_MasterTransferBlocking(base, &dataXfer);
                if (status != kStatus_Success)
                {
                    LPSPI_MemDbgLog("DataXfer(R)", status, cmd0, addr24, xfer->dataSize, base);
                }
            }
            break;
            default:
                /* To avoid MISRA-C 2012 rule 16.4 issue. */
                break;
        }
        BOARD_LpspiPcsPinControl(false);

#endif
    } while (false);

    return status;
}

status_t LPSPI_MemReadId(flash_id_t *flashId, LPSPI_Type *base)
{
    status_t status = kStatus_Fail;

    do
    {
        if (flashId == NULL)
        {
            status = kStatus_InvalidArgument;
            break;
        }

        uint8_t cmdBuffer[1] = {kFlashCmd_ReadId};
        uint8_t dataBuffer[sizeof(flash_id_t)];
        spi_mem_xfer_t spiMemXfer;
        spiMemXfer.cmd      = cmdBuffer;
        spiMemXfer.cmdSize  = sizeof(cmdBuffer);
        spiMemXfer.data     = dataBuffer;
        spiMemXfer.dataSize = sizeof(dataBuffer);
        spiMemXfer.mode     = kSpiMem_Xfer_CommandReadData;
        status              = LPSPI_MemXfer(&spiMemXfer, base);

        if (status != kStatus_Success)
        {
            break;
        }

        (void)memcpy(&flashId->mid, dataBuffer, sizeof(flash_id_t));

        // According to JEP106AV, the valid ID starts from 0x01 to 0xFE, in which bit7 is the odd checksum bit,
        // and 0x7F is Continuation code
        uint32_t tempMid = flashId->mid;
        // Invalid manufacturer id
        if ((tempMid == 0u) || (tempMid == 0xFFu))
        {
            status = kStatus_Fail;
            break;
        }

        uint8_t *id_buf = (uint8_t *)dataBuffer;
        for (uint32_t i = 0u; i < sizeof(flash_id_t); i++)
        {
            if (*id_buf == 0x7Fu)
            {
                ++id_buf;
                continue;
            }
            break;
        }

        tempMid              = *id_buf;
        uint32_t oddBitCount = 0u;
        for (uint32_t i = 0u; i < 8u; i++)
        {
            if ((tempMid & 1u) != 0U)
            {
                ++oddBitCount;
            }
            tempMid >>= 1u;
        }
        // Parity: Odd
        if ((oddBitCount & 1u) == 0u)
        {
            status = kStatus_Fail;
            break;
        }

        flashId->mid    = id_buf[0];
        flashId->did[0] = id_buf[1];
        flashId->did[1] = id_buf[2];

        status = kStatus_Success;

    } while (false);

    return status;
}

static status_t LPSPI_MemWaitBusy(LPSPI_Type *base)
{
    bool isBusy     = true;
    status_t status = kStatus_Fail;
    do
    {
        uint8_t flashStatus = 0u;
        status = LPSPI_MemReadStatus(&flashStatus, base);
        if (status != kStatus_Success)
        {
            break;
        }

        isBusy = (flashStatus & 1U) != 0U;
    } while (isBusy);

    return status;
}

static status_t LPSPI_MemReadStatus(uint8_t *outStatus, LPSPI_Type *base)
{
    if (outStatus == NULL)
    {
        return kStatus_InvalidArgument;
    }

    uint8_t cmdBuffer[] = {kFlashCmd_ReadStatus};
    spi_mem_xfer_t spiMemXfer;
    spiMemXfer.cmd      = cmdBuffer;
    spiMemXfer.cmdSize  = sizeof(cmdBuffer);
    spiMemXfer.data     = outStatus;
    spiMemXfer.dataSize = 1u;
    spiMemXfer.mode     = kSpiMem_Xfer_CommandReadData;

    return LPSPI_MemXfer(&spiMemXfer, base);
}

static status_t LPSPI_MemWaitModuleIdle(LPSPI_Type *base)
{
    if (base == NULL)
    {
        return kStatus_InvalidArgument;
    }

    for (uint32_t i = 0u; i < 200000u; i++)
    {
        if ((LPSPI_GetStatusFlags(base) & (uint32_t)kLPSPI_ModuleBusyFlag) == 0u)
        {
            return kStatus_Success;
        }
    }
    return kStatus_LPSPI_Timeout;
}

status_t LPSPI_MemIsBusy(LPSPI_Type *base, bool *isBusy)
{
    status_t status = kStatus_Fail;

    uint8_t cmdBuffer[] = {kFlashCmd_ReadStatus};
    uint8_t flashStatus = 0u;
    spi_mem_xfer_t spiMemXfer;
    spiMemXfer.cmd      = cmdBuffer;
    spiMemXfer.cmdSize  = sizeof(cmdBuffer);
    spiMemXfer.data     = &flashStatus;
    spiMemXfer.dataSize = 1u;
    spiMemXfer.mode     = kSpiMem_Xfer_CommandReadData;

    status = LPSPI_MemXfer(&spiMemXfer, base);
    if (status != kStatus_Success)
    {
        return status;
    }

    *isBusy = (flashStatus & 1U) != 0U;

    return status;
}

status_t LPSPI_MemRead(uint32_t addr, uint8_t *buffer, uint32_t lengthInBytes, bool isFastRead, LPSPI_Type *base)
{
    status_t status = kStatus_Fail;

    uint8_t cmdBuffer[5];
    uint32_t cmdSize = 4u;

    if (isFastRead)
    {
        cmdBuffer[0] = kFlashCmd_FastRead;
        cmdSize      = 5u;
        cmdBuffer[4] = 0x00u; // DUMMY byte for fast read operation.
    }
    else
    {
        cmdBuffer[0] = kFlashCmd_ReadMemory24Bit;
    }

    uint32_t tmpAddr = addr;
    for (uint32_t i = 3u; i > 0u; i--)
    {
        cmdBuffer[i] = (uint8_t)(tmpAddr & 0xFFu);
        tmpAddr >>= 8u;
    }

    spi_mem_xfer_t spiMemXfer;
    spiMemXfer.cmd      = cmdBuffer;
    spiMemXfer.cmdSize  = cmdSize;
    spiMemXfer.data     = buffer;
    spiMemXfer.dataSize = lengthInBytes;
    spiMemXfer.mode     = kSpiMem_Xfer_CommandReadData;

    status = LPSPI_MemXfer(&spiMemXfer, base);

    return status;
}

status_t LPSPI_MemWriteEnable(LPSPI_Type *base)
{
    uint8_t cmdBuffer[5];
    uint32_t cmdSize = 4u;

    cmdBuffer[0] = kFlashCmd_WriteEnable;
    cmdSize      = 1u;

    spi_mem_xfer_t spiMemXfer;
    spiMemXfer.cmd      = cmdBuffer;
    spiMemXfer.cmdSize  = cmdSize;
    spiMemXfer.data     = NULL;
    spiMemXfer.dataSize = 0U;
    spiMemXfer.mode     = kSpiMem_Xfer_CommandOnly;

    const uint32_t max_attempts = 3u;
    for (uint32_t attempt = 0u; attempt < max_attempts; attempt++)
    {
        status_t status = LPSPI_MemXfer(&spiMemXfer, base);
        if (status != kStatus_Success)
        {
            LPSPI_MemDbgLog("WriteEnable", status, cmdBuffer[0], 0u, 0u, base);
            OSA_TimeDelay(1u);
            continue;
        }

        uint8_t flashStatus = 0u;
        status = LPSPI_MemReadStatus(&flashStatus, base);
        if (status != kStatus_Success)
        {
            LPSPI_MemDbgLog("WREN_ReadSR", status, cmdBuffer[0], 0u, 1u, base);
            OSA_TimeDelay(1u);
            continue;
        }

        if ((flashStatus & 0x02u) != 0u)
        {
            return kStatus_Success;
        }

        LPSPI_MemDbgLog("WREN_NoWEL", kStatus_Fail, cmdBuffer[0], (uint32_t)flashStatus, 1u, base);
        OSA_TimeDelay(1u);
    }

    return kStatus_Fail;
}

status_t LPSPI_MemWritePage(uint32_t addr, uint8_t *buffer, uint32_t lengthInBytes, bool blocking, LPSPI_Type *base)
{
    status_t status = kStatus_Fail;

    do
    {
        if (lengthInBytes == 0u)
        {
            status = kStatus_Success;
            break;
        }

        uint8_t cmdBuffer[5];
        uint32_t cmdSize = 4u;

        status = LPSPI_MemWriteEnable(base);
        if (status != kStatus_Success)
        {
            break;
        }

        cmdBuffer[0]     = kFlashCmd_PageProgram;
        uint32_t tmpAddr = addr;

        for (uint32_t i = 3u; i > 0u; i--)
        {
            cmdBuffer[i] = (uint8_t)(tmpAddr & 0xFFu);
            tmpAddr >>= 8u;
        }

        spi_mem_xfer_t spiMemXfer;
        spiMemXfer.cmd      = cmdBuffer;
        spiMemXfer.cmdSize  = cmdSize;
        spiMemXfer.data     = buffer;
        spiMemXfer.dataSize = lengthInBytes;
        spiMemXfer.mode     = kSpiMem_Xfer_CommandWriteData;

        status = LPSPI_MemXfer(&spiMemXfer, base);
        if (status != kStatus_Success)
        {
            break;
        }

        if (true == blocking)
        {
            status = LPSPI_MemWaitBusy(base);
            // 等待program完成后再额外稳定一段时间，确保flash内部数据可靠后再开始验证读取
            // 根因：sample=2113时program pipeline未完成就开始了验证读，导致VerifyRetryOk
            OSA_TimeDelay(1u);
        }
    } while (false);

    return status;
}

status_t LPSPI_MemErase(uint32_t addr, eraseOptions_t option, bool blocking, LPSPI_Type *base)
{
    status_t status = kStatus_Fail;

    do
    {
        uint8_t cmdBuffer[5];
        uint32_t cmdSize = 4u;

        status = LPSPI_MemWriteEnable(base);
        if (status != kStatus_Success)
        {
            break;
        }

        if (option == kSize_EraseAll)
        {
            cmdBuffer[0] = kFlashCmd_EraseAll;
            cmdSize      = 1u;
        }
        else
        {
            switch (option)
            {
                case kSize_ErasePage:
                    cmdBuffer[0] = kFlashCmd_ErasePage;
                    break;

                case kSize_Erase4K:
                    cmdBuffer[0] = kFlashCmd_Erase4K;
                    break;

                case kSize_Erase32K:
                    cmdBuffer[0] = kFlashCmd_Erase32K;
                    break;

                case kSize_Erase64K:
                    cmdBuffer[0] = kFlashCmd_Erase64K;
                    break;

                default:
                    status = kStatus_Fail;
                    break;
            }
            if (status != kStatus_Success)
            {
                break;
            }
            uint32_t tmpAddr = addr;
            for (uint32_t i = 3u; i > 0u; i--)
            {
                cmdBuffer[i] = (uint8_t)(tmpAddr & 0xFFu);
                tmpAddr >>= 8u;
            }
        }

        spi_mem_xfer_t spiMemXfer;
        spiMemXfer.cmd      = cmdBuffer;
        spiMemXfer.cmdSize  = cmdSize;
        spiMemXfer.data     = NULL;
        spiMemXfer.dataSize = 0U;
        spiMemXfer.mode     = kSpiMem_Xfer_CommandOnly;

        status = LPSPI_MemXfer(&spiMemXfer, base);
        if (status != kStatus_Success)
        {
            break;
        }

        if (true == blocking)
        {
            status = LPSPI_MemWaitBusy(base);
        }
    } while (false);

    return status;
}

status_t LPSPI_MemDeinit(LPSPI_Type *base)
{
    status_t status = kStatus_Fail;
    do
    {
        // Assert the PCS to high first
        BOARD_LpspiPcsPinControl(false);
        // De-initialize LPSPI
        LPSPI_Type *lpspiInstance = base;
        LPSPI_Deinit(lpspiInstance);

        BOARD_LpspiIomuxConfig(kSpiIomux_DefaultMode);

        status = kStatus_Success;
    } while (false);

    return status;
}
