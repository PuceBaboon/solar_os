#include "solar_os_daq_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"

#define DAQ_DEFAULT_SCALAR_INTERVAL_MS 1000U
#define DAQ_DEFAULT_BYTE_INTERVAL_MS 25U
#define DAQ_MAX_INTERVAL_MS 86400000U
#define DAQ_RAW_READ_MAX 512U

static const char *TAG = "solar_os_daq";

typedef struct {
    bool running;
    FILE *file;
    solar_os_stream_handle_t stream;
    solar_os_stream_info_t info;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char last_change_key[SOLAR_OS_STREAM_CHANGE_KEY_MAX];
    uint32_t interval_ms;
    uint32_t next_sample_ms;
    uint32_t written_records;
    uint64_t written_bytes;
    uint32_t skipped_records;
    uint32_t failed_records;
    bool change_only;
    bool append;
    bool raw;
    esp_err_t last_error;
} daq_job_state_t;

static daq_job_state_t daq = {
    .stream = SOLAR_OS_STREAM_HANDLE_INIT,
    .last_error = ESP_OK,
};

static bool daq_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        parsed < min ||
        parsed > max) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static esp_err_t daq_flush_to_disk(FILE *file)
{
    if (file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fflush(file) != 0) {
        return ESP_FAIL;
    }

    const int fd = fileno(file);
    if (fd < 0) {
        return ESP_FAIL;
    }
    return fsync(fd) == 0 ? ESP_OK : ESP_FAIL;
}

static void daq_cleanup(void)
{
    if (daq.file != NULL) {
        (void)daq_flush_to_disk(daq.file);
        fclose(daq.file);
        daq.file = NULL;
    }
    solar_os_stream_close(&daq.stream);
    daq.running = false;
    daq.next_sample_ms = 0;
}

static bool daq_parse_args(int argc,
                           char **argv,
                           solar_os_stream_info_t *info,
                           char *path,
                           size_t path_len,
                           uint32_t *interval_ms,
                           bool *change_only,
                           bool *append,
                           bool *raw)
{
    if (argc < 3 ||
        argv == NULL ||
        argv[1] == NULL ||
        argv[2] == NULL ||
        info == NULL ||
        path == NULL ||
        interval_ms == NULL ||
        change_only == NULL ||
        append == NULL ||
        raw == NULL) {
        return false;
    }

    if (solar_os_stream_get_info(argv[1], info) != ESP_OK) {
        return false;
    }
    if (solar_os_storage_resolve_path(argv[2], path, path_len) != ESP_OK) {
        return false;
    }

    *interval_ms =
        info->type == SOLAR_OS_STREAM_TYPE_BYTES ?
        DAQ_DEFAULT_BYTE_INTERVAL_MS :
        DAQ_DEFAULT_SCALAR_INTERVAL_MS;
    *change_only = false;
    *append = true;
    *raw = false;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            *raw = true;
            continue;
        }
        if (strcmp(argv[i], "--changes") == 0) {
            *change_only = true;
            continue;
        }
        if (strcmp(argv[i], "--append") == 0) {
            *append = true;
            continue;
        }
        if (strcmp(argv[i], "--replace") == 0) {
            *append = false;
            continue;
        }
        if (strcmp(argv[i], "--rate-ms") == 0) {
            if (i + 1 >= argc ||
                !daq_parse_u32(argv[++i], 0, DAQ_MAX_INTERVAL_MS, interval_ms)) {
                return false;
            }
            continue;
        }
        if (strcmp(argv[i], "--rate") == 0) {
            uint32_t seconds = 0;
            if (i + 1 >= argc || !daq_parse_u32(argv[++i], 1, 86400, &seconds)) {
                return false;
            }
            *interval_ms = seconds * 1000U;
            continue;
        }
        return false;
    }

    if (*raw && *change_only) {
        return false;
    }

    return true;
}

static esp_err_t daq_write_header_if_needed(FILE *file,
                                            const solar_os_stream_info_t *info,
                                            bool append)
{
    if (file == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool write_header = !append;
    if (append) {
        if (fseek(file, 0, SEEK_END) != 0) {
            return ESP_FAIL;
        }
        const long pos = ftell(file);
        if (pos < 0) {
            return ESP_FAIL;
        }
        write_header = pos == 0;
    }

    if (!write_header) {
        return ESP_OK;
    }

    char header[SOLAR_OS_STREAM_CSV_HEADER_MAX];
    esp_err_t err = solar_os_stream_csv_header(info, header, sizeof(header));
    if (err != ESP_OK) {
        return err;
    }

    if (fprintf(file, "%s\n", header) < 0) {
        return ESP_FAIL;
    }
    return daq_flush_to_disk(file);
}

static esp_err_t daq_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    solar_os_stream_info_t info;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    uint32_t interval_ms = 0;
    bool change_only = false;
    bool append = true;
    bool raw = false;

    if (!daq_parse_args(argc,
                        argv,
                        &info,
                        path,
                        sizeof(path),
                        &interval_ms,
                        &change_only,
                        &append,
                        &raw)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (raw && info.type != SOLAR_OS_STREAM_TYPE_BYTES) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (daq.running || daq.file != NULL || solar_os_port_handle_valid(&daq.stream.port)) {
        daq_cleanup();
    }

    FILE *file = fopen(path, raw ? (append ? "ab" : "wb") : (append ? "a+" : "w"));
    if (file == NULL) {
        daq.last_error = ESP_FAIL;
        return ESP_FAIL;
    }

    esp_err_t err = solar_os_stream_open(info.id, "daq", &daq.stream);
    if (err != ESP_OK) {
        fclose(file);
        daq.last_error = err;
        return err;
    }

    if (!raw) {
        err = daq_write_header_if_needed(file, &info, append);
        if (err != ESP_OK) {
            fclose(file);
            solar_os_stream_close(&daq.stream);
            daq.last_error = err;
            return err;
        }
    }

    memset(&daq.info, 0, sizeof(daq.info));
    daq.info = info;
    daq.file = file;
    strlcpy(daq.path, path, sizeof(daq.path));
    daq.last_change_key[0] = '\0';
    daq.interval_ms = interval_ms;
    daq.next_sample_ms = 0;
    daq.written_records = 0;
    daq.written_bytes = 0;
    daq.skipped_records = 0;
    daq.failed_records = 0;
    daq.change_only = change_only;
    daq.append = append;
    daq.raw = raw;
    daq.last_error = ESP_OK;
    daq.running = true;

    SOLAR_OS_LOGI(TAG,
                  "started: %s -> %s interval=%" PRIu32 "ms%s%s",
                  daq.info.id,
                  daq.path,
                  daq.interval_ms,
                  daq.raw ? " raw" : "",
                  daq.change_only ? " changes" : "");
    return ESP_OK;
}

static void daq_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    SOLAR_OS_LOGI(TAG,
                  "stopped: %s records=%" PRIu32 " bytes=%" PRIu64
                  " skipped=%" PRIu32 " failed=%" PRIu32,
                  daq.info.id,
                  daq.written_records,
                  daq.written_bytes,
                  daq.skipped_records,
                  daq.failed_records);
    daq_cleanup();
}

static bool daq_should_sample(uint32_t now_ms)
{
    if (!daq.running) {
        return false;
    }
    if (daq.interval_ms == 0) {
        return true;
    }
    if (daq.next_sample_ms != 0 && (int32_t)(now_ms - daq.next_sample_ms) < 0) {
        return false;
    }

    daq.next_sample_ms = now_ms + daq.interval_ms;
    return true;
}

static bool daq_event_raw(void)
{
    if (!solar_os_port_handle_valid(&daq.stream.port)) {
        daq.failed_records++;
        daq.last_error = ESP_ERR_INVALID_STATE;
        return true;
    }

    uint8_t data[DAQ_RAW_READ_MAX];
    size_t read_len = 0;
    const esp_err_t err = solar_os_port_read(&daq.stream.port,
                                             data,
                                             sizeof(data),
                                             0,
                                             &read_len);
    if (err == ESP_ERR_TIMEOUT) {
        daq.skipped_records++;
        daq.last_error = ESP_OK;
        return true;
    }
    if (err != ESP_OK) {
        daq.failed_records++;
        daq.last_error = err;
        SOLAR_OS_LOGW(TAG, "raw read failed: %s", esp_err_to_name(err));
        return true;
    }
    if (read_len == 0) {
        daq.skipped_records++;
        daq.last_error = ESP_OK;
        return true;
    }

    if (fwrite(data, 1, read_len, daq.file) != read_len ||
        daq_flush_to_disk(daq.file) != ESP_OK) {
        daq.failed_records++;
        daq.last_error = ESP_FAIL;
        SOLAR_OS_LOGW(TAG, "raw write failed");
        return true;
    }

    daq.written_records++;
    daq.written_bytes += read_len;
    daq.last_error = ESP_OK;
    return true;
}

static bool daq_event_csv(void)
{
    solar_os_stream_csv_record_t record;
    const solar_os_stream_read_options_t options = {
        .window_ms = daq.info.id[0] == 'm' ? 100U : 0U,
        .timeout_ms = 0,
    };
    const esp_err_t err = solar_os_stream_read_csv(&daq.stream, &options, &record);
    if (err == ESP_ERR_TIMEOUT && !record.has_data) {
        daq.skipped_records++;
        daq.last_error = ESP_OK;
        return true;
    }
    if (err != ESP_OK) {
        daq.failed_records++;
        daq.last_error = err;
        SOLAR_OS_LOGW(TAG, "sample failed: %s", esp_err_to_name(err));
        return true;
    }

    if (daq.change_only &&
        daq.last_change_key[0] != '\0' &&
        strcmp(daq.last_change_key, record.change_key) == 0) {
        daq.skipped_records++;
        daq.last_error = ESP_OK;
        return true;
    }

    if (fprintf(daq.file, "%s\n", record.line) < 0 ||
        daq_flush_to_disk(daq.file) != ESP_OK) {
        daq.failed_records++;
        daq.last_error = ESP_FAIL;
        SOLAR_OS_LOGW(TAG, "write failed");
        return true;
    }

    strlcpy(daq.last_change_key, record.change_key, sizeof(daq.last_change_key));
    daq.written_records++;
    daq.last_error = ESP_OK;
    return true;
}

static bool daq_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL || event->type != SOLAR_OS_EVENT_TICK ||
        !daq_should_sample(event->data.tick_ms)) {
        return false;
    }

    return daq.raw ? daq_event_raw() : daq_event_csv();
}

void solar_os_daq_job_get_status(solar_os_daq_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->running = daq.running;
    strlcpy(status->stream_id, daq.info.id, sizeof(status->stream_id));
    strlcpy(status->path, daq.path, sizeof(status->path));
    status->stream_type = daq.info.type;
    status->interval_ms = daq.interval_ms;
    status->written_records = daq.written_records;
    status->written_bytes = daq.written_bytes;
    status->skipped_records = daq.skipped_records;
    status->failed_records = daq.failed_records;
    status->change_only = daq.change_only;
    status->append = daq.append;
    status->raw = daq.raw;
    status->last_error = daq.last_error;
}

const solar_os_job_t solar_os_daq_job = {
    .name = "daq",
    .summary = "capture data streams to CSV or raw files",
    .start = daq_start,
    .stop = daq_stop,
    .event = daq_event,
};
