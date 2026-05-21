/*
 * bsp_led.c
 *  目前: 采集 → 14pin  |   发送 → 13pin  |  电量 → 45pin
 *  Created on: 2026年5月20日
 *      Author: elois
 */

#include "bsp_led.h"

#include "board/pin_mux.h"
#include "fsl_gpio.h"

typedef struct
{
    GPIO_Type *gpio;
    uint32_t pin;
} bsp_led_pin_t;

static bool s_ledInited = false;

static const bsp_led_pin_t s_ledPins[BSP_LED_COUNT] = {
    [BSP_LED_BLE] = {.gpio = BOARD_INITPINBUTTON0_BLE_LED_GPIO, .pin = BOARD_INITPINBUTTON0_BLE_LED_PIN},
    [BSP_LED_COLLECTION] = {.gpio = BOARD_INITPINBUTTON0_Collection_LED_GPIO, .pin = BOARD_INITPINBUTTON0_Collection_LED_PIN},
    [BSP_LED_SEND] = {.gpio = BOARD_INITPINBUTTON0_Send_LED_GPIO, .pin = BOARD_INITPINBUTTON0_Send_LED_PIN},
    [BSP_LED_ELECTRICITY] = {.gpio = BOARD_INITPINBUTTON0_Electricity_LED_GPIO, .pin = BOARD_INITPINBUTTON0_Electricity_LED_PIN},
};

void BSP_LED_Init(void)
{
    if (s_ledInited)
    {
        return;
    }

    BOARD_InitPinButton0();

    const gpio_pin_config_t cfg = {
        .pinDirection = kGPIO_DigitalOutput,
        .outputLogic  = 0U,
    };

    for (uint32_t i = 0; i < (uint32_t)BSP_LED_COUNT; i++)
    {
        GPIO_PinInit(s_ledPins[i].gpio, s_ledPins[i].pin, &cfg);
    }

    s_ledInited = true;
}

void BSP_LED_Deinit(void)
{
    if (!s_ledInited)
    {
        return;
    }

    for (uint32_t i = 0; i < (uint32_t)BSP_LED_COUNT; i++)
    {
        GPIO_PinWrite(s_ledPins[i].gpio, s_ledPins[i].pin, 0U);
    }

    s_ledInited = false;
}

void BSP_LED_Set(bsp_led_id_t led, bool on)
{
    if ((led >= BSP_LED_COUNT) || (!s_ledInited))
    {
        return;
    }

    GPIO_PinWrite(s_ledPins[led].gpio, s_ledPins[led].pin, on ? 1U : 0U);
}

void BSP_LED_Toggle(bsp_led_id_t led)
{
    if ((led >= BSP_LED_COUNT) || (!s_ledInited))
    {
        return;
    }

    GPIO_PortToggle(s_ledPins[led].gpio, 1UL << s_ledPins[led].pin);
}

