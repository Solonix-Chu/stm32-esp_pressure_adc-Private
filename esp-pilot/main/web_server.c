#include "web_server.h"

#include "adc_logger.h"
#include "wifi_ap.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

extern const char web_index_html_start[] asm("_binary_index_html_start");
extern const char web_index_html_end[] asm("_binary_index_html_end");
extern const char web_app_js_start[] asm("_binary_app_js_start");
extern const char web_app_js_end[] asm("_binary_app_js_end");
extern const char web_style_css_start[] asm("_binary_style_css_start");
extern const char web_style_css_end[] asm("_binary_style_css_end");

static const char *TAG = "web";
static httpd_handle_t s_httpd;

static esp_err_t web_send_embedded(httpd_req_t *req,
                                   const char *content_type,
                                   const char *start,
                                   const char *end)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, (end - start) - 1);
}

static esp_err_t web_send_json_message(httpd_req_t *req,
                                       const char *status_line,
                                       const char *result,
                                       const char *message)
{
    char body[192];
    const int length = snprintf(body,
                                sizeof(body),
                                "{\"result\":\"%s\",\"message\":\"%s\"}",
                                result,
                                message != NULL ? message : "");
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, length);
}

static esp_err_t web_parse_name_query(httpd_req_t *req, char *name, size_t name_size)
{
    char query[96] = {0};

    if ((name == NULL) || (name_size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (httpd_req_get_url_query_len(req) <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (httpd_query_key_value(query, "name", name, name_size) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!adc_logger_is_valid_file_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t web_handle_index(httpd_req_t *req)
{
    return web_send_embedded(req, "text/html; charset=utf-8", web_index_html_start, web_index_html_end);
}

static esp_err_t web_handle_app_js(httpd_req_t *req)
{
    return web_send_embedded(req, "application/javascript; charset=utf-8", web_app_js_start, web_app_js_end);
}

static esp_err_t web_handle_style_css(httpd_req_t *req)
{
    return web_send_embedded(req, "text/css; charset=utf-8", web_style_css_start, web_style_css_end);
}

static esp_err_t web_handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t web_handle_status(httpd_req_t *req)
{
    char body[1024];
    adc_logger_status_t status = {0};
    const int64_t now_us = esp_timer_get_time();

    adc_logger_get_status(&status);

    const int length = snprintf(
        body,
        sizeof(body),
        "{"
        "\"ap_ssid\":\"%s\","
        "\"ap_ip\":\"%s\","
        "\"storage_ready\":%s,"
        "\"spi_ready\":%s,"
        "\"recording_active\":%s,"
        "\"writer_open\":%s,"
        "\"active_file\":\"%s\","
        "\"last_error\":\"%s\","
        "\"packets_received\":%llu,"
        "\"packets_valid\":%llu,"
        "\"packets_invalid\":%llu,"
        "\"packets_enqueued\":%llu,"
        "\"packets_written\":%llu,"
        "\"bytes_written\":%llu,"
        "\"spi_errors\":%" PRIu32 ","
        "\"checksum_errors\":%" PRIu32 ","
        "\"header_errors\":%" PRIu32 ","
        "\"queue_overflows\":%" PRIu32 ","
        "\"sequence_gap_events\":%" PRIu32 ","
        "\"last_sequence\":%" PRIu32 ","
        "\"last_tick_ms\":%" PRIu32 ","
        "\"last_sample_rate_hz\":%" PRIu32 ","
        "\"last_stm32_dropped_packets\":%" PRIu32 ","
        "\"last_flags\":%u,"
        "\"last_packet_age_ms\":%lld,"
        "\"storage_total_bytes\":%u,"
        "\"storage_used_bytes\":%u,"
        "\"queued_packets\":%" PRIu32 ","
        "\"free_packet_slots\":%" PRIu32
        "}",
        wifi_ap_get_ssid(),
        wifi_ap_get_ip(),
        status.storage_ready ? "true" : "false",
        status.spi_ready ? "true" : "false",
        status.recording_active ? "true" : "false",
        status.writer_open ? "true" : "false",
        status.active_file,
        status.last_error,
        (unsigned long long) status.packets_received,
        (unsigned long long) status.packets_valid,
        (unsigned long long) status.packets_invalid,
        (unsigned long long) status.packets_enqueued,
        (unsigned long long) status.packets_written,
        (unsigned long long) status.bytes_written,
        status.spi_errors,
        status.checksum_errors,
        status.header_errors,
        status.queue_overflows,
        status.sequence_gap_events,
        status.last_sequence,
        status.last_tick_ms,
        status.last_sample_rate_hz,
        status.last_stm32_dropped_packets,
        status.last_flags,
        (long long) ((status.last_packet_time_us > 0) ? ((now_us - status.last_packet_time_us) / 1000) : -1),
        (unsigned int) status.storage_total_bytes,
        (unsigned int) status.storage_used_bytes,
        status.queued_packets,
        status.free_packet_slots);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, length);
}

static esp_err_t web_handle_files(httpd_req_t *req)
{
    adc_logger_file_list_t *list = calloc(1, sizeof(*list));
    if (list == NULL) {
        return web_send_json_message(req, "500 Internal Server Error", "error", "out of memory");
    }

    const esp_err_t list_err = adc_logger_list_files(list);
    if (list_err != ESP_OK) {
        free(list);
        ESP_RETURN_ON_ERROR(list_err, TAG, "file list failed");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr_chunk(req, "{\"files\":[");
    if (send_err != ESP_OK) {
        free(list);
        ESP_RETURN_ON_ERROR(send_err, TAG, "send files header failed");
    }

    for (size_t i = 0; i < list->count; ++i) {
        char chunk[160];
        const adc_logger_file_info_t *item = &list->items[i];
        const int length = snprintf(chunk,
                                    sizeof(chunk),
                                    "%s{\"name\":\"%s\",\"size_bytes\":%u,\"modified_time\":%lld}",
                                    (i == 0U) ? "" : ",",
                                    item->name,
                                    (unsigned int) item->size_bytes,
                                    (long long) item->modified_time);
        send_err = httpd_resp_send_chunk(req, chunk, length);
        if (send_err != ESP_OK) {
            free(list);
            ESP_RETURN_ON_ERROR(send_err, TAG, "send file item failed");
        }
    }

    char tail[48];
    const int tail_len = snprintf(tail,
                                  sizeof(tail),
                                  "],\"truncated\":%s}",
                                  list->truncated ? "true" : "false");
    send_err = httpd_resp_send_chunk(req, tail, tail_len);
    if (send_err != ESP_OK) {
        free(list);
        ESP_RETURN_ON_ERROR(send_err, TAG, "send files tail failed");
    }

    free(list);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t web_handle_record_start(httpd_req_t *req)
{
    const esp_err_t err = adc_logger_start_recording();
    if (err == ESP_OK) {
        return web_send_json_message(req, "200 OK", "ok", "recording started");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_json_message(req, "409 Conflict", "error", "recording already active");
    }
    return web_send_json_message(req, "500 Internal Server Error", "error", "failed to start recording");
}

static esp_err_t web_handle_record_stop(httpd_req_t *req)
{
    const esp_err_t err = adc_logger_stop_recording();
    if (err == ESP_OK) {
        return web_send_json_message(req, "200 OK", "ok", "recording stopped");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_json_message(req, "409 Conflict", "error", "recording is not active");
    }
    return web_send_json_message(req, "500 Internal Server Error", "error", "failed to stop recording");
}

static esp_err_t web_handle_download(httpd_req_t *req)
{
    char name[ADC_LOGGER_MAX_FILE_NAME_LEN] = {0};
    char header[96];
    FILE *file = NULL;
    size_t size = 0;
    char buffer[1024];

    if (web_parse_name_query(req, name, sizeof(name)) != ESP_OK) {
        return web_send_json_message(req, "400 Bad Request", "error", "missing or invalid file name");
    }

    const esp_err_t err = adc_logger_open_file_for_read(name, &file, &size);
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_json_message(req, "409 Conflict", "error", "stop recording before downloading");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_json_message(req, "404 Not Found", "error", "file not found");
    }
    if (err != ESP_OK) {
        return web_send_json_message(req, "500 Internal Server Error", "error", "failed to open file");
    }

    snprintf(header, sizeof(header), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", header);
    ESP_LOGI(TAG, "downloading %s (%u bytes)", name, (unsigned int) size);

    while (!feof(file)) {
        const size_t read_bytes = fread(buffer, 1, sizeof(buffer), file);
        if (read_bytes == 0U) {
            break;
        }
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            adc_logger_close_file(file);
            return ESP_FAIL;
        }
    }

    adc_logger_close_file(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t web_handle_delete(httpd_req_t *req)
{
    char name[ADC_LOGGER_MAX_FILE_NAME_LEN] = {0};

    if (web_parse_name_query(req, name, sizeof(name)) != ESP_OK) {
        return web_send_json_message(req, "400 Bad Request", "error", "missing or invalid file name");
    }

    const esp_err_t err = adc_logger_delete_file(name);
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_json_message(req, "409 Conflict", "error", "active recording file cannot be deleted");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_json_message(req, "404 Not Found", "error", "file not found");
    }
    if (err != ESP_OK) {
        return web_send_json_message(req, "500 Internal Server Error", "error", "delete failed");
    }

    return web_send_json_message(req, "200 OK", "ok", "file deleted");
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (s_httpd != NULL) {
        return ESP_OK;
    }

    config.max_uri_handlers = 10;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &config), TAG, "http server start failed");

    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = web_handle_index},
        {.uri = "/app.js", .method = HTTP_GET, .handler = web_handle_app_js},
        {.uri = "/style.css", .method = HTTP_GET, .handler = web_handle_style_css},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = web_handle_favicon},
        {.uri = "/api/status", .method = HTTP_GET, .handler = web_handle_status},
        {.uri = "/api/files", .method = HTTP_GET, .handler = web_handle_files},
        {.uri = "/api/files", .method = HTTP_DELETE, .handler = web_handle_delete},
        {.uri = "/api/files/download", .method = HTTP_GET, .handler = web_handle_download},
        {.uri = "/api/recording/start", .method = HTTP_POST, .handler = web_handle_record_start},
        {.uri = "/api/recording/stop", .method = HTTP_POST, .handler = web_handle_record_stop},
    };

    for (size_t i = 0; i < (sizeof(uris) / sizeof(uris[0])); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &uris[i]), TAG, "register uri failed");
    }

    ESP_LOGI(TAG, "web ui ready at http://%s", wifi_ap_get_ip());
    return ESP_OK;
}
