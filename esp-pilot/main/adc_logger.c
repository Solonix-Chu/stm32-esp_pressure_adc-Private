#include "adc_logger.h"

#include "adc_link_protocol.h"

#include <inttypes.h>
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define ADC_LOGGER_STORAGE_BASE_PATH "/spiffs"
#define ADC_LOGGER_STORAGE_LABEL "storage"
#define ADC_LOGGER_LOG_INTERVAL_MS 2000
#define ADC_LOGGER_SPI_GUARD_BYTES 4U
#define ADC_LOGGER_SPI_WIRE_BYTES (ADC_LINK_PACKET_BYTES + ADC_LOGGER_SPI_GUARD_BYTES)
#define ADC_LOGGER_FILE_FLUSH_PACKET_INTERVAL 64U
#define ADC_LOGGER_RDY_SETUP_DELAY_US 10
#define ADC_LOGGER_CS_SETUP_DELAY_US 2
#define ADC_LOGGER_CS_HOLD_DELAY_US 2
#define ADC_LOGGER_RDY_LOW_POLL_US 20

typedef struct
{
    uint8_t data[ADC_LINK_PACKET_BYTES];
    size_t length;
} adc_logger_packet_slot_t;

typedef struct
{
    bool started;
    bool storage_ready;
    bool spi_ready;
    bool recording_enabled;
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
    bool last_sequence_valid;
    uint32_t last_tick_ms;
    uint32_t last_sample_rate_hz;
    uint32_t last_stm32_dropped_packets;
    uint16_t last_flags;
    int64_t last_packet_time_us;
    size_t storage_total_bytes;
    size_t storage_used_bytes;
    char active_file[ADC_LOGGER_MAX_FILE_NAME_LEN];
    char active_metadata_file[ADC_LOGGER_MAX_FILE_NAME_LEN];
    char last_error[ADC_LOGGER_MAX_ERROR_TEXT_LEN];
} adc_logger_runtime_t;

static const char *TAG = "adc_logger";

static SemaphoreHandle_t s_state_mutex;
static QueueHandle_t s_free_queue;
static QueueHandle_t s_write_queue;
static TaskHandle_t s_spi_task_handle;
static spi_device_handle_t s_spi_device;
static FILE *s_record_file;
static uint32_t s_writer_packets_since_flush;
static adc_logger_runtime_t s_runtime;
static adc_logger_packet_slot_t s_packet_slots[CONFIG_ADC_LOGGER_QUEUE_DEPTH];
static char s_file_buffer[CONFIG_ADC_LOGGER_FILE_BUFFER_BYTES];

DMA_ATTR static uint8_t s_spi_tx_buffer[ADC_LOGGER_SPI_WIRE_BYTES];
DMA_ATTR static uint8_t s_spi_rx_buffer[ADC_LOGGER_SPI_WIRE_BYTES];

static void adc_logger_writer_task(void *arg);
static void adc_logger_spi_task(void *arg);
static esp_err_t adc_logger_init_storage(void);
static esp_err_t adc_logger_init_spi(void);

static void adc_logger_set_cs_level(uint32_t level)
{
    gpio_set_level(CONFIG_ADC_LOGGER_PIN_CS, level);
}

static bool adc_logger_wait_for_rdy_setup(void)
{
    esp_rom_delay_us(ADC_LOGGER_RDY_SETUP_DELAY_US);
    return gpio_get_level(CONFIG_ADC_LOGGER_PIN_RDY) != 0;
}

static void adc_logger_wait_for_rdy_low(void)
{
    while (gpio_get_level(CONFIG_ADC_LOGGER_PIN_RDY) != 0) {
        esp_rom_delay_us(ADC_LOGGER_RDY_LOW_POLL_US);
    }
}

static void adc_logger_lock(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

static void adc_logger_unlock(void)
{
    xSemaphoreGive(s_state_mutex);
}

static void adc_logger_set_last_error_locked(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(s_runtime.last_error, sizeof(s_runtime.last_error), fmt, args);
    va_end(args);
}

static void adc_logger_update_storage_usage_locked(void)
{
    if (!s_runtime.storage_ready) {
        s_runtime.storage_total_bytes = 0;
        s_runtime.storage_used_bytes = 0;
        return;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(ADC_LOGGER_STORAGE_LABEL, &total, &used) == ESP_OK) {
        s_runtime.storage_total_bytes = total;
        s_runtime.storage_used_bytes = used;
    }
}

static bool adc_logger_recording_or_writer_open_locked(void)
{
    return s_runtime.recording_enabled || s_runtime.writer_open;
}

static bool adc_logger_name_char_allowed(char ch)
{
    return isalnum((unsigned char) ch) || (ch == '_') || (ch == '-') || (ch == '.');
}

bool adc_logger_is_valid_file_name(const char *name)
{
    size_t len;

    if (name == NULL) {
        return false;
    }

    len = strlen(name);
    if ((len == 0U) || (len >= ADC_LOGGER_MAX_FILE_NAME_LEN)) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        if (!adc_logger_name_char_allowed(name[i])) {
            return false;
        }
    }

    return (strchr(name, '/') == NULL) && (strchr(name, '\\') == NULL);
}

static esp_err_t adc_logger_build_path(const char *name, char *path, size_t path_size)
{
    int written;

    if ((!adc_logger_is_valid_file_name(name)) || (path == NULL) || (path_size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(path, path_size, ADC_LOGGER_STORAGE_BASE_PATH "/%s", name);
    if ((written < 0) || ((size_t) written >= path_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static int adc_logger_compare_files_desc(const void *lhs, const void *rhs)
{
    const adc_logger_file_info_t *a = lhs;
    const adc_logger_file_info_t *b = rhs;
    return strcmp(b->name, a->name);
}

static void IRAM_ATTR adc_logger_rdy_isr(void *arg)
{
    TaskHandle_t task = (TaskHandle_t) arg;
    BaseType_t higher_priority_task_woken = pdFALSE;

    vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void adc_logger_log_periodic_status(void)
{
    static int64_t last_log_ms = 0;
    const int64_t now_ms = esp_timer_get_time() / 1000;

    if ((now_ms - last_log_ms) < ADC_LOGGER_LOG_INTERVAL_MS) {
        return;
    }
    last_log_ms = now_ms;

    adc_logger_status_t status = {0};
    adc_logger_get_status(&status);
    ESP_LOGI(TAG,
             "rx=%llu valid=%llu invalid=%llu written=%llu queue=%" PRIu32 "/%" PRIu32
             " spi_err=%" PRIu32 " hdr_err=%" PRIu32 " csum_err=%" PRIu32 " ovf=%" PRIu32
             " rec=%d file=%s err=%s",
             (unsigned long long) status.packets_received,
             (unsigned long long) status.packets_valid,
             (unsigned long long) status.packets_invalid,
             (unsigned long long) status.packets_written,
             status.queued_packets,
             status.free_packet_slots,
             status.spi_errors,
             status.header_errors,
             status.checksum_errors,
             status.queue_overflows,
             status.recording_active ? 1 : 0,
             status.active_file[0] != '\0' ? status.active_file : "-",
             status.last_error);
}

static void adc_logger_copy_slot(adc_logger_packet_slot_t *slot, const uint8_t *packet, size_t length)
{
    slot->length = length;
    memcpy(slot->data, packet, length);
}

static void adc_logger_finish_recording_if_idle(void)
{
    adc_logger_lock();
    const bool should_close =
        (!s_runtime.recording_enabled) && s_runtime.writer_open && (uxQueueMessagesWaiting(s_write_queue) == 0U);
    adc_logger_unlock();

    if (!should_close) {
        return;
    }

    if (s_record_file != NULL) {
        fflush(s_record_file);
        fclose(s_record_file);
        s_record_file = NULL;
    }
    s_writer_packets_since_flush = 0;

    adc_logger_lock();
    s_runtime.writer_open = false;
    s_runtime.active_file[0] = '\0';
    s_runtime.active_metadata_file[0] = '\0';
    adc_logger_update_storage_usage_locked();
    adc_logger_unlock();
}

static void adc_logger_writer_task(void *arg)
{
    (void) arg;

    while (true) {
        adc_logger_packet_slot_t *slot = NULL;

        if (xQueueReceive(s_write_queue, &slot, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool write_ok = false;

            if ((slot != NULL) && (s_record_file != NULL)) {
                const size_t written = fwrite(slot->data, 1, slot->length, s_record_file);
                if (written == slot->length) {
                    write_ok = true;
                    ++s_writer_packets_since_flush;

                    adc_logger_lock();
                    ++s_runtime.packets_written;
                    s_runtime.bytes_written += written;
                    adc_logger_unlock();
                }
            }

            if (!write_ok) {
                adc_logger_lock();
                s_runtime.recording_enabled = false;
                adc_logger_set_last_error_locked("storage write failed");
                adc_logger_unlock();
                if (s_record_file != NULL) {
                    fflush(s_record_file);
                }
            } else if (s_writer_packets_since_flush >= ADC_LOGGER_FILE_FLUSH_PACKET_INTERVAL) {
                fflush(s_record_file);
                s_writer_packets_since_flush = 0;
            }

            if (slot != NULL) {
                xQueueSend(s_free_queue, &slot, portMAX_DELAY);
            }
        }

        adc_logger_finish_recording_if_idle();
    }
}

static esp_err_t adc_logger_queue_packet_for_recording(const uint8_t *packet, size_t length)
{
    adc_logger_packet_slot_t *slot = NULL;

    if (xQueueReceive(s_free_queue, &slot, 0) != pdTRUE) {
        adc_logger_lock();
        ++s_runtime.queue_overflows;
        adc_logger_set_last_error_locked("record queue overflow");
        adc_logger_unlock();
        return ESP_ERR_NO_MEM;
    }

    adc_logger_copy_slot(slot, packet, length);
    if (xQueueSend(s_write_queue, &slot, 0) != pdTRUE) {
        xQueueSend(s_free_queue, &slot, portMAX_DELAY);
        adc_logger_lock();
        ++s_runtime.queue_overflows;
        adc_logger_set_last_error_locked("record queue busy");
        adc_logger_unlock();
        return ESP_ERR_TIMEOUT;
    }

    adc_logger_lock();
    ++s_runtime.packets_enqueued;
    adc_logger_unlock();

    return ESP_OK;
}

static esp_err_t adc_logger_process_packet(void)
{
    spi_transaction_t trans = {
        .length = ADC_LOGGER_SPI_WIRE_BYTES * 8U,
        .rxlength = ADC_LOGGER_SPI_WIRE_BYTES * 8U,
        .tx_buffer = s_spi_tx_buffer,
        .rx_buffer = s_spi_rx_buffer,
    };
    adc_link_packet_header_t header = {0};
    uint32_t computed_checksum = 0;
    const int64_t now_us = esp_timer_get_time();
    adc_link_packet_status_t packet_status;
    bool recording_enabled = false;
    const uint8_t *packet_data = s_spi_rx_buffer;

    adc_logger_set_cs_level(0);
    esp_rom_delay_us(ADC_LOGGER_CS_SETUP_DELAY_US);
    const esp_err_t spi_err = spi_device_polling_transmit(s_spi_device, &trans);
    esp_rom_delay_us(ADC_LOGGER_CS_HOLD_DELAY_US);
    adc_logger_set_cs_level(1);
    if (spi_err != ESP_OK) {
        adc_logger_lock();
        ++s_runtime.packets_received;
        ++s_runtime.spi_errors;
        adc_logger_set_last_error_locked("spi read failed: %s", esp_err_to_name(spi_err));
        adc_logger_unlock();
        return spi_err;
    }

    adc_logger_lock();
    ++s_runtime.packets_received;
    adc_logger_unlock();

    memcpy(&header, s_spi_rx_buffer, sizeof(header));
    packet_status =
        adc_link_validate_packet(s_spi_rx_buffer, ADC_LINK_PACKET_BYTES, &header, &computed_checksum);

    if (packet_status != ADC_LINK_PACKET_OK) {
        for (size_t offset = 1; offset <= ADC_LOGGER_SPI_GUARD_BYTES; ++offset) {
            adc_link_packet_header_t candidate_header = {0};
            uint32_t candidate_checksum = 0;
            const adc_link_packet_status_t candidate_status =
                adc_link_validate_packet(&s_spi_rx_buffer[offset],
                                         ADC_LINK_PACKET_BYTES,
                                         &candidate_header,
                                         &candidate_checksum);
            if (candidate_status == ADC_LINK_PACKET_OK) {
                packet_status = ADC_LINK_PACKET_OK;
                header = candidate_header;
                computed_checksum = candidate_checksum;
                packet_data = &s_spi_rx_buffer[offset];
                break;
            }
        }
    }
    if (packet_status != ADC_LINK_PACKET_OK) {
        adc_logger_lock();
        ++s_runtime.packets_invalid;
        if (packet_status == ADC_LINK_PACKET_ERR_CHECKSUM) {
            ++s_runtime.checksum_errors;
            adc_logger_set_last_error_locked("checksum m=%08" PRIx32 " rx=%08" PRIx32 " calc=%08" PRIx32,
                                             header.magic,
                                             header.checksum,
                                             computed_checksum);
        } else {
            ++s_runtime.header_errors;
            adc_logger_set_last_error_locked("%s m=%08" PRIx32 " v=%u h=%u f=%u",
                                             adc_link_packet_status_to_string(packet_status),
                                             header.magic,
                                             (unsigned int) header.version,
                                             (unsigned int) header.header_bytes,
                                             (unsigned int) header.flags);
        }
        adc_logger_unlock();
        return ESP_FAIL;
    }

    adc_logger_lock();
    ++s_runtime.packets_valid;
    s_runtime.last_tick_ms = header.tick_ms;
    s_runtime.last_sample_rate_hz = header.sample_rate_hz;
    s_runtime.last_stm32_dropped_packets = header.dropped_packets;
    s_runtime.last_flags = header.flags;
    s_runtime.last_packet_time_us = now_us;
    if (s_runtime.last_sequence_valid && (header.sequence != (s_runtime.last_sequence + 1U))) {
        ++s_runtime.sequence_gap_events;
    }
    s_runtime.last_sequence = header.sequence;
    s_runtime.last_sequence_valid = true;
    recording_enabled = s_runtime.recording_enabled;
    adc_logger_unlock();

    if (recording_enabled) {
        return adc_logger_queue_packet_for_recording(packet_data, ADC_LINK_PACKET_BYTES);
    }

    return ESP_OK;
}

static void adc_logger_spi_task(void *arg)
{
    (void) arg;

    while (true) {
        while (gpio_get_level(CONFIG_ADC_LOGGER_PIN_RDY) == 0) {
            (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }

        if (!adc_logger_wait_for_rdy_setup()) {
            continue;
        }

        (void) adc_logger_process_packet();
        adc_logger_log_periodic_status();
        adc_logger_wait_for_rdy_low();
    }
}

static esp_err_t adc_logger_init_storage(void)
{
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = ADC_LOGGER_STORAGE_BASE_PATH,
        .partition_label = ADC_LOGGER_STORAGE_LABEL,
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "mount spiffs failed");

    adc_logger_lock();
    s_runtime.storage_ready = true;
    adc_logger_update_storage_usage_locked();
    adc_logger_unlock();

    ESP_LOGI(TAG,
             "storage mounted at %s (%u bytes total, %u bytes used)",
             ADC_LOGGER_STORAGE_BASE_PATH,
             (unsigned int) s_runtime.storage_total_bytes,
             (unsigned int) s_runtime.storage_used_bytes);

    return ESP_OK;
}

static esp_err_t adc_logger_init_spi(void)
{
    const spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_ADC_LOGGER_PIN_MOSI,
        .miso_io_num = CONFIG_ADC_LOGGER_PIN_MISO,
        .sclk_io_num = CONFIG_ADC_LOGGER_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ADC_LOGGER_SPI_WIRE_BYTES,
    };
    const spi_device_interface_config_t devcfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = CONFIG_ADC_LOGGER_SPI_CLOCK_HZ,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    const gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_ADC_LOGGER_PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t rdy_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_ADC_LOGGER_PIN_RDY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };

    memset(s_spi_tx_buffer, 0xFF, sizeof(s_spi_tx_buffer));
    ESP_RETURN_ON_ERROR(gpio_config(&cs_cfg), TAG, "cs gpio config failed");
    adc_logger_set_cs_level(1);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus init failed");
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_device), TAG, "spi device add failed");
    ESP_RETURN_ON_ERROR(gpio_config(&rdy_cfg), TAG, "rdy gpio config failed");
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    adc_logger_lock();
    s_runtime.spi_ready = true;
    adc_logger_unlock();
    return ESP_OK;
}

static esp_err_t adc_logger_next_recording_name(char *bin_name,
                                                size_t bin_name_size,
                                                char *meta_name,
                                                size_t meta_name_size)
{
    DIR *dir = NULL;
    struct dirent *entry;
    int max_id = -1;

    dir = opendir(ADC_LOGGER_STORAGE_BASE_PATH);
    if (dir == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    while ((entry = readdir(dir)) != NULL) {
        int id = -1;
        if (sscanf(entry->d_name, "adc_raw_%05d.bin", &id) == 1) {
            if (id > max_id) {
                max_id = id;
            }
        }
    }
    closedir(dir);

    const int next_id = max_id + 1;
    if ((snprintf(bin_name, bin_name_size, "adc_raw_%05d.bin", next_id) >= (int) bin_name_size) ||
        (snprintf(meta_name, meta_name_size, "adc_raw_%05d.json", next_id) >= (int) meta_name_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void adc_logger_write_metadata(FILE *meta_file, const char *bin_name)
{
    const time_t now = time(NULL);
    const long long boot_ms = esp_timer_get_time() / 1000;

    fprintf(meta_file,
            "{\n"
            "  \"file\": \"%s\",\n"
            "  \"created_unix\": %lld,\n"
            "  \"boot_ms\": %lld,\n"
            "  \"protocol\": \"stm32_spi_link_v1\",\n"
            "  \"packet_bytes\": %u,\n"
            "  \"header_bytes\": %u,\n"
            "  \"payload_bytes\": %u,\n"
            "  \"sample_rate_hz\": %u,\n"
            "  \"channel_count\": %u,\n"
            "  \"samples_per_channel\": %u,\n"
            "  \"bits_per_sample\": %u\n"
            "}\n",
            bin_name,
            (long long) now,
            boot_ms,
            (unsigned int) ADC_LINK_PACKET_BYTES,
            (unsigned int) ADC_LINK_HEADER_BYTES,
            (unsigned int) ADC_LINK_PAYLOAD_BYTES,
            (unsigned int) ADC_LINK_SAMPLE_RATE_HZ,
            (unsigned int) ADC_LINK_CHANNEL_COUNT,
            (unsigned int) ADC_LINK_SAMPLES_PER_CHANNEL,
            (unsigned int) ADC_LINK_BITS_PER_SAMPLE);
}

esp_err_t adc_logger_start(void)
{
    esp_err_t err;

    if (s_runtime.started) {
        return ESP_OK;
    }

    s_state_mutex = xSemaphoreCreateMutex();
    if (s_state_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_free_queue = xQueueCreate(CONFIG_ADC_LOGGER_QUEUE_DEPTH, sizeof(adc_logger_packet_slot_t *));
    s_write_queue = xQueueCreate(CONFIG_ADC_LOGGER_QUEUE_DEPTH, sizeof(adc_logger_packet_slot_t *));
    if ((s_free_queue == NULL) || (s_write_queue == NULL)) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < CONFIG_ADC_LOGGER_QUEUE_DEPTH; ++i) {
        adc_logger_packet_slot_t *slot = &s_packet_slots[i];
        xQueueSend(s_free_queue, &slot, portMAX_DELAY);
    }

    err = adc_logger_init_storage();
    if (err != ESP_OK) {
        return err;
    }

    err = adc_logger_init_spi();
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(adc_logger_writer_task, "adc_writer", 4096, NULL, 18, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(adc_logger_spi_task, "adc_spi", 6144, NULL, 20, &s_spi_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(CONFIG_ADC_LOGGER_PIN_RDY, adc_logger_rdy_isr, s_spi_task_handle),
                        TAG,
                        "rdy isr attach failed");

    adc_logger_lock();
    s_runtime.started = true;
    adc_logger_set_last_error_locked("ready");
    adc_logger_unlock();

    ESP_LOGI(TAG,
             "STM32 link ready: RDY=%d CS=%d SCK=%d MISO=%d MOSI=%d @ %d Hz",
             CONFIG_ADC_LOGGER_PIN_RDY,
             CONFIG_ADC_LOGGER_PIN_CS,
             CONFIG_ADC_LOGGER_PIN_SCK,
             CONFIG_ADC_LOGGER_PIN_MISO,
             CONFIG_ADC_LOGGER_PIN_MOSI,
             CONFIG_ADC_LOGGER_SPI_CLOCK_HZ);
    return ESP_OK;
}

esp_err_t adc_logger_start_recording(void)
{
    char bin_name[ADC_LOGGER_MAX_FILE_NAME_LEN] = {0};
    char meta_name[ADC_LOGGER_MAX_FILE_NAME_LEN] = {0};
    char bin_path[64] = {0};
    char meta_path[64] = {0};
    FILE *record_file = NULL;
    FILE *meta_file = NULL;

    adc_logger_lock();
    if (!s_runtime.storage_ready) {
        adc_logger_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (adc_logger_recording_or_writer_open_locked()) {
        adc_logger_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    adc_logger_unlock();

    ESP_RETURN_ON_ERROR(adc_logger_next_recording_name(bin_name, sizeof(bin_name), meta_name, sizeof(meta_name)),
                        TAG,
                        "file naming failed");
    ESP_RETURN_ON_ERROR(adc_logger_build_path(bin_name, bin_path, sizeof(bin_path)), TAG, "bin path failed");
    ESP_RETURN_ON_ERROR(adc_logger_build_path(meta_name, meta_path, sizeof(meta_path)), TAG, "meta path failed");

    record_file = fopen(bin_path, "wb");
    if (record_file == NULL) {
        return ESP_FAIL;
    }
    (void) setvbuf(record_file, s_file_buffer, _IOFBF, sizeof(s_file_buffer));

    meta_file = fopen(meta_path, "wb");
    if (meta_file == NULL) {
        fclose(record_file);
        return ESP_FAIL;
    }
    adc_logger_write_metadata(meta_file, bin_name);
    fclose(meta_file);

    adc_logger_lock();
    s_record_file = record_file;
    s_runtime.recording_enabled = true;
    s_runtime.writer_open = true;
    s_runtime.active_file[0] = '\0';
    s_runtime.active_metadata_file[0] = '\0';
    strlcpy(s_runtime.active_file, bin_name, sizeof(s_runtime.active_file));
    strlcpy(s_runtime.active_metadata_file, meta_name, sizeof(s_runtime.active_metadata_file));
    adc_logger_set_last_error_locked("recording %s", bin_name);
    adc_logger_unlock();

    ESP_LOGI(TAG, "recording started: %s", bin_name);
    return ESP_OK;
}

esp_err_t adc_logger_stop_recording(void)
{
    bool was_active = false;

    adc_logger_lock();
    was_active = adc_logger_recording_or_writer_open_locked();
    s_runtime.recording_enabled = false;
    adc_logger_unlock();

    if (!was_active) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < 500; ++i) {
        bool writer_open = false;

        adc_logger_lock();
        writer_open = s_runtime.writer_open;
        adc_logger_unlock();

        if (!writer_open) {
            ESP_LOGI(TAG, "recording stopped");
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_ERR_TIMEOUT;
}

void adc_logger_get_status(adc_logger_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));

    adc_logger_lock();
    adc_logger_update_storage_usage_locked();
    out_status->storage_ready = s_runtime.storage_ready;
    out_status->spi_ready = s_runtime.spi_ready;
    out_status->recording_active = s_runtime.recording_enabled;
    out_status->writer_open = s_runtime.writer_open;
    out_status->packets_received = s_runtime.packets_received;
    out_status->packets_valid = s_runtime.packets_valid;
    out_status->packets_invalid = s_runtime.packets_invalid;
    out_status->packets_enqueued = s_runtime.packets_enqueued;
    out_status->packets_written = s_runtime.packets_written;
    out_status->bytes_written = s_runtime.bytes_written;
    out_status->spi_errors = s_runtime.spi_errors;
    out_status->checksum_errors = s_runtime.checksum_errors;
    out_status->header_errors = s_runtime.header_errors;
    out_status->queue_overflows = s_runtime.queue_overflows;
    out_status->sequence_gap_events = s_runtime.sequence_gap_events;
    out_status->last_sequence = s_runtime.last_sequence;
    out_status->last_tick_ms = s_runtime.last_tick_ms;
    out_status->last_sample_rate_hz = s_runtime.last_sample_rate_hz;
    out_status->last_stm32_dropped_packets = s_runtime.last_stm32_dropped_packets;
    out_status->last_flags = s_runtime.last_flags;
    out_status->last_packet_time_us = s_runtime.last_packet_time_us;
    out_status->storage_total_bytes = s_runtime.storage_total_bytes;
    out_status->storage_used_bytes = s_runtime.storage_used_bytes;
    out_status->queued_packets = (uint32_t) uxQueueMessagesWaiting(s_write_queue);
    out_status->free_packet_slots = (uint32_t) uxQueueMessagesWaiting(s_free_queue);
    strlcpy(out_status->active_file, s_runtime.active_file, sizeof(out_status->active_file));
    strlcpy(out_status->last_error, s_runtime.last_error, sizeof(out_status->last_error));
    adc_logger_unlock();
}

esp_err_t adc_logger_list_files(adc_logger_file_list_t *out_list)
{
    DIR *dir = NULL;
    struct dirent *entry;

    if (out_list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_list, 0, sizeof(*out_list));

    dir = opendir(ADC_LOGGER_STORAGE_BASE_PATH);
    if (dir == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[64] = {0};
        struct stat st = {0};

        if (!adc_logger_is_valid_file_name(entry->d_name)) {
            continue;
        }
        if (adc_logger_build_path(entry->d_name, path, sizeof(path)) != ESP_OK) {
            continue;
        }
        if (stat(path, &st) != 0) {
            continue;
        }

        if (out_list->count >= CONFIG_ADC_LOGGER_FILE_LIST_MAX) {
            out_list->truncated = true;
            continue;
        }

        adc_logger_file_info_t *item = &out_list->items[out_list->count++];
        memset(item, 0, sizeof(*item));
        strlcpy(item->name, entry->d_name, sizeof(item->name));
        item->size_bytes = (size_t) st.st_size;
        item->modified_time = (int64_t) st.st_mtime;
    }
    closedir(dir);

    qsort(out_list->items, out_list->count, sizeof(out_list->items[0]), adc_logger_compare_files_desc);
    return ESP_OK;
}

static bool adc_logger_name_is_active_locked(const char *name)
{
    return (strcmp(name, s_runtime.active_file) == 0) || (strcmp(name, s_runtime.active_metadata_file) == 0);
}

esp_err_t adc_logger_delete_file(const char *name)
{
    char path[64] = {0};

    if (!adc_logger_is_valid_file_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_logger_lock();
    if (adc_logger_name_is_active_locked(name)) {
        adc_logger_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    adc_logger_unlock();

    ESP_RETURN_ON_ERROR(adc_logger_build_path(name, path, sizeof(path)), TAG, "delete path failed");
    if (unlink(path) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    adc_logger_lock();
    adc_logger_update_storage_usage_locked();
    adc_logger_set_last_error_locked("deleted %s", name);
    adc_logger_unlock();
    return ESP_OK;
}

esp_err_t adc_logger_open_file_for_read(const char *name, FILE **out_file, size_t *out_size)
{
    char path[64] = {0};
    struct stat st = {0};
    FILE *file = NULL;

    if ((out_file == NULL) || !adc_logger_is_valid_file_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_logger_lock();
    if (adc_logger_recording_or_writer_open_locked()) {
        adc_logger_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    adc_logger_unlock();

    ESP_RETURN_ON_ERROR(adc_logger_build_path(name, path, sizeof(path)), TAG, "read path failed");

    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    *out_file = file;
    if (out_size != NULL) {
        *out_size = (size_t) st.st_size;
    }
    return ESP_OK;
}

void adc_logger_close_file(FILE *file)
{
    if (file != NULL) {
        fclose(file);
    }
}
