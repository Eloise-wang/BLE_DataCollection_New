#include "task_manager.h"

#include "bsp_led.h"
#include "task.h"

// LED任务
void TASK_LedTask(void *pvParameters)
{
    (void)pvParameters;

    BSP_LED_Init();

    TickType_t lastWake = xTaskGetTickCount();

    for (;;)
    {
        const EventBits_t bits = (g_system_event_group != NULL) ? xEventGroupGetBits(g_system_event_group) : 0U;

        const bool collecting = ((bits & TASK_EVENT_BIT_COLLECT_RUNNING) != 0U);
        const TickType_t period = collecting ? pdMS_TO_TICKS(300U) : pdMS_TO_TICKS(1000U);

        vTaskDelayUntil(&lastWake, period);

        BSP_LED_Toggle(BSP_LED_COLLECTION);
    }
}
