#include "task_manager.h"

#include "bsp_led.h"
#include "sensors.h"
#include "task.h"

void TASK_LedTask(void *pvParameters)
{
    (void)pvParameters;

    BSP_LED_Init();

    TickType_t lastElectricityToggle = xTaskGetTickCount();

    TickType_t collectionPulseUntil = 0U;
    bool didPowerOnSelfTest = false;
    const TickType_t powerOnStart = xTaskGetTickCount();

    for (;;)
    {
        const EventBits_t bits = (g_system_event_group != NULL) ? xEventGroupGetBits(g_system_event_group) : 0U;

        const TickType_t now = xTaskGetTickCount();

        //LED任务启动后延时 250ms 自动执行一次流水灯 用来自检
        if (!didPowerOnSelfTest && ((now - powerOnStart) >= pdMS_TO_TICKS(250U)))
        {
            const bsp_led_id_t order[] = {
                BSP_LED_BLE,
                BSP_LED_COLLECTION,
                BSP_LED_SEND,
                BSP_LED_ELECTRICITY,
            };

            for (uint32_t i = 0U; i < (uint32_t)(sizeof(order) / sizeof(order[0])); i++)
            {
                BSP_LED_Set(order[i], true);
                vTaskDelay(pdMS_TO_TICKS(250U));
                BSP_LED_Set(order[i], false);
                vTaskDelay(pdMS_TO_TICKS(20U));
            }

            lastElectricityToggle = now;

            didPowerOnSelfTest = true;
        }

        const bool collecting   = ((bits & TASK_EVENT_BIT_COLLECT_RUNNING) != 0U);
        const bool collectDone  = ((bits & TASK_EVENT_BIT_COLLECT_DONE) != 0U);
        const bool bleConnected = ((bits & TASK_EVENT_BIT_BLE_CONNECTED) != 0U);
        const bool historySend  = ((bits & TASK_EVENT_BIT_HISTORY_SENDING) != 0U);
        const bool erasing      = ((bits & TASK_EVENT_BIT_ERASING) != 0U);
        const bool pretest      = TASK_IsPretest();

        BSP_LED_Set(BSP_LED_BLE, bleConnected);
        BSP_LED_Set(BSP_LED_SEND, historySend || erasing);

        if ((g_system_event_group != NULL) && ((bits & TASK_EVENT_BIT_COLLECTION_PULSE) != 0U))
        {
            collectionPulseUntil = now + pdMS_TO_TICKS(120U);
            (void)xEventGroupClearBits(g_system_event_group, TASK_EVENT_BIT_COLLECTION_PULSE);
        }

        const bool pulseActive = (now < collectionPulseUntil);
        if (pulseActive)
        {
            BSP_LED_Set(BSP_LED_COLLECTION, true);
        }
        else if (collectDone && !pretest)
        {
            BSP_LED_Set(BSP_LED_COLLECTION, true);
        }
        else if (collecting)
        {
            BSP_LED_Set(BSP_LED_COLLECTION, false);
        }
        else
        {
            BSP_LED_Set(BSP_LED_COLLECTION, false);
        }

        if (!didPowerOnSelfTest)
        {
            BSP_LED_Set(BSP_LED_ELECTRICITY, false);
            lastElectricityToggle = now;
        }
        else
        {
            const uint8_t bat = SENSORS_GetBatteryLevel();
            const bool lowBattery = (bat < 30U);
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
        }

        vTaskDelay(pdMS_TO_TICKS(50U));
    }
}
