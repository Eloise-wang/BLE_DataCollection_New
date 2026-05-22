#include "task_manager.h"

#include "bsp_wdog.h"
#include "task.h"

// 看门狗任务
void TASK_WatchdogTask(void *pvParameters)
{
    (void)pvParameters;

    const bsp_wdog_config_t cfg = {
        .timeout_ms      = 3000U,
        .enable_in_wait  = true,
        .enable_in_stop  = false,
        .enable_in_debug = false,
    };

    BSP_WDOG_Init(&cfg);

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
        BSP_WDOG_Refresh();
    }
}
