#ifndef DEVICE_LINK_H
#define DEVICE_LINK_H

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    char id[32];
    char type_label[32];
    bool pending;
} approval_request_t;

typedef struct {
    char id[32];
    char type_label[32];
    bool pending;
} prompt_request_t;

esp_err_t device_link_init(void);

/**
 * Get the current pending approval request (if any).
 * Returns true if a request is pending, false otherwise.
 */
bool device_link_get_pending_approval(approval_request_t *out);

/**
 * Get the current pending read-only prompt (if any).
 * Returns true if a prompt is pending, false otherwise.
 */
bool device_link_get_pending_prompt(prompt_request_t *out);

/**
 * Clear a pending approval without returning any decision to the host.
 * The device is read-only: every authorization decision is made on the Mac's
 * native prompt. This only dismisses the on-device status overlay.
 */
void device_link_dismiss_approval(void);

/**
 * Clear a pending read-only prompt overlay.
 */
void device_link_dismiss_prompt(void);

#endif
