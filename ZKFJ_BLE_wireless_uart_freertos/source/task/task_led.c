#include "task_manager.h"

#include "bsp_led.h"
#include "sensors.h"
#include "task.h"

void TASK_LedTask(void *pvParameters)
{
    (void)pvParameters;

    BSP_LED_Init();

    TickType_t lastElectricityToggle = xTaskGetTickCount();

    BSP_LED_Set(BSP_LED_BLE, true);
    BSP_LED_Set(BSP_LED_COLLECTION, true);
    BSP_LED_Set(BSP_LED_SEND, true);
    BSP_LED_Set(BSP_LED_ELECTRICITY, true);
    vTaskDelay(pdMS_TO_TICKS(1000U));
    BSP_LED_Set(BSP_LED_BLE, false);
    BSP_LED_Set(BSP_LED_COLLECTION, false);
    BSP_LED_Set(BSP_LED_SEND, false);
    BSP_LED_Set(BSP_LED_ELECTRICITY, false);

    TickType_t collectionPulseUntil = 0U;

    for (;;)
    {
        const EventBits_t bits = (g_system_event_group != NULL) ? xEventGroupGetBits(g_system_event_group) : 0U;

        const TickType_t now = xTaskGetTickCount();

        const bool collecting   = ((bits & TASK_EVENT_BIT_COLLECT_RUNNING) != 0U);
        const bool collectDone  = ((bits & TASK_EVENT_BIT_COLLECT_DONE) != 0U);
        const bool bleConnected = ((bits & TASK_EVENT_BIT_BLE_CONNECTED) != 0U);
        const bool historySend  = ((bits & TASK_EVENT_BIT_HISTORY_SENDING) != 0U);

        BSP_LED_Set(BSP_LED_BLE, bleConnected);
        BSP_LED_Set(BSP_LED_SEND, historySend);

        if ((g_system_event_group != NULL) && ((bits & TASK_EVENT_BIT_COLLECTION_PULSE) != 0U))
        {
            collectionPulseUntil = now + pdMS_TO_TICKS(120U);
            (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECTION_PULSE);
        }

        if (collectDone)
        {
            BSP_LED_Set(BSP_LED_COLLECTION, true);
        }
        else if (collecting)
        {
            BSP_LED_Set(BSP_LED_COLLECTION, now < collectionPulseUntil);
        }
        else
        {
            BSP_LED_Set(BSP_LED_COLLECTION, false);
        }

        const uint8_t bat = SENSORS_GetBatteryLevel();
        const bool lowBattery = (bat < 20U);
        if (lowBattery)
        {
            if ((now - lastElectricityToggle) >= pdMS_TO_TICKS(300U))
            {
                lastElectricityToggle = now;
                BSP_LED_Toggle(BSP_LED_ELECTRICITY);
            }
        }
        else
        {
            BSP_LED_Set(BSP_LED_ELECTRICITY, false);
            lastElectricityToggle = now;
        }

        vTaskDelay(pdMS_TO_TICKS(50U));
    }
}
