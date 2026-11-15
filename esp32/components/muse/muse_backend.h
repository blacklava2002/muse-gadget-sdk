/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/*
 * Who answers a push-to-talk turn (CONFIG_MUSE_WEBHOOK).
 *
 *   MUSE     Muse's own voice session (muse_chat_session.cpp), as before.
 *   WEBHOOK  An HTTP(S) endpoint of your own (muse_chat_webhook.c): the note
 *            is POSTed there as a WAV and the reply comes back as JSON or
 *            NDJSON, with optional audio. See docs/webhook.md for the contract.
 *
 * The choice is kept in NVS and can change at any time; a turn already under
 * way finishes on the backend it started on. Live voice calls only exist on
 * the Muse backend: with the webhook chosen a tap does nothing and a hold
 * sends a note.
 *
 * Every muse_hatch_* function in muse_chat.h goes through muse_backend.c,
 * which hands it to the chosen backend. The Muse backend's own definitions
 * are renamed muse_hatch_vm_* at compile time (muse_backend_rename.h).
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSE_BACKEND_MUSE = 0,
    MUSE_BACKEND_WEBHOOK = 1,
} muse_backend_t;

#define MUSE_WEBHOOK_URL_MAX 255
#define MUSE_WEBHOOK_TOKEN_MAX 1023

/* Loads the saved choice; call after nvs_flash_init (muse_settings_init). */
void muse_backend_init(void);

muse_backend_t muse_backend(void);
static inline bool muse_backend_webhook(void) { return muse_backend() == MUSE_BACKEND_WEBHOOK; }
void muse_backend_set(muse_backend_t backend);
const char *muse_backend_name(muse_backend_t backend);   /* "Muse" / "Webhook" */

/* The webhook's URL (http:// or https://) and optional bearer token. The
 * setters save at once. append=true adds to what is stored, for values sent
 * over Bluetooth in pieces ("webhook.url+", "webhook.token+"). */
void muse_webhook_url(char out[MUSE_WEBHOOK_URL_MAX + 1]);
esp_err_t muse_webhook_set_url(const char *url, bool append);
void muse_webhook_token(char out[MUSE_WEBHOOK_TOKEN_MAX + 1]);
size_t muse_webhook_token_len(void);
esp_err_t muse_webhook_set_token(const char *token, bool append);

/* Serial/BLE commands (muse_ble.c): true if `cmd` was one of ours; *res then
 * says how it went ("ok" or "error: ..."), and *secret whether `v` must not
 * be logged. */
bool muse_backend_command(const char *cmd, const char *v, const char **res, bool *secret);

#ifdef __cplusplus
}
#endif
