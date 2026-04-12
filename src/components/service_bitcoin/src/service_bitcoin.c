#include "service_bitcoin.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core_types/app_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/bignum.h"
#include "mbedtls/ecp.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/private/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "power_policy.h"
#include "slot_ripemd160.h"

#define SLOT_BATCH_SIZE 16
#define SLOT_UI_UPDATE_US (500LL * 1000)
#define SLOT_TASK_STACK_SIZE (18 * 1024)
#define SLOT_COMMAND_QUEUE_LEN 8

#define SLOT_LABEL_GENESIS 1
#define SLOT_LABEL_SELFTEST 0x8001

typedef struct {
    uint16_t label_id;
    slot_fingerprint_kind_t kind;
    uint8_t hash160[20];
} slot_target_entry_t;

typedef struct {
    bool is_self_test;
    uint32_t created_at_epoch_s;
    uint16_t label_id;
    uint8_t kind;
    uint8_t hash160[20];
    uint8_t private_key[32];
} slot_hit_record_t;

typedef enum {
    SLOT_CMD_START = 1,
    SLOT_CMD_PAUSE,
    SLOT_CMD_RESET,
    SLOT_CMD_ACK,
} slot_command_type_t;

typedef struct {
    slot_command_type_t type;
    slot_mode_t mode;
} slot_command_t;

static const char *TAG = "service_bitcoin";
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_command_queue;
static slot_snapshot_t s_snapshot;
static bool s_has_real_hit;
static bool s_worker_should_run;
static int64_t s_last_rate_update_us;
static uint64_t s_attempts_at_last_rate;
static mbedtls_ecp_group s_ecp_group;
static mbedtls_mpi s_curve_order;
static slot_target_entry_t s_selftest_target;

static const slot_target_entry_t s_prod_targets[] = {
    {
        .label_id = SLOT_LABEL_GENESIS,
        .kind = SLOT_FP_P2PKH_UNCOMPRESSED,
        .hash160 =
            {
                0x62, 0xe9, 0x07, 0xb1, 0x5c, 0xbf, 0x27, 0xd5, 0x42, 0x53,
                0x99, 0xeb, 0xf6, 0xf0, 0xfb, 0x50, 0xeb, 0xb8, 0x8f, 0x18,
            },
    },
};

static const uint8_t s_selftest_privkey[32] = {
    0x1f, 0x1e, 0x1d, 0x1c, 0x1b, 0x1a, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x01,
};

static void publish_slot_event(void)
{
    app_event_t event = {
        .type = APP_EVENT_DATA_BITCOIN,
        .payload = NULL,
    };

    event_bus_publish(&event);
}

static uint32_t current_epoch_s(void)
{
    time_t now = time(NULL);

    if (now <= 1700000000) {
        return 0;
    }

    return (uint32_t)now;
}

static int slot_rng_fill(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static void slot_snapshot_touch_update(void) { s_snapshot.updated_at_epoch_s = current_epoch_s(); }

static bool slot_storage_ready(void)
{
#if CONFIG_NVS_ENCRYPTION
    return nvs_flash_get_default_security_scheme() != NULL;
#else
    return false;
#endif
}

static const slot_target_entry_t *slot_find_target(slot_mode_t mode, const uint8_t *hash160,
                                                   slot_fingerprint_kind_t kind)
{
    size_t i = 0;

    if (hash160 == NULL) {
        return NULL;
    }

    if (mode == SLOT_MODE_SELFTEST) {
        if (s_selftest_target.kind == kind &&
            memcmp(s_selftest_target.hash160, hash160, sizeof(s_selftest_target.hash160)) == 0) {
            return &s_selftest_target;
        }
        return NULL;
    }

    for (i = 0; i < sizeof(s_prod_targets) / sizeof(s_prod_targets[0]); ++i) {
        if (s_prod_targets[i].kind == kind &&
            memcmp(s_prod_targets[i].hash160, hash160, sizeof(s_prod_targets[i].hash160)) == 0) {
            return &s_prod_targets[i];
        }
    }

    return NULL;
}

static void slot_write_prefix(slot_snapshot_t *snapshot, const uint8_t *hash160,
                              slot_fingerprint_kind_t kind)
{
    if (snapshot == NULL || hash160 == NULL) {
        return;
    }

    snapshot->last_fp_kind = (uint8_t)kind;
    memcpy(snapshot->last_hash160_prefix, hash160, sizeof(snapshot->last_hash160_prefix));
}

static esp_err_t slot_compute_hash160(const uint8_t *privkey, bool compressed, uint8_t out[20])
{
    mbedtls_mpi priv;
    mbedtls_ecp_point pub;
    uint8_t pubkey[65] = {0};
    uint8_t sha256[32] = {0};
    size_t olen = 0;
    esp_err_t err = ESP_OK;
    int ret = 0;

    ESP_RETURN_ON_FALSE(privkey != NULL, ESP_ERR_INVALID_ARG, TAG, "privkey required");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "hash out required");

    mbedtls_mpi_init(&priv);
    mbedtls_ecp_point_init(&pub);

    ret = mbedtls_mpi_read_binary(&priv, privkey, 32);
    if (ret != 0 || mbedtls_mpi_cmp_int(&priv, 1) < 0 ||
        mbedtls_mpi_cmp_mpi(&priv, &s_curve_order) >= 0) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    ret = mbedtls_ecp_mul(&s_ecp_group, &pub, &priv, &s_ecp_group.G, slot_rng_fill, NULL);
    if (ret != 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    ret = mbedtls_ecp_point_write_binary(
        &s_ecp_group, &pub, compressed ? MBEDTLS_ECP_PF_COMPRESSED : MBEDTLS_ECP_PF_UNCOMPRESSED,
        &olen, pubkey, sizeof(pubkey));
    if (ret != 0 || olen == 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    ret = mbedtls_sha256(pubkey, olen, sha256, 0);
    if (ret != 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    ret = slot_ripemd160(sha256, sizeof(sha256), out);
    if (ret != 0) {
        err = ESP_FAIL;
    }

cleanup:
    mbedtls_platform_zeroize(sha256, sizeof(sha256));
    mbedtls_platform_zeroize(pubkey, sizeof(pubkey));
    mbedtls_ecp_point_free(&pub);
    mbedtls_mpi_free(&priv);
    return err;
}

static esp_err_t slot_prepare_selftest_target(void)
{
    esp_err_t err;

    memset(&s_selftest_target, 0, sizeof(s_selftest_target));
    s_selftest_target.label_id = SLOT_LABEL_SELFTEST;
    s_selftest_target.kind = SLOT_FP_P2PKH_COMPRESSED;
    err = slot_compute_hash160(s_selftest_privkey, true, s_selftest_target.hash160);
    return err;
}

static esp_err_t slot_write_hit_record(const char *key, const slot_hit_record_t *record)
{
    nvs_handle_t handle = 0;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, TAG, "nvs key required");
    ESP_RETURN_ON_FALSE(record != NULL, ESP_ERR_INVALID_ARG, TAG, "record required");

    err = nvs_open("slot", NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "slot nvs open failed");
    err = nvs_set_blob(handle, key, record, sizeof(*record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t slot_read_hit_record(const char *key, slot_hit_record_t *record)
{
    nvs_handle_t handle = 0;
    esp_err_t err;
    size_t len = sizeof(*record);

    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, TAG, "nvs key required");
    ESP_RETURN_ON_FALSE(record != NULL, ESP_ERR_INVALID_ARG, TAG, "record required");

    err = nvs_open("slot", NVS_READONLY, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "slot nvs open failed");
    err = nvs_get_blob(handle, key, record, &len);
    nvs_close(handle);

    if (err == ESP_OK && len != sizeof(*record)) {
        memset(record, 0, sizeof(*record));
        return ESP_ERR_INVALID_SIZE;
    }

    return err;
}

static int slot_base58_encode(const uint8_t *input, size_t input_len, char *output,
                              size_t output_len)
{
    static const char alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    uint8_t temp[56] = {0};
    size_t leading_zeros = 0;
    size_t encoded_len = 0;
    size_t total;
    size_t i;
    size_t j;

    while (leading_zeros < input_len && input[leading_zeros] == 0) {
        leading_zeros++;
    }

    for (i = leading_zeros; i < input_len; i++) {
        int carry = input[i];
        for (j = 0; j < encoded_len; j++) {
            carry += 256 * temp[j];
            temp[j] = carry % 58;
            carry /= 58;
        }
        while (carry > 0) {
            if (encoded_len >= sizeof(temp)) {
                return -1;
            }
            temp[encoded_len++] = carry % 58;
            carry /= 58;
        }
    }

    total = leading_zeros + encoded_len;
    if (total + 1 > output_len) {
        return -1;
    }

    for (i = 0; i < leading_zeros; i++) {
        output[i] = '1';
    }
    for (i = 0; i < encoded_len; i++) {
        output[leading_zeros + i] = alphabet[temp[encoded_len - 1 - i]];
    }
    output[total] = '\0';
    return (int)total;
}

static esp_err_t slot_encode_wif(const uint8_t privkey[32], slot_fingerprint_kind_t kind,
                                 char *wif_out, size_t wif_len)
{
    uint8_t payload[34];
    uint8_t hash1[32];
    uint8_t hash2[32];
    uint8_t full[38];
    size_t payload_len;
    int ret;

    payload[0] = 0x80;
    memcpy(payload + 1, privkey, 32);

    if (kind == SLOT_FP_P2PKH_COMPRESSED) {
        payload[33] = 0x01;
        payload_len = 34;
    } else {
        payload_len = 33;
    }

    mbedtls_sha256(payload, payload_len, hash1, 0);
    mbedtls_sha256(hash1, 32, hash2, 0);

    memcpy(full, payload, payload_len);
    memcpy(full + payload_len, hash2, 4);

    ret = slot_base58_encode(full, payload_len + 4, wif_out, wif_len);

    mbedtls_platform_zeroize(payload, sizeof(payload));
    mbedtls_platform_zeroize(hash1, sizeof(hash1));
    mbedtls_platform_zeroize(hash2, sizeof(hash2));
    mbedtls_platform_zeroize(full, sizeof(full));

    return (ret > 0) ? ESP_OK : ESP_FAIL;
}

static void slot_apply_hit_locked(slot_mode_t mode, const uint8_t *privkey,
                                  const slot_target_entry_t *target, const uint8_t *hash160,
                                  slot_fingerprint_kind_t kind)
{
    slot_hit_record_t record = {0};
    esp_err_t persist_err;

    s_snapshot.state = SLOT_STATE_HIT;
    s_snapshot.mode = mode;
    s_snapshot.matched_label_id = target->label_id;
    s_snapshot.is_self_test_hit = (mode == SLOT_MODE_SELFTEST);
    s_snapshot.hit_persisted = false;
    slot_snapshot_touch_update();
    slot_write_prefix(&s_snapshot, hash160, kind);

    record.is_self_test = (mode == SLOT_MODE_SELFTEST);
    record.created_at_epoch_s = s_snapshot.updated_at_epoch_s;
    record.label_id = target->label_id;
    record.kind = (uint8_t)kind;
    memcpy(record.hash160, hash160, sizeof(record.hash160));
    memcpy(record.private_key, privkey, sizeof(record.private_key));

    persist_err = slot_write_hit_record(
        mode == SLOT_MODE_SELFTEST ? "slot_selftest_hit_latest" : "slot_real_hit_latest", &record);
    s_snapshot.hit_persisted = (persist_err == ESP_OK);
    if (persist_err == ESP_OK && mode == SLOT_MODE_NORMAL) {
        s_has_real_hit = true;
    }
    mbedtls_platform_zeroize(&record, sizeof(record));
}

typedef struct {
    bool hit;
    slot_fingerprint_kind_t kind;
    uint8_t hash160[20];
    const slot_target_entry_t *target;
} slot_candidate_result_t;

static void slot_evaluate_candidate(slot_mode_t mode, const uint8_t *privkey,
                                    slot_candidate_result_t *result)
{
    memset(result, 0, sizeof(*result));

    if (slot_compute_hash160(privkey, false, result->hash160) == ESP_OK) {
        result->kind = SLOT_FP_P2PKH_UNCOMPRESSED;
        result->target = slot_find_target(mode, result->hash160, SLOT_FP_P2PKH_UNCOMPRESSED);
        if (result->target != NULL) {
            result->hit = true;
            return;
        }
    }

    if (slot_compute_hash160(privkey, true, result->hash160) == ESP_OK) {
        result->kind = SLOT_FP_P2PKH_COMPRESSED;
        result->target = slot_find_target(mode, result->hash160, SLOT_FP_P2PKH_COMPRESSED);
        if (result->target != NULL) {
            result->hit = true;
            return;
        }
    }
}

static void slot_update_rate_locked(void)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_us = now_us - s_last_rate_update_us;

    if (elapsed_us < SLOT_UI_UPDATE_US || elapsed_us <= 0) {
        return;
    }

    s_snapshot.keys_per_sec =
        (uint32_t)(((s_snapshot.attempts - s_attempts_at_last_rate) * 1000000ULL) /
                   (uint64_t)elapsed_us);
    s_attempts_at_last_rate = s_snapshot.attempts;
    s_last_rate_update_us = now_us;
    slot_snapshot_touch_update();
}

static void slot_pause_locked(slot_state_t next_state)
{
    s_worker_should_run = false;
    s_snapshot.state = next_state;
    s_snapshot.keys_per_sec = 0;
    slot_snapshot_touch_update();
}

static void slot_reset_counters_locked(void)
{
    s_snapshot.attempts = 0;
    s_snapshot.keys_per_sec = 0;
    s_snapshot.batch_size = SLOT_BATCH_SIZE;
    s_snapshot.matched_label_id = 0;
    s_snapshot.hit_persisted = false;
    s_snapshot.is_self_test_hit = false;
    memset(s_snapshot.last_hash160_prefix, 0, sizeof(s_snapshot.last_hash160_prefix));
    s_snapshot.last_fp_kind = SLOT_FP_P2PKH_UNCOMPRESSED;
    s_attempts_at_last_rate = 0;
    s_last_rate_update_us = esp_timer_get_time();
    slot_snapshot_touch_update();
}

static void slot_handle_start_locked(slot_mode_t mode)
{
    if (mode == SLOT_MODE_NORMAL && !s_snapshot.secure_storage_ready) {
        s_snapshot.state = SLOT_STATE_STORAGE_UNAVAILABLE;
        s_snapshot.mode = mode;
        slot_snapshot_touch_update();
        return;
    }

    if (s_snapshot.state == SLOT_STATE_HIT && mode == SLOT_MODE_NORMAL && s_has_real_hit) {
        s_snapshot.state = SLOT_STATE_PAUSED;
        slot_snapshot_touch_update();
        return;
    }

    s_snapshot.mode = mode;
    s_snapshot.state = SLOT_STATE_RUNNING;
    s_snapshot.batch_size = SLOT_BATCH_SIZE;
    s_snapshot.keys_per_sec = 0;
    s_worker_should_run = true;
    s_attempts_at_last_rate = s_snapshot.attempts;
    s_last_rate_update_us = esp_timer_get_time();
    slot_snapshot_touch_update();
}

static void bitcoin_service_task(void *arg)
{
    slot_command_t command = {0};

    (void)arg;
    for (;;) {
        bool did_work = false;

        while (xQueueReceive(s_command_queue, &command, 0) == pdTRUE) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            switch (command.type) {
            case SLOT_CMD_START:
                slot_handle_start_locked(command.mode);
                break;
            case SLOT_CMD_PAUSE:
                if (s_snapshot.state == SLOT_STATE_RUNNING) {
                    slot_pause_locked(SLOT_STATE_PAUSED);
                }
                break;
            case SLOT_CMD_RESET:
                slot_reset_counters_locked();
                if (s_snapshot.state != SLOT_STATE_RUNNING) {
                    s_snapshot.state = s_snapshot.secure_storage_ready
                                           ? SLOT_STATE_IDLE
                                           : SLOT_STATE_STORAGE_UNAVAILABLE;
                }
                break;
            case SLOT_CMD_ACK:
                if (s_snapshot.state == SLOT_STATE_HIT) {
                    if (s_snapshot.is_self_test_hit) {
                        slot_reset_counters_locked();
                        s_snapshot.state = s_snapshot.secure_storage_ready
                                               ? SLOT_STATE_IDLE
                                               : SLOT_STATE_STORAGE_UNAVAILABLE;
                        s_snapshot.mode = SLOT_MODE_NORMAL;
                    } else {
                        s_snapshot.state = SLOT_STATE_PAUSED;
                    }
                    slot_snapshot_touch_update();
                }
                break;
            default:
                break;
            }
            xSemaphoreGive(s_mutex);
            publish_slot_event();
        }

        /* Read state under short lock, then release for expensive ECC work */
        {
            bool should_run;
            slot_mode_t batch_mode;

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            should_run = s_worker_should_run && s_snapshot.state == SLOT_STATE_RUNNING;
            batch_mode = s_snapshot.mode;
            xSemaphoreGive(s_mutex);

            if (should_run) {
                uint8_t candidate[32] = {0};
                slot_candidate_result_t result = {0};
                uint8_t hit_privkey[32] = {0};
                const slot_target_entry_t *hit_target = NULL;
                slot_fingerprint_kind_t hit_kind = SLOT_FP_P2PKH_UNCOMPRESSED;
                uint8_t hit_h160[20] = {0};
                uint32_t batch_count = 0;
                uint32_t i;

                for (i = 0; i < SLOT_BATCH_SIZE; ++i) {
                    if (batch_mode == SLOT_MODE_SELFTEST) {
                        memcpy(candidate, s_selftest_privkey, sizeof(candidate));
                    } else {
                        esp_fill_random(candidate, sizeof(candidate));
                    }

                    batch_count++;
                    slot_evaluate_candidate(batch_mode, candidate, &result);

                    if (result.hit) {
                        hit_target = result.target;
                        hit_kind = result.kind;
                        memcpy(hit_h160, result.hash160, sizeof(hit_h160));
                        memcpy(hit_privkey, candidate, sizeof(hit_privkey));
                        break;
                    }
                    if (batch_mode == SLOT_MODE_SELFTEST) {
                        break;
                    }
                }

                mbedtls_platform_zeroize(candidate, sizeof(candidate));

                /* Short lock to apply results */
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                if (s_snapshot.state == SLOT_STATE_RUNNING) {
                    s_snapshot.attempts += batch_count;
                    slot_write_prefix(&s_snapshot, result.hash160, result.kind);
                    if (hit_target != NULL) {
                        slot_apply_hit_locked(batch_mode, hit_privkey, hit_target, hit_h160,
                                              hit_kind);
                        s_worker_should_run = false;
                    }
                    slot_update_rate_locked();
                }
                xSemaphoreGive(s_mutex);

                mbedtls_platform_zeroize(hit_privkey, sizeof(hit_privkey));
                mbedtls_platform_zeroize(hit_h160, sizeof(hit_h160));
                mbedtls_platform_zeroize(&result, sizeof(result));
                did_work = true;
            }
        }

        if (did_work) {
            publish_slot_event();
            taskYIELD();
            continue;
        }

        if (xQueueReceive(s_command_queue, &command, pdMS_TO_TICKS(100)) == pdTRUE) {
            xQueueSendToFront(s_command_queue, &command, 0);
        }
    }
}

static void bitcoin_service_event_handler(const app_event_t *event, void *context)
{
    power_policy_output_t policy;

    (void)context;
    if (event == NULL) {
        return;
    }

    if (event->type != APP_EVENT_POWER_CHANGED) {
        return;
    }

    power_policy_get_output(&policy);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_snapshot.secure_storage_ready = slot_storage_ready();
    if (!policy.slot_compute_allowed && s_snapshot.state == SLOT_STATE_RUNNING) {
        slot_pause_locked(SLOT_STATE_PAUSED);
        xSemaphoreGive(s_mutex);
        publish_slot_event();
        return;
    }
    if (!s_snapshot.secure_storage_ready && s_snapshot.state == SLOT_STATE_IDLE) {
        s_snapshot.state = SLOT_STATE_STORAGE_UNAVAILABLE;
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t bitcoin_service_init(void)
{
    int ret = 0;

    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "bitcoin mutex alloc failed");
    s_command_queue = xQueueCreate(SLOT_COMMAND_QUEUE_LEN, sizeof(slot_command_t));
    ESP_RETURN_ON_FALSE(s_command_queue != NULL, ESP_ERR_NO_MEM, TAG, "bitcoin queue alloc failed");

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.secure_storage_ready = slot_storage_ready();
    s_snapshot.state =
        s_snapshot.secure_storage_ready ? SLOT_STATE_IDLE : SLOT_STATE_STORAGE_UNAVAILABLE;
    s_snapshot.mode = SLOT_MODE_NORMAL;
    s_snapshot.batch_size = SLOT_BATCH_SIZE;
    slot_snapshot_touch_update();
    s_has_real_hit = false;
    s_worker_should_run = false;
    s_last_rate_update_us = esp_timer_get_time();
    s_attempts_at_last_rate = 0;

    mbedtls_ecp_group_init(&s_ecp_group);
    mbedtls_mpi_init(&s_curve_order);
    ret = mbedtls_ecp_group_load(&s_ecp_group, MBEDTLS_ECP_DP_SECP256K1);
    ESP_RETURN_ON_FALSE(ret == 0, ESP_FAIL, TAG, "secp256k1 group load failed");
    ret = mbedtls_mpi_copy(&s_curve_order, &s_ecp_group.N);
    ESP_RETURN_ON_FALSE(ret == 0, ESP_FAIL, TAG, "curve order copy failed");
    ESP_RETURN_ON_ERROR(slot_prepare_selftest_target(), TAG, "selftest target init failed");
    ESP_RETURN_ON_ERROR(event_bus_subscribe(bitcoin_service_event_handler, NULL), TAG,
                        "bitcoin subscribe failed");
    {
        BaseType_t task_ret = xTaskCreatePinnedToCore(bitcoin_service_task, "bitcoin_service",
                                                      SLOT_TASK_STACK_SIZE, NULL, 4, NULL, 1);
        ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG, "bitcoin task create failed");
    }

    return ESP_OK;
}

void bitcoin_service_get_slot_snapshot(slot_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_snapshot;
    xSemaphoreGive(s_mutex);
}

esp_err_t bitcoin_service_start_slot(slot_mode_t mode)
{
    slot_command_t command = {
        .type = SLOT_CMD_START,
        .mode = mode,
    };

    return (xQueueSend(s_command_queue, &command, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void bitcoin_service_pause_slot(void)
{
    slot_command_t command = {
        .type = SLOT_CMD_PAUSE,
        .mode = SLOT_MODE_NORMAL,
    };

    (void)xQueueSend(s_command_queue, &command, 0);
}

void bitcoin_service_reset_slot_counters(void)
{
    slot_command_t command = {
        .type = SLOT_CMD_RESET,
        .mode = SLOT_MODE_NORMAL,
    };

    (void)xQueueSend(s_command_queue, &command, 0);
}

void bitcoin_service_acknowledge_hit(void)
{
    slot_command_t command = {
        .type = SLOT_CMD_ACK,
        .mode = SLOT_MODE_NORMAL,
    };

    (void)xQueueSend(s_command_queue, &command, 0);
}

bool bitcoin_service_slot_has_real_hit(void)
{
    bool has_hit;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    has_hit = s_has_real_hit;
    xSemaphoreGive(s_mutex);
    return has_hit;
}

const char *bitcoin_service_label_for_id(uint16_t label_id)
{
    switch (label_id) {
    case SLOT_LABEL_GENESIS:
        return "Genesis Coinbase";
    case SLOT_LABEL_SELFTEST:
        return "Self-test Vector";
    default:
        return "--";
    }
}

esp_err_t bitcoin_service_read_hit_record(slot_hit_export_t *out)
{
    slot_hit_record_t record = {0};
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "export out required");
    memset(out, 0, sizeof(*out));

    err = slot_read_hit_record("slot_real_hit_latest", &record);
    if (err != ESP_OK) {
        return err;
    }

    out->valid = true;
    out->is_self_test = record.is_self_test;
    out->created_at_epoch_s = record.created_at_epoch_s;
    out->label_id = record.label_id;
    out->kind = record.kind;
    memcpy(out->hash160, record.hash160, sizeof(out->hash160));

    err = slot_encode_wif(record.private_key, (slot_fingerprint_kind_t)record.kind, out->wif,
                          sizeof(out->wif));

    mbedtls_platform_zeroize(&record, sizeof(record));

    if (err != ESP_OK) {
        memset(out, 0, sizeof(*out));
        return err;
    }

    return ESP_OK;
}
