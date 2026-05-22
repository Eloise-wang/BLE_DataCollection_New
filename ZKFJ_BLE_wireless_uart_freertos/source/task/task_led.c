#include "task_manager.h"

#include "bsp_led.h"
#include "sensors.h"
#include "task.h"

// LED任务
void TASK_LedTask(void *pvParameters)
{
    (void)pvParameters;

    BSP_LED_Init();

    TickType_t lastWake = xTaskGetTickCount();
    TickType_t lastElectricityToggle = xTaskGetTickCount();

    for (;;)
    {
        const EventBits_t bits = (g_system_event_group != NULL) ? xEventGroupGetBits(g_system_event_group) : 0U;

        const bool collecting = ((bits & TASK_EVENT_BIT_COLLECT_RUNNING) != 0U);
        const TickType_t period = collecting ? pdMS_TO_TICKS(300U) : pdMS_TO_TICKS(1000U);

        vTaskDelayUntil(&lastWake, period);

        BSP_LED_Toggle(BSP_LED_COLLECTION);

        const uint8_t bat = SENSORS_GetBatteryLevel();
        const bool lowBattery = (bat < 20U);
        if (lowBattery)
        {
            const TickType_t now = xTaskGetTickCount();
            if ((now - lastElectricityToggle) >= pdMS_TO_TICKS(300U))
            {
                lastElectricityToggle = now;
                BSP_LED_Toggle(BSP_LED_ELECTRICITY);
            }
        }
        else
        {
            BSP_LED_Set(BSP_LED_ELECTRICITY, false);
            lastElectricityToggle = xTaskGetTickCount();
        }
    }
}
