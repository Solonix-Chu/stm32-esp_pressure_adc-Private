#ifndef ADC_LINK_PROTOCOL_H
#define ADC_LINK_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_LINK_MAGIC UINT32_C(0x314B4E4C)
#define ADC_LINK_VERSION UINT16_C(1)
#define ADC_LINK_HEADER_BYTES UINT16_C(40)
#define ADC_LINK_PAYLOAD_BYTES UINT32_C(3072)
#define ADC_LINK_PACKET_BYTES (ADC_LINK_HEADER_BYTES + ADC_LINK_PAYLOAD_BYTES)
#define ADC_LINK_CHANNEL_COUNT UINT16_C(6)
#define ADC_LINK_SAMPLES_PER_CHANNEL UINT16_C(256)
#define ADC_LINK_BITS_PER_SAMPLE UINT16_C(12)
#define ADC_LINK_SAMPLE_RATE_HZ UINT32_C(51470)
#define ADC_LINK_FLAG_HALF UINT16_C(0x0001)
#define ADC_LINK_FLAG_FULL UINT16_C(0x0002)

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t sequence;
    uint32_t tick_ms;
    uint32_t sample_rate_hz;
    uint16_t channel_count;
    uint16_t samples_per_channel;
    uint16_t bits_per_sample;
    uint16_t flags;
    uint32_t dropped_packets;
    uint32_t payload_bytes;
    uint32_t checksum;
} adc_link_packet_header_t;

typedef enum
{
    ADC_LINK_PACKET_OK = 0,
    ADC_LINK_PACKET_ERR_LENGTH,
    ADC_LINK_PACKET_ERR_MAGIC,
    ADC_LINK_PACKET_ERR_VERSION,
    ADC_LINK_PACKET_ERR_HEADER_BYTES,
    ADC_LINK_PACKET_ERR_SAMPLE_RATE,
    ADC_LINK_PACKET_ERR_CHANNEL_COUNT,
    ADC_LINK_PACKET_ERR_SAMPLES_PER_CHANNEL,
    ADC_LINK_PACKET_ERR_BITS_PER_SAMPLE,
    ADC_LINK_PACKET_ERR_PAYLOAD_BYTES,
    ADC_LINK_PACKET_ERR_FLAGS,
    ADC_LINK_PACKET_ERR_CHECKSUM,
} adc_link_packet_status_t;

uint32_t adc_link_compute_checksum(const uint8_t *packet, size_t length);
adc_link_packet_status_t adc_link_validate_packet(const uint8_t *packet,
                                                  size_t length,
                                                  adc_link_packet_header_t *out_header,
                                                  uint32_t *out_computed_checksum);
const char *adc_link_packet_status_to_string(adc_link_packet_status_t status);

#ifdef __cplusplus
}
#endif

#endif
