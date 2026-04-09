#include "adc_link_protocol.h"

#include <stdbool.h>
#include <string.h>

uint32_t adc_link_compute_checksum(const uint8_t *packet, size_t length)
{
    const size_t checksum_offset = offsetof(adc_link_packet_header_t, checksum);
    uint32_t hash = UINT32_C(2166136261);

    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte =
            ((i >= checksum_offset) && (i < (checksum_offset + sizeof(uint32_t)))) ? 0U : packet[i];
        hash ^= (uint32_t) byte;
        hash *= UINT32_C(16777619);
    }

    return hash;
}

adc_link_packet_status_t adc_link_validate_packet(const uint8_t *packet,
                                                  size_t length,
                                                  adc_link_packet_header_t *out_header,
                                                  uint32_t *out_computed_checksum)
{
    adc_link_packet_header_t header = {0};
    uint32_t computed_checksum;

    if ((packet == NULL) || (length != ADC_LINK_PACKET_BYTES)) {
        return ADC_LINK_PACKET_ERR_LENGTH;
    }

    memcpy(&header, packet, sizeof(header));

    if (header.magic != ADC_LINK_MAGIC) {
        return ADC_LINK_PACKET_ERR_MAGIC;
    }
    if (header.version != ADC_LINK_VERSION) {
        return ADC_LINK_PACKET_ERR_VERSION;
    }
    if (header.header_bytes != ADC_LINK_HEADER_BYTES) {
        return ADC_LINK_PACKET_ERR_HEADER_BYTES;
    }
    if (header.sample_rate_hz != ADC_LINK_SAMPLE_RATE_HZ) {
        return ADC_LINK_PACKET_ERR_SAMPLE_RATE;
    }
    if (header.channel_count != ADC_LINK_CHANNEL_COUNT) {
        return ADC_LINK_PACKET_ERR_CHANNEL_COUNT;
    }
    if (header.samples_per_channel != ADC_LINK_SAMPLES_PER_CHANNEL) {
        return ADC_LINK_PACKET_ERR_SAMPLES_PER_CHANNEL;
    }
    if (header.bits_per_sample != ADC_LINK_BITS_PER_SAMPLE) {
        return ADC_LINK_PACKET_ERR_BITS_PER_SAMPLE;
    }
    if (header.payload_bytes != ADC_LINK_PAYLOAD_BYTES) {
        return ADC_LINK_PACKET_ERR_PAYLOAD_BYTES;
    }
    if ((header.flags & (uint16_t) ~(ADC_LINK_FLAG_HALF | ADC_LINK_FLAG_FULL)) != 0U) {
        return ADC_LINK_PACKET_ERR_FLAGS;
    }

    computed_checksum = adc_link_compute_checksum(packet, length);
    if (out_computed_checksum != NULL) {
        *out_computed_checksum = computed_checksum;
    }
    if (computed_checksum != header.checksum) {
        return ADC_LINK_PACKET_ERR_CHECKSUM;
    }

    if (out_header != NULL) {
        *out_header = header;
    }
    return ADC_LINK_PACKET_OK;
}

const char *adc_link_packet_status_to_string(adc_link_packet_status_t status)
{
    switch (status) {
    case ADC_LINK_PACKET_OK:
        return "ok";
    case ADC_LINK_PACKET_ERR_LENGTH:
        return "invalid length";
    case ADC_LINK_PACKET_ERR_MAGIC:
        return "magic mismatch";
    case ADC_LINK_PACKET_ERR_VERSION:
        return "version mismatch";
    case ADC_LINK_PACKET_ERR_HEADER_BYTES:
        return "header size mismatch";
    case ADC_LINK_PACKET_ERR_SAMPLE_RATE:
        return "sample rate mismatch";
    case ADC_LINK_PACKET_ERR_CHANNEL_COUNT:
        return "channel count mismatch";
    case ADC_LINK_PACKET_ERR_SAMPLES_PER_CHANNEL:
        return "samples per channel mismatch";
    case ADC_LINK_PACKET_ERR_BITS_PER_SAMPLE:
        return "bits per sample mismatch";
    case ADC_LINK_PACKET_ERR_PAYLOAD_BYTES:
        return "payload size mismatch";
    case ADC_LINK_PACKET_ERR_FLAGS:
        return "flags invalid";
    case ADC_LINK_PACKET_ERR_CHECKSUM:
        return "checksum mismatch";
    default:
        return "unknown";
    }
}
