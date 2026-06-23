#include "solar_os_ota.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "solar_os_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "solar_os_config.h"

#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_URL_KEY "url"
#define OTA_NVS_FLAVOR_KEY "flavor"
#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_MANIFEST_MAX 512

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#ifndef SOLAR_OS_FLAVOR_NAME
#define SOLAR_OS_FLAVOR_NAME "full"
#endif

static char ota_url[SOLAR_OS_OTA_URL_MAX] = SOLAR_OS_OTA_DEFAULT_URL;
static char ota_target_flavor[SOLAR_OS_OTA_FLAVOR_MAX] = SOLAR_OS_FLAVOR_NAME;
static bool ota_loaded;
static const char *TAG = "solar_os_ota";

typedef struct {
    char *body;
    size_t body_len;
    size_t body_cap;
    bool truncated;
} ota_http_body_t;

static void ota_load(void);

static bool ota_url_is_valid(const char *url)
{
    if (url == NULL || url[0] == '\0' || strlen(url) >= SOLAR_OS_OTA_URL_MAX) {
        return false;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)url; *p != '\0'; p++) {
        if (!isprint(*p) || isspace(*p)) {
            return false;
        }
    }
    return true;
}

static bool ota_flavor_is_valid(const char *flavor)
{
    if (flavor == NULL || flavor[0] == '\0' || strlen(flavor) >= SOLAR_OS_OTA_FLAVOR_MAX) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)flavor; *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') {
            return false;
        }
    }
    return true;
}

static bool ota_url_has_suffix(const char *url, const char *suffix)
{
    if (url == NULL || suffix == NULL) {
        return false;
    }
    const size_t url_len = strlen(url);
    const size_t suffix_len = strlen(suffix);
    return url_len >= suffix_len && strcmp(url + url_len - suffix_len, suffix) == 0;
}

static bool ota_url_has_flavor_suffix(const char *url, const char *flavor)
{
    if (url == NULL || flavor == NULL || flavor[0] == '\0') {
        return false;
    }

    size_t url_len = strlen(url);
    while (url_len > 0 && url[url_len - 1] == '/') {
        url_len--;
    }

    const size_t flavor_len = strlen(flavor);
    if (url_len < flavor_len ||
        memcmp(url + url_len - flavor_len, flavor, flavor_len) != 0) {
        return false;
    }

    return url_len == flavor_len || url[url_len - flavor_len - 1] == '/';
}

static esp_err_t ota_build_artifact_url(const char *leaf, char *out, size_t out_len)
{
    if (leaf == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_load();

    const bool firmware_leaf = strcmp(leaf, SOLAR_OS_OTA_FIRMWARE_FILE) == 0;
    if (ota_url_has_suffix(ota_url, ".bin")) {
        if (firmware_leaf) {
            if (strlcpy(out, ota_url, out_len) >= out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            return ESP_OK;
        }

        const char *slash = strrchr(ota_url, '/');
        const size_t prefix_len = slash != NULL ? (size_t)(slash - ota_url + 1) : 0;
        const int written = snprintf(out,
                                     out_len,
                                     "%.*s%s",
                                     (int)prefix_len,
                                     ota_url,
                                     leaf);
        return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    const size_t url_len = strlen(ota_url);
    const bool has_slash = url_len > 0 && ota_url[url_len - 1] == '/';
    const bool has_flavor = ota_url_has_flavor_suffix(ota_url, ota_target_flavor);
    const int written = has_flavor ?
        snprintf(out, out_len, "%s%s%s", ota_url, has_slash ? "" : "/", leaf) :
        snprintf(out,
                 out_len,
                 "%s%s%s/%s",
                 ota_url,
                 has_slash ? "" : "/",
                 ota_target_flavor,
                 leaf);
    return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void ota_load(void)
{
    if (ota_loaded) {
        return;
    }
    ota_loaded = true;

    strlcpy(ota_target_flavor, SOLAR_OS_FLAVOR_NAME, sizeof(ota_target_flavor));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return;
    }

    char stored_url[SOLAR_OS_OTA_URL_MAX];
    size_t url_len = sizeof(stored_url);
    ret = nvs_get_str(nvs, OTA_NVS_URL_KEY, stored_url, &url_len);
    if (ret == ESP_OK && ota_url_is_valid(stored_url)) {
        strlcpy(ota_url, stored_url, sizeof(ota_url));
    }

    char stored_flavor[SOLAR_OS_OTA_FLAVOR_MAX];
    size_t flavor_len = sizeof(stored_flavor);
    ret = nvs_get_str(nvs, OTA_NVS_FLAVOR_KEY, stored_flavor, &flavor_len);
    nvs_close(nvs);

    if (ret == ESP_OK && ota_flavor_is_valid(stored_flavor)) {
        strlcpy(ota_target_flavor, stored_flavor, sizeof(ota_target_flavor));
    }
}

static esp_err_t ota_save(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, OTA_NVS_URL_KEY, ota_url);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, OTA_NVS_FLAVOR_KEY, ota_target_flavor);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t ota_http_event(esp_http_client_event_t *event)
{
    if (event == NULL ||
        event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == NULL ||
        event->data_len <= 0) {
        return ESP_OK;
    }

    ota_http_body_t *body = (ota_http_body_t *)event->user_data;
    if (body == NULL || body->body == NULL || body->body_cap == 0) {
        return ESP_OK;
    }

    const size_t remaining = body->body_cap - body->body_len - 1;
    const size_t copy_len = (size_t)event->data_len > remaining ?
        remaining :
        (size_t)event->data_len;
    if (copy_len > 0) {
        memcpy(body->body + body->body_len, event->data, copy_len);
        body->body_len += copy_len;
        body->body[body->body_len] = '\0';
    }
    if (copy_len < (size_t)event->data_len) {
        body->truncated = true;
    }
    return ESP_OK;
}

static void ota_trim(char *text)
{
    if (text == NULL) {
        return;
    }

    char *start = text;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

static bool ota_parse_version_line(char *line, char *version, size_t version_len)
{
    if (line == NULL || version == NULL || version_len == 0) {
        return false;
    }

    char *comment = strchr(line, '#');
    if (comment != NULL) {
        *comment = '\0';
    }
    ota_trim(line);
    if (line[0] == '\0') {
        return false;
    }

    if (strncmp(line, "version", 7) == 0) {
        char *value = line + 7;
        while (*value != '\0' && isspace((unsigned char)*value)) {
            value++;
        }
        if (*value == '=' || *value == ':') {
            value++;
            ota_trim(value);
            line = value;
        }
    }

    char *end = line;
    while (*end != '\0' && !isspace((unsigned char)*end)) {
        end++;
    }
    *end = '\0';
    if (line[0] == '\0') {
        return false;
    }

    strlcpy(version, line, version_len);
    return true;
}

static esp_err_t ota_parse_version_manifest(char *manifest, char *version, size_t version_len)
{
    if (manifest == NULL || version == NULL || version_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    version[0] = '\0';

    char *saveptr = NULL;
    for (char *line = strtok_r(manifest, "\r\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\r\n", &saveptr)) {
        if (ota_parse_version_line(line, version, version_len)) {
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t ota_http_get_text(const char *url,
                                   char *body_buffer,
                                   size_t body_buffer_len,
                                   int *status_code,
                                   int64_t *content_length)
{
    if (url == NULL || body_buffer == NULL || body_buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_http_body_t body = {
        .body = body_buffer,
        .body_cap = body_buffer_len,
    };
    body_buffer[0] = '\0';
    if (status_code != NULL) {
        *status_code = -1;
    }
    if (content_length != NULL) {
        *content_length = -1;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .event_handler = ota_http_event,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "SolarOS-ota/0.1",
        .user_data = &body,
        .max_redirection_count = 3,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    SOLAR_OS_LOGI(TAG, "check %s", url);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    const int64_t http_len = esp_http_client_get_content_length(client);
    if (status_code != NULL) {
        *status_code = http_status;
    }
    if (content_length != NULL) {
        *content_length = http_len;
    }
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "check failed: %s", esp_err_to_name(err));
        return err;
    }
    if (http_status < 200 || http_status >= 300) {
        SOLAR_OS_LOGW(TAG, "check HTTP %d", http_status);
        return ESP_FAIL;
    }
    if (body.truncated) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static int ota_partition_slot(const esp_partition_t *partition)
{
    if (partition == NULL ||
        partition->type != ESP_PARTITION_TYPE_APP ||
        partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
        partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return -1;
    }

    return (int)partition->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_0;
}

static const char *ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
    default:
        return "undefined";
    }
}

static void ota_fill_partition(solar_os_ota_partition_t *info, const esp_partition_t *partition)
{
    if (info == NULL) {
        return;
    }

    memset(info, 0, sizeof(*info));
    info->slot = -1;
    strlcpy(info->state, "unknown", sizeof(info->state));

    if (partition == NULL) {
        return;
    }

    info->valid = true;
    info->slot = ota_partition_slot(partition);
    info->subtype = (uint8_t)partition->subtype;
    info->address = partition->address;
    info->size = partition->size;
    strlcpy(info->label, partition->label, sizeof(info->label));

    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(partition, &app_desc) == ESP_OK) {
        strlcpy(info->version, app_desc.version, sizeof(info->version));
        strlcpy(info->project_name, app_desc.project_name, sizeof(info->project_name));
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) == ESP_OK) {
        strlcpy(info->state, ota_state_name(state), sizeof(info->state));
    }
}

static const esp_partition_t *ota_find_slot(uint8_t slot)
{
    if (slot >= (uint8_t)(ESP_PARTITION_SUBTYPE_APP_OTA_MAX -
                          ESP_PARTITION_SUBTYPE_APP_OTA_0)) {
        return NULL;
    }

    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_OTA_0 + slot,
                                    NULL);
}

esp_err_t solar_os_ota_init(void)
{
    ota_load();
    return ESP_OK;
}

void solar_os_ota_get_url(char *url, size_t len)
{
    if (url == NULL || len == 0) {
        return;
    }

    ota_load();
    strlcpy(url, ota_url, len);
}

esp_err_t solar_os_ota_set_url(const char *url)
{
    ota_load();
    if (!ota_url_is_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(ota_url, url, sizeof(ota_url));
    return ota_save();
}

void solar_os_ota_get_flavor(char *flavor, size_t len)
{
    if (flavor == NULL || len == 0) {
        return;
    }

    ota_load();
    strlcpy(flavor, ota_target_flavor, len);
}

esp_err_t solar_os_ota_set_flavor(const char *flavor)
{
    ota_load();
    if (!ota_flavor_is_valid(flavor)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(ota_target_flavor, flavor, sizeof(ota_target_flavor));
    return ota_save();
}

esp_err_t solar_os_ota_get_artifact_urls(char *version_url,
                                         size_t version_url_len,
                                         char *firmware_url,
                                         size_t firmware_url_len)
{
    esp_err_t err = ESP_OK;

    if (version_url != NULL && version_url_len > 0) {
        err = ota_build_artifact_url(SOLAR_OS_OTA_VERSION_FILE, version_url, version_url_len);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (firmware_url != NULL && firmware_url_len > 0) {
        err = ota_build_artifact_url(SOLAR_OS_OTA_FIRMWARE_FILE, firmware_url, firmware_url_len);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_ota_get_status(solar_os_ota_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    solar_os_ota_get_url(status->url, sizeof(status->url));
    strlcpy(status->compiled_flavor, SOLAR_OS_FLAVOR_NAME, sizeof(status->compiled_flavor));
    solar_os_ota_get_flavor(status->target_flavor, sizeof(status->target_flavor));
    status->ota_partition_count = esp_ota_get_app_partition_count();

    ota_fill_partition(&status->running, esp_ota_get_running_partition());
    ota_fill_partition(&status->boot, esp_ota_get_boot_partition());
    ota_fill_partition(&status->next_update, esp_ota_get_next_update_partition(NULL));
    return ESP_OK;
}

esp_err_t solar_os_ota_set_boot_slot(uint8_t slot)
{
    const esp_partition_t *partition = ota_find_slot(slot);
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    return esp_ota_set_boot_partition(partition);
}

esp_err_t solar_os_ota_check(solar_os_ota_check_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    strlcpy(result->compiled_flavor, SOLAR_OS_FLAVOR_NAME, sizeof(result->compiled_flavor));
    solar_os_ota_get_flavor(result->target_flavor, sizeof(result->target_flavor));
    strlcpy(result->current_version, SOLAR_OS_VERSION, sizeof(result->current_version));
    result->status_code = -1;
    result->content_length = -1;

    esp_err_t err = solar_os_ota_get_artifact_urls(result->version_url,
                                                  sizeof(result->version_url),
                                                  result->firmware_url,
                                                  sizeof(result->firmware_url));
    if (err != ESP_OK) {
        return err;
    }

    char manifest[OTA_MANIFEST_MAX];
    err = ota_http_get_text(result->version_url,
                            manifest,
                            sizeof(manifest),
                            &result->status_code,
                            &result->content_length);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_parse_version_manifest(manifest,
                                     result->available_version,
                                     sizeof(result->available_version));
    if (err != ESP_OK) {
        return err;
    }

    result->update_available =
        strcmp(result->available_version, SOLAR_OS_VERSION) != 0 ||
        strcmp(result->target_flavor, SOLAR_OS_FLAVOR_NAME) != 0;
    SOLAR_OS_LOGI(TAG,
             "check current=%s/%s target=%s available=%s update=%s",
             SOLAR_OS_VERSION,
             SOLAR_OS_FLAVOR_NAME,
             result->target_flavor,
             result->available_version,
             result->update_available ? "yes" : "no");
    return ESP_OK;
}

static void ota_report_progress(solar_os_ota_progress_cb_t cb,
                                void *user,
                                const solar_os_ota_progress_t *progress)
{
    if (cb != NULL && progress != NULL) {
        cb(progress, user);
    }
}

esp_err_t solar_os_ota_upgrade(solar_os_ota_progress_cb_t progress, void *user)
{
    char firmware_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    esp_err_t err = ota_build_artifact_url(SOLAR_OS_OTA_FIRMWARE_FILE,
                                           firmware_url,
                                           sizeof(firmware_url));
    if (err != ESP_OK) {
        return err;
    }

    solar_os_ota_progress_t event = {
        .stage = SOLAR_OS_OTA_PROGRESS_CONNECTING,
        .status_code = -1,
    };
    strlcpy(event.firmware_url, firmware_url, sizeof(event.firmware_url));
    ota_report_progress(progress, user, &event);

    SOLAR_OS_LOGI(TAG, "upgrade %s", firmware_url);
    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .user_agent = "SolarOS-ota/0.1",
        .keep_alive_enable = true,
        .max_redirection_count = 3,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "upgrade begin failed: %s", esp_err_to_name(err));
        return err;
    }

    event.status_code = esp_https_ota_get_status_code(handle);
    int image_size = esp_https_ota_get_image_size(handle);
    if (image_size > 0) {
        event.image_size = (uint32_t)image_size;
        event.image_size_known = true;
    }

    esp_app_desc_t app_desc;
    if (esp_https_ota_get_img_desc(handle, &app_desc) == ESP_OK) {
        strlcpy(event.version, app_desc.version, sizeof(event.version));
        event.stage = SOLAR_OS_OTA_PROGRESS_IMAGE;
        const int desc_bytes_read = esp_https_ota_get_image_len_read(handle);
        event.bytes_read = desc_bytes_read > 0 ? (uint32_t)desc_bytes_read : 0;
        ota_report_progress(progress, user, &event);
        SOLAR_OS_LOGI(TAG, "image version=%s project=%s size=%" PRIu32,
                 app_desc.version,
                 app_desc.project_name,
                 event.image_size);
    }

    do {
        err = esp_https_ota_perform(handle);
        const int bytes_read = esp_https_ota_get_image_len_read(handle);
        image_size = esp_https_ota_get_image_size(handle);
        event.stage = SOLAR_OS_OTA_PROGRESS_WRITING;
        event.bytes_read = bytes_read > 0 ? (uint32_t)bytes_read : 0;
        if (image_size > 0) {
            event.image_size = (uint32_t)image_size;
            event.image_size_known = true;
        }
        ota_report_progress(progress, user, &event);
        vTaskDelay(1);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "upgrade perform failed: %s", esp_err_to_name(err));
        (void)esp_https_ota_abort(handle);
        return err;
    }

    event.stage = SOLAR_OS_OTA_PROGRESS_VERIFYING;
    ota_report_progress(progress, user, &event);

    if (!esp_https_ota_is_complete_data_received(handle)) {
        SOLAR_OS_LOGW(TAG, "upgrade incomplete");
        (void)esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "upgrade finish failed: %s", esp_err_to_name(err));
        return err;
    }

    event.stage = SOLAR_OS_OTA_PROGRESS_DONE;
    ota_report_progress(progress, user, &event);
    SOLAR_OS_LOGI(TAG, "upgrade complete");
    return ESP_OK;
}
