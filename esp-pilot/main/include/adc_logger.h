#ifndef ADC_LOGGER_H
#define ADC_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_LOGGER_MAX_FILE_NAME_LEN 32
#define ADC_LOGGER_MAX_ERROR_TEXT_LEN 80

typedef struct
{
    bool storage_ready;
    bool spi_ready;
    bool recording_active;
    bool writer_open;
    uint64_t packets_received;
    uint64_t packets_valid;
    uint64_t packets_invalid;
    uint64_t packets_enqueued;
    uint64_t packets_written;
    uint64_t bytes_written;
    uint32_t spi_errors;
    uint32_t checksum_errors;
    uint32_t header_errors;
    uint32_t queue_overflows;
    uint32_t sequence_gap_events;
    uint32_t last_sequence;
    uint32_t last_tick_ms;
    uint32_t last_sample_rate_hz;
    uint32_t last_stm32_dropped_packets;
    uint16_t last_flags;
    int64_t last_packet_time_us;
    size_t storage_total_bytes;
    size_t storage_used_bytes;
    uint32_t queued_packets;
    uint32_t free_packet_slots;
    char active_file[ADC_LOGGER_MAX_FILE_NAME_LEN];
    char last_error[ADC_LOGGER_MAX_ERROR_TEXT_LEN];
} adc_logger_status_t;

typedef struct
{
    char name[ADC_LOGGER_MAX_FILE_NAME_LEN];
    size_t size_bytes;
    int64_t modified_time;
} adc_logger_file_info_t;

typedef struct
{
    size_t count;
    bool truncated;
    adc_logger_file_info_t items[CONFIG_ADC_LOGGER_FILE_LIST_MAX];
} adc_logger_file_list_t;

esp_err_t adc_logger_start(void);
esp_err_t adc_logger_start_recording(void);
esp_err_t adc_logger_stop_recording(void);
void adc_logger_get_status(adc_logger_status_t *out_status);
esp_err_t adc_logger_list_files(adc_logger_file_list_t *out_list);
esp_err_t adc_logger_delete_file(const char *name);
esp_err_t adc_logger_open_file_for_read(const char *name, FILE **out_file, size_t *out_size);
void adc_logger_close_file(FILE *file);
bool adc_logger_is_valid_file_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
