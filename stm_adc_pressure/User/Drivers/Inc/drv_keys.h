#ifndef DRV_KEYS_H
#define DRV_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum
{
  DRV_KEY_ID_0 = 0U,
  DRV_KEY_ID_1 = 1U,
  DRV_KEY_ID_2 = 2U,
  DRV_KEY_ID_COUNT
} DrvKeyId;

#define DRV_KEY_EVENT_KEY0 (1UL << 0)
#define DRV_KEY_EVENT_KEY1 (1UL << 1)
#define DRV_KEY_EVENT_KEY2 (1UL << 2)

void DrvKeys_Init(void);
uint8_t DrvKeys_IsPressed(DrvKeyId key_id);
uint32_t DrvKeys_PollEvents(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_KEYS_H */
