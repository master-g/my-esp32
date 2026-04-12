#ifndef SERVICE_BITCOIN_H
#define SERVICE_BITCOIN_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SLOT_FP_P2PKH_UNCOMPRESSED = 0,
    SLOT_FP_P2PKH_COMPRESSED = 1,
} slot_fingerprint_kind_t;

typedef enum {
    SLOT_MODE_NORMAL = 0,
    SLOT_MODE_SELFTEST,
} slot_mode_t;

typedef enum {
    SLOT_STATE_IDLE = 0,
    SLOT_STATE_RUNNING,
    SLOT_STATE_PAUSED,
    SLOT_STATE_HIT,
    SLOT_STATE_STORAGE_UNAVAILABLE,
    SLOT_STATE_ERROR,
} slot_state_t;

typedef struct {
    slot_state_t state;
    slot_mode_t mode;
    bool secure_storage_ready;
    uint64_t attempts;
    uint32_t keys_per_sec;
    uint32_t batch_size;
    uint32_t updated_at_epoch_s;
    uint8_t last_fp_kind;
    uint8_t last_hash160_prefix[4];
    uint16_t matched_label_id;
    bool hit_persisted;
    bool is_self_test_hit;
} slot_snapshot_t;

#define SLOT_WIF_MAX_LEN 53

typedef struct {
    bool valid;
    bool is_self_test;
    uint32_t created_at_epoch_s;
    uint16_t label_id;
    uint8_t kind;
    uint8_t hash160[20];
    char wif[SLOT_WIF_MAX_LEN];
} slot_hit_export_t;

esp_err_t bitcoin_service_init(void);
void bitcoin_service_get_slot_snapshot(slot_snapshot_t *out);
esp_err_t bitcoin_service_start_slot(slot_mode_t mode);
void bitcoin_service_pause_slot(void);
void bitcoin_service_reset_slot_counters(void);
void bitcoin_service_acknowledge_hit(void);
bool bitcoin_service_slot_has_real_hit(void);
const char *bitcoin_service_label_for_id(uint16_t label_id);
esp_err_t bitcoin_service_read_hit_record(slot_hit_export_t *out);

#endif
