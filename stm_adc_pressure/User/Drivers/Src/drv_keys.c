#include "drv_keys.h"

#include "main.h"

#include "stm32f4xx_hal.h"

#define DRV_KEYS_DEBOUNCE_MS 15U

typedef struct
{
  uint8_t raw_pressed;
  uint8_t stable_pressed;
  uint32_t changed_tick;
} DrvKeyState;

static DrvKeyState drv_key_states[DRV_KEY_ID_COUNT];

static GPIO_PinState drv_keys_read_pin(DrvKeyId key_id)
{
  switch (key_id)
  {
    case DRV_KEY_ID_0:
      return HAL_GPIO_ReadPin(KEY0_GPIO_Port, KEY0_Pin);
    case DRV_KEY_ID_1:
      return HAL_GPIO_ReadPin(KEY1_GPIO_Port, KEY1_Pin);
    case DRV_KEY_ID_2:
      return HAL_GPIO_ReadPin(KEY2_GPIO_Port, KEY2_Pin);
    case DRV_KEY_ID_COUNT:
    default:
      return GPIO_PIN_SET;
  }
}

static uint8_t drv_keys_read_pressed(DrvKeyId key_id)
{
  return (drv_keys_read_pin(key_id) == GPIO_PIN_RESET) ? 1U : 0U;
}

void DrvKeys_Init(void)
{
  uint32_t now = HAL_GetTick();

  for (uint32_t index = 0U; index < (uint32_t)DRV_KEY_ID_COUNT; ++index)
  {
    uint8_t pressed = drv_keys_read_pressed((DrvKeyId)index);

    drv_key_states[index].raw_pressed = pressed;
    drv_key_states[index].stable_pressed = pressed;
    drv_key_states[index].changed_tick = now;
  }
}

uint8_t DrvKeys_IsPressed(DrvKeyId key_id)
{
  if ((uint32_t)key_id >= (uint32_t)DRV_KEY_ID_COUNT)
  {
    return 0U;
  }

  return drv_key_states[(uint32_t)key_id].stable_pressed;
}

uint32_t DrvKeys_PollEvents(void)
{
  uint32_t events = 0U;
  uint32_t now = HAL_GetTick();

  for (uint32_t index = 0U; index < (uint32_t)DRV_KEY_ID_COUNT; ++index)
  {
    uint8_t pressed = drv_keys_read_pressed((DrvKeyId)index);
    DrvKeyState *state = &drv_key_states[index];

    if (pressed != state->raw_pressed)
    {
      state->raw_pressed = pressed;
      state->changed_tick = now;
      continue;
    }

    if (((now - state->changed_tick) >= DRV_KEYS_DEBOUNCE_MS) &&
        (state->stable_pressed != pressed))
    {
      state->stable_pressed = pressed;

      if (pressed != 0U)
      {
        events |= (1UL << index);
      }
    }
  }

  return events;
}
