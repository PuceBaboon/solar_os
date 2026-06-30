#include "solar_os_scp.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "libssh2.h"
#include "solar_os_ssh_transport.h"
#include "solar_os_task.h"

#define SOLAR_OS_SCP_DEFAULT_PORT 22
#define SOLAR_OS_SCP_TASK_STACK 24576
#define SOLAR_OS_SCP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define SOLAR_OS_SCP_EVENT_QUEUE_LEN 16
#define SOLAR_OS_SCP_BUFFER_SIZE 2048
#define SOLAR_OS_SCP_PROGRESS_STEP 4096

struct solar_os_scp_session {
    solar_os_scp_direction_t direction;
    char host[SOLAR_OS_SSH_HOST_MAX];
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char password[SOLAR_OS_SSH_PASSWORD_MAX];
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    char remote_path[SOLAR_OS_STORAGE_PATH_MAX];
    uint16_t port;
    QueueHandle_t events;
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool task_done;
    volatile bool detached;
};

static const char *TAG = "solar_os_scp";

static bool scp_should_stop(const solar_os_scp_session_t *session);
static void scp_send_status(solar_os_scp_session_t *session, const char *message);
static void scp_send_error(solar_os_scp_session_t *session, const char *message);

static const char *scp_direction_name(solar_os_scp_direction_t direction)
{
    return direction == SOLAR_OS_SCP_UPLOAD ? "upload" : "download";
}

static bool scp_transport_should_stop(void *user)
{
    return scp_should_stop((const solar_os_scp_session_t *)user);
}

static void scp_transport_status(void *user, const char *message)
{
    scp_send_status((solar_os_scp_session_t *)user, message);
}

static void scp_transport_error(void *user, const char *message)
{
    scp_send_error((solar_os_scp_session_t *)user, message);
}

static solar_os_ssh_transport_config_t scp_transport_config(solar_os_scp_session_t *session)
{
    return (solar_os_ssh_transport_config_t){
        .host = session->host,
        .port = session->port,
        .username = session->username,
        .password = session->password,
        .log_tag = TAG,
        .user = session,
        .should_stop = scp_transport_should_stop,
        .status = scp_transport_status,
        .error = scp_transport_error,
        .include_username_in_auth_status = true,
        .report_password_success = true,
        .report_publickey_success = true,
        .include_error_code = true,
        .log_resolve = true,
        .log_connect_fail = true,
        .log_handshake_complete = true,
        .log_host_key_match = true,
        .log_key_paths = true,
        .allow_unverified_host_key_without_storage = true,
    };
}

static void scp_send_event(solar_os_scp_session_t *session,
                           solar_os_scp_event_type_t type,
                           const char *message,
                           uint64_t transferred,
                           uint64_t total)
{
    if (session == NULL || session->events == NULL) {
        return;
    }

    solar_os_scp_event_t event = {
        .type = type,
        .transferred = transferred,
        .total = total,
    };
    if (message != NULL) {
        strlcpy(event.message, message, sizeof(event.message));
    }

    if (type == SOLAR_OS_SCP_EVENT_STATUS && event.message[0] != '\0') {
        SOLAR_OS_LOGI(TAG, "%s", event.message);
    } else if (type == SOLAR_OS_SCP_EVENT_ERROR && event.message[0] != '\0') {
        SOLAR_OS_LOGE(TAG, "%s", event.message);
    } else if (type == SOLAR_OS_SCP_EVENT_DONE && event.message[0] != '\0') {
        SOLAR_OS_LOGI(TAG, "done: %s", event.message);
    }

    (void)xQueueSend(session->events, &event, pdMS_TO_TICKS(50));
}

static void scp_send_status(solar_os_scp_session_t *session, const char *message)
{
    scp_send_event(session, SOLAR_OS_SCP_EVENT_STATUS, message, 0, 0);
}

static void scp_send_progress(solar_os_scp_session_t *session,
                              uint64_t transferred,
                              uint64_t total)
{
    scp_send_event(session, SOLAR_OS_SCP_EVENT_PROGRESS, NULL, transferred, total);
}

static void scp_send_error(solar_os_scp_session_t *session, const char *message)
{
    scp_send_event(session, SOLAR_OS_SCP_EVENT_ERROR, message, 0, 0);
}

static void scp_send_libssh2_error(solar_os_scp_session_t *session,
                                   LIBSSH2_SESSION *lib_session,
                                   const char *prefix,
                                   int code)
{
    solar_os_ssh_transport_config_t config = scp_transport_config(session);
    solar_os_ssh_transport_send_libssh2_error(&config, lib_session, prefix, code);
}

static bool scp_should_stop(const solar_os_scp_session_t *session)
{
    return session == NULL || session->stop_requested;
}

static void scp_session_destroy(solar_os_scp_session_t *session)
{
    if (session == NULL) {
        return;
    }

    if (session->events != NULL) {
        vQueueDelete(session->events);
        session->events = NULL;
    }
    memset(session->password, 0, sizeof(session->password));
    heap_caps_free(session);
}

static int scp_wait_socket(solar_os_scp_session_t *session,
                           int socket_fd,
                           LIBSSH2_SESSION *lib_session)
{
    solar_os_ssh_transport_config_t config = scp_transport_config(session);
    return solar_os_ssh_transport_wait_socket(&config, socket_fd, lib_session);
}

static LIBSSH2_CHANNEL *scp_send_channel_open(solar_os_scp_session_t *session,
                                              LIBSSH2_SESSION *lib_session,
                                              int socket_fd,
                                              int mode,
                                              uint64_t size)
{
    LIBSSH2_CHANNEL *channel = NULL;
    scp_send_status(session, "opening remote file");

    while (!scp_should_stop(session)) {
        channel = libssh2_scp_send64(lib_session,
                                     session->remote_path,
                                     mode & 0777,
                                     (libssh2_int64_t)size,
                                     0,
                                     0);
        if (channel != NULL) {
            return channel;
        }
        if (libssh2_session_last_errno(lib_session) != LIBSSH2_ERROR_EAGAIN) {
            scp_send_libssh2_error(session, lib_session, "remote open failed", -1);
            return NULL;
        }
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }

    return NULL;
}

static LIBSSH2_CHANNEL *scp_recv_channel_open(solar_os_scp_session_t *session,
                                              LIBSSH2_SESSION *lib_session,
                                              int socket_fd,
                                              libssh2_struct_stat *fileinfo)
{
    LIBSSH2_CHANNEL *channel = NULL;
    scp_send_status(session, "opening remote file");

    while (!scp_should_stop(session)) {
        channel = libssh2_scp_recv2(lib_session, session->remote_path, fileinfo);
        if (channel != NULL) {
            return channel;
        }
        if (libssh2_session_last_errno(lib_session) != LIBSSH2_ERROR_EAGAIN) {
            scp_send_libssh2_error(session, lib_session, "remote open failed", -1);
            return NULL;
        }
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }

    return NULL;
}

static bool scp_channel_write_all(solar_os_scp_session_t *session,
                                  LIBSSH2_SESSION *lib_session,
                                  LIBSSH2_CHANNEL *channel,
                                  int socket_fd,
                                  const char *data,
                                  size_t len)
{
    size_t offset = 0;
    while (offset < len && !scp_should_stop(session)) {
        const ssize_t written = libssh2_channel_write(channel, data + offset, len - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written == LIBSSH2_ERROR_EAGAIN) {
            (void)scp_wait_socket(session, socket_fd, lib_session);
            continue;
        }

        scp_send_libssh2_error(session, lib_session, "channel write failed", (int)written);
        return false;
    }

    return !scp_should_stop(session);
}

static void scp_close_channel(solar_os_scp_session_t *session,
                              LIBSSH2_SESSION *lib_session,
                              LIBSSH2_CHANNEL *channel,
                              int socket_fd)
{
    if (channel == NULL) {
        return;
    }

    int rc;
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_send_eof(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_wait_eof(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_wait_closed(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }

    (void)libssh2_channel_free(channel);
}

static char *scp_alloc_buffer(void)
{
    char *buffer = heap_caps_malloc(SOLAR_OS_SCP_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(SOLAR_OS_SCP_BUFFER_SIZE, MALLOC_CAP_8BIT);
    }
    return buffer;
}

static esp_err_t scp_upload(solar_os_scp_session_t *session,
                            LIBSSH2_SESSION *lib_session,
                            int socket_fd)
{
    struct stat st;
    if (stat(session->local_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        SOLAR_OS_LOGE(TAG, "local file unavailable: %s errno=%d", session->local_path, errno);
        scp_send_error(session, "local file unavailable");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *file = fopen(session->local_path, "rb");
    if (file == NULL) {
        SOLAR_OS_LOGE(TAG, "local file open failed: %s errno=%d", session->local_path, errno);
        scp_send_error(session, "local file open failed");
        return ESP_FAIL;
    }

    const uint64_t total = st.st_size > 0 ? (uint64_t)st.st_size : 0ULL;
    SOLAR_OS_LOGI(TAG,
             "upload open local=%s remote=%s bytes=%" PRIu64,
             session->local_path,
             session->remote_path,
             total);
    LIBSSH2_CHANNEL *channel =
        scp_send_channel_open(session, lib_session, socket_fd, st.st_mode, total);
    if (channel == NULL) {
        fclose(file);
        return ESP_FAIL;
    }

    char *buffer = scp_alloc_buffer();
    if (buffer == NULL) {
        scp_close_channel(session, lib_session, channel, socket_fd);
        fclose(file);
        scp_send_error(session, "transfer buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    scp_send_status(session, "uploading");
    uint64_t transferred = 0;
    uint64_t next_progress = 0;
    esp_err_t ret = ESP_OK;

    while (!scp_should_stop(session)) {
        const size_t read_len = fread(buffer, 1, SOLAR_OS_SCP_BUFFER_SIZE, file);
        if (read_len > 0) {
            if (!scp_channel_write_all(session, lib_session, channel, socket_fd, buffer, read_len)) {
                ret = ESP_FAIL;
                break;
            }
            transferred += read_len;
            if (transferred >= next_progress || transferred == total) {
                scp_send_progress(session, transferred, total);
                next_progress = transferred + SOLAR_OS_SCP_PROGRESS_STEP;
            }
        }

        if (read_len < SOLAR_OS_SCP_BUFFER_SIZE) {
            if (ferror(file)) {
                scp_send_error(session, "local file read failed");
                ret = ESP_FAIL;
            }
            break;
        }
    }

    heap_caps_free(buffer);
    fclose(file);
    scp_close_channel(session, lib_session, channel, socket_fd);
    return scp_should_stop(session) ? ESP_ERR_INVALID_STATE : ret;
}

static esp_err_t scp_download(solar_os_scp_session_t *session,
                              LIBSSH2_SESSION *lib_session,
                              int socket_fd)
{
    libssh2_struct_stat fileinfo = {0};
    LIBSSH2_CHANNEL *channel =
        scp_recv_channel_open(session, lib_session, socket_fd, &fileinfo);
    if (channel == NULL) {
        return ESP_FAIL;
    }

    FILE *file = fopen(session->local_path, "wb");
    if (file == NULL) {
        SOLAR_OS_LOGE(TAG, "local file create failed: %s errno=%d", session->local_path, errno);
        scp_close_channel(session, lib_session, channel, socket_fd);
        scp_send_error(session, "local file create failed");
        return ESP_FAIL;
    }

    char *buffer = scp_alloc_buffer();
    if (buffer == NULL) {
        fclose(file);
        scp_close_channel(session, lib_session, channel, socket_fd);
        scp_send_error(session, "transfer buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    const uint64_t total = fileinfo.st_size > 0 ? (uint64_t)fileinfo.st_size : 0ULL;
    SOLAR_OS_LOGI(TAG,
             "download open remote=%s local=%s bytes=%" PRIu64,
             session->remote_path,
             session->local_path,
             total);
    scp_send_status(session, "downloading");
    uint64_t transferred = 0;
    uint64_t next_progress = 0;
    esp_err_t ret = ESP_OK;

    while (!scp_should_stop(session) && transferred < total) {
        size_t want = SOLAR_OS_SCP_BUFFER_SIZE;
        if (total - transferred < want) {
            want = (size_t)(total - transferred);
        }

        const ssize_t read_len = libssh2_channel_read(channel, buffer, want);
        if (read_len > 0) {
            if (fwrite(buffer, 1, (size_t)read_len, file) != (size_t)read_len) {
                scp_send_error(session, "local file write failed");
                ret = ESP_FAIL;
                break;
            }
            transferred += (uint64_t)read_len;
            if (transferred >= next_progress || transferred == total) {
                scp_send_progress(session, transferred, total);
                next_progress = transferred + SOLAR_OS_SCP_PROGRESS_STEP;
            }
            continue;
        }

        if (read_len == LIBSSH2_ERROR_EAGAIN) {
            (void)scp_wait_socket(session, socket_fd, lib_session);
            continue;
        }

        scp_send_libssh2_error(session, lib_session, "channel read failed", (int)read_len);
        ret = ESP_FAIL;
        break;
    }

    if (fclose(file) != 0 && ret == ESP_OK) {
        scp_send_error(session, "local file close failed");
        ret = ESP_FAIL;
    }
    heap_caps_free(buffer);
    scp_close_channel(session, lib_session, channel, socket_fd);

    if (ret != ESP_OK || scp_should_stop(session)) {
        (void)unlink(session->local_path);
    }
    return scp_should_stop(session) ? ESP_ERR_INVALID_STATE : ret;
}

static void scp_session_task(void *arg)
{
    solar_os_scp_session_t *session = (solar_os_scp_session_t *)arg;
    solar_os_ssh_transport_t transport = {
        .socket_fd = -1,
    };
    solar_os_ssh_transport_config_t transport_config = scp_transport_config(session);
    int socket_fd = -1;
    LIBSSH2_SESSION *lib_session = NULL;
    bool ok = false;

    SOLAR_OS_LOGI(TAG,
             "task start: %s %s@%s:%" PRIu16 " local=%s remote=%s",
             scp_direction_name(session->direction),
             session->username,
             session->host,
             session->port,
             session->local_path,
             session->remote_path);

    if (solar_os_ssh_transport_open(&transport_config, &transport) != ESP_OK ||
        scp_should_stop(session)) {
        goto done;
    }
    socket_fd = transport.socket_fd;
    lib_session = transport.session;

    esp_err_t transfer_ret = session->direction == SOLAR_OS_SCP_UPLOAD ?
        scp_upload(session, lib_session, socket_fd) :
        scp_download(session, lib_session, socket_fd);
    ok = transfer_ret == ESP_OK;

done:
    solar_os_ssh_transport_close(&transport, "SolarOS scp shutdown");

    if (session != NULL) {
        SOLAR_OS_LOGI(TAG, "task complete: %s", ok ? "ok" : "failed");
        scp_send_event(session,
                       SOLAR_OS_SCP_EVENT_DONE,
                       ok ? "done" : "failed",
                       0,
                       0);
        const bool detached = session->detached;
        session->task_done = true;
        if (detached) {
            scp_session_destroy(session);
        }
    }

    SOLAR_OS_LOGI(TAG, "SCP task stopped");
    vTaskDelete(NULL);
}

esp_err_t solar_os_scp_start(const solar_os_scp_config_t *config,
                             solar_os_scp_session_t **session_out)
{
    if (config == NULL ||
        session_out == NULL ||
        config->host == NULL ||
        config->host[0] == '\0' ||
        config->username == NULL ||
        config->username[0] == '\0' ||
        config->local_path == NULL ||
        config->local_path[0] == '\0' ||
        config->remote_path == NULL ||
        config->remote_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_scp_session_t *session =
        heap_caps_calloc(1, sizeof(*session), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }

    session->direction = config->direction;
    strlcpy(session->host, config->host, sizeof(session->host));
    strlcpy(session->username, config->username, sizeof(session->username));
    strlcpy(session->password, config->password != NULL ? config->password : "", sizeof(session->password));
    strlcpy(session->local_path, config->local_path, sizeof(session->local_path));
    strlcpy(session->remote_path, config->remote_path, sizeof(session->remote_path));
    session->port = config->port > 0 ? config->port : SOLAR_OS_SCP_DEFAULT_PORT;

    SOLAR_OS_LOGI(TAG,
             "start request: %s %s@%s:%" PRIu16 " local=%s remote=%s password=%s",
             scp_direction_name(session->direction),
             session->username,
             session->host,
             session->port,
             session->local_path,
             session->remote_path,
             session->password[0] != '\0' ? "yes" : "no");

    session->events = xQueueCreate(SOLAR_OS_SCP_EVENT_QUEUE_LEN, sizeof(solar_os_scp_event_t));
    if (session->events == NULL) {
        solar_os_scp_stop(session);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(scp_session_task,
                                                 "solar_os_scp",
                                                 SOLAR_OS_SCP_TASK_STACK,
                                                 session,
                                                 SOLAR_OS_SCP_TASK_PRIORITY,
                                                 &session->task,
                                                 tskNO_AFFINITY);
    if (created != pdPASS) {
        solar_os_scp_stop(session);
        return ESP_ERR_NO_MEM;
    }

    *session_out = session;
    return ESP_OK;
}

bool solar_os_scp_stop(solar_os_scp_session_t *session)
{
    if (session == NULL) {
        return true;
    }

    session->stop_requested = true;
    if (!solar_os_task_wait_done(session->task,
                                 &session->task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        session->detached = true;
        SOLAR_OS_LOGW(TAG, "SCP task did not stop within %u ms; detached for worker cleanup",
                 (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return false;
    }

    scp_session_destroy(session);
    return true;
}

bool solar_os_scp_poll(solar_os_scp_session_t *session, solar_os_scp_event_t *event)
{
    if (session == NULL || event == NULL || session->events == NULL) {
        return false;
    }

    return xQueueReceive(session->events, event, 0) == pdTRUE;
}
