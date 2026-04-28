#ifndef DEVICE_LINK_H
#define DEVICE_LINK_H

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    APPROVAL_DECISION_ALLOW = 0,
    APPROVAL_DECISION_DENY,
    APPROVAL_DECISION_ALLOW_ALWAYS,
} approval_decision_t;

typedef struct {
    char id[32];
    char tool_name[48];
    char description[80];
    bool pending;
} approval_request_t;

typedef struct {
    char id[32];
    char title[40];
    char question[96];
    char options_text[120];
    char option_labels[4][16]; // Individual option labels for button text
    uint8_t option_count;
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
 * Submit a decision for the current pending approval.
 * This unblocks the RPC handler which sends the response to the host.
 */
void device_link_resolve_approval(approval_decision_t decision);

/**
 * Cancel a pending approval (e.g., on connection loss).
 * Resolves with DENY and unblocks the RPC handler.
 */
void device_link_cancel_approval(void);

/**
 * Clear a pending approval without turning it into a DENY.
 * Used when the host finishes the approval elsewhere and only the device UI
 * needs to be dismissed.
 */
void device_link_dismiss_approval(void);

/**
 * Clear a pending read-only prompt.
 */
/**
 * Submit a selection for the current pending prompt.
 * Sends claude.prompt.response event with the selection index.
 */
void device_link_resolve_prompt(uint8_t selection_index);

void device_link_dismiss_prompt(void);

#endif
