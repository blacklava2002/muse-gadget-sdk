/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The backend switch (muse_backend.h): the saved choice, the webhook's URL and
 * token, and the public muse_hatch_* API, each call handed to the backend that
 * owns it. Muse's own session answers as muse_hatch_vm_* (renamed at compile
 * time, muse_backend_rename.h); the webhook as muse_hook_* (muse_chat_webhook.c).
 */
#include "muse_backend.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "muse_chat_priv.h"
#include "muse_chat_webhook.h"

static const char *TAG = "muse_backend";

#define NS "muse_hook"

static nvs_handle_t s_nvs;
static SemaphoreHandle_t s_lock;
static atomic_int s_backend = MUSE_BACKEND_MUSE;
/* The backend the current turn started on: a switch mid-turn doesn't strand it. */
static atomic_int s_turn_backend = MUSE_BACKEND_MUSE;
static char s_url[MUSE_WEBHOOK_URL_MAX + 1];
static char s_token[MUSE_WEBHOOK_TOKEN_MAX + 1];

#define LOCKED(body) do { xSemaphoreTake(s_lock, portMAX_DELAY); body; xSemaphoreGive(s_lock); } while (0)

/* ---- Muse's own session, renamed (muse_backend_rename.h) ---- */

void muse_hatch_vm_start(void);
void muse_hatch_vm_status(muse_hatch_status_t *out);
void muse_hatch_vm_test(void);
void muse_hatch_vm_config_changed(void);
void muse_hatch_vm_set_resting(bool resting);
bool muse_hatch_vm_ready(void);
void muse_hatch_vm_turn_begin(void);
void muse_hatch_vm_turn_audio(const int16_t *pcm, size_t frames);
size_t muse_hatch_vm_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms);
void muse_hatch_vm_turn_end(void);
void muse_hatch_vm_turn_cancel(void);
muse_hatch_ev_t muse_hatch_vm_turn_event(char *text, size_t cap);
bool muse_hatch_vm_incoming(void);
bool muse_hatch_vm_turn_caption(size_t played, char *out, size_t cap);
float muse_hatch_vm_turn_progress(size_t played, int *message);
size_t muse_hatch_vm_turn_read(int16_t *pcm, size_t frames, int wait_ms);
size_t muse_hatch_vm_mp3_selftest(int16_t **pcm);
void muse_hatch_vm_text_turn(char *text);
void muse_hatch_vm_text_cancel(void);

/* ---- Settings ---- */

static void load_str(const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(s_nvs, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

void muse_backend_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    if (nvs_open(NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: webhook settings won't be kept");
        return;
    }
    uint8_t b = MUSE_BACKEND_MUSE;
    nvs_get_u8(s_nvs, "backend", &b);
    load_str("url", s_url, sizeof(s_url));
    load_str("token", s_token, sizeof(s_token));
    atomic_store(&s_backend, b == MUSE_BACKEND_WEBHOOK ? MUSE_BACKEND_WEBHOOK : MUSE_BACKEND_MUSE);
    ESP_LOGI(TAG, "backend: %s%s", muse_backend_name(muse_backend()),
             muse_backend_webhook() && !s_url[0] ? " (no URL yet)" : "");
}

muse_backend_t muse_backend(void)
{
    return (muse_backend_t)atomic_load(&s_backend);
}

const char *muse_backend_name(muse_backend_t backend)
{
    return backend == MUSE_BACKEND_WEBHOOK ? "Webhook" : "Muse";
}

void muse_backend_set(muse_backend_t backend)
{
    backend = backend == MUSE_BACKEND_WEBHOOK ? MUSE_BACKEND_WEBHOOK : MUSE_BACKEND_MUSE;
    if (atomic_exchange(&s_backend, backend) == (int)backend) {
        return;
    }
    if (s_nvs) {
        nvs_set_u8(s_nvs, "backend", (uint8_t)backend);
        nvs_commit(s_nvs);
    }
    ESP_LOGI(TAG, "backend now %s", muse_backend_name(backend));
    muse_hook_config_changed();
}

static esp_err_t set_str(const char *key, char *dst, size_t cap, const char *v, bool append)
{
    esp_err_t err = ESP_OK;
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    LOCKED({
        size_t have = append ? strlen(dst) : 0;
        size_t add = strlen(v ? v : "");
        if (have + add >= cap) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(dst + have, v, add);
            dst[have + add] = '\0';
            if (s_nvs) {
                nvs_set_str(s_nvs, key, dst);
                nvs_commit(s_nvs);
            }
        }
    });
    if (err == ESP_OK) {
        muse_hook_config_changed();
    }
    return err;
}

void muse_webhook_url(char out[MUSE_WEBHOOK_URL_MAX + 1])
{
    if (!s_lock) {
        out[0] = '\0';
        return;
    }
    LOCKED(strlcpy(out, s_url, MUSE_WEBHOOK_URL_MAX + 1));
}

esp_err_t muse_webhook_set_url(const char *url, bool append)
{
    return set_str("url", s_url, sizeof(s_url), url, append);
}

void muse_webhook_token(char out[MUSE_WEBHOOK_TOKEN_MAX + 1])
{
    if (!s_lock) {
        out[0] = '\0';
        return;
    }
    LOCKED(strlcpy(out, s_token, MUSE_WEBHOOK_TOKEN_MAX + 1));
}

size_t muse_webhook_token_len(void)
{
    size_t n = 0;
    if (s_lock) {
        LOCKED(n = strlen(s_token));
    }
    return n;
}

esp_err_t muse_webhook_set_token(const char *token, bool append)
{
    return set_str("token", s_token, sizeof(s_token), token, append);
}

bool muse_backend_command(const char *cmd, const char *v, const char **res, bool *secret)
{
    *res = "ok";
    *secret = false;
    if (!strcmp(cmd, "backend")) {
        if (!strcmp(v, "muse")) {
            muse_backend_set(MUSE_BACKEND_MUSE);
        } else if (!strcmp(v, "webhook")) {
            muse_backend_set(MUSE_BACKEND_WEBHOOK);
        } else {
            *res = "error: backend=muse or backend=webhook";
        }
    } else if (!strcmp(cmd, "webhook.url") || !strcmp(cmd, "webhook.url+")) {
        bool append = cmd[11] == '+';
        if (!append && v[0] && strncmp(v, "https://", 8) && strncmp(v, "http://", 7)) {
            *res = "error: URL must start with https:// or http://";
        } else if (muse_webhook_set_url(v, append) != ESP_OK) {
            *res = "error: URL too long";
        }
        /* A URL can hold a secret (?key=...), so it isn't logged either. */
        *secret = true;
    } else if (!strcmp(cmd, "webhook.token") || !strcmp(cmd, "webhook.token+")) {
        if (muse_webhook_set_token(v, cmd[13] == '+') != ESP_OK) {
            *res = "error: token too long";
        }
        *secret = true;
    } else if (!strcmp(cmd, "webhook.test")) {
        muse_hook_test();
    } else {
        return false;
    }
    return true;
}

/* ---- The public API: each call to its backend ---- */

#define WEBHOOK_TURN() (atomic_load(&s_turn_backend) == MUSE_BACKEND_WEBHOOK)

static void latch(void)
{
    atomic_store(&s_turn_backend, atomic_load(&s_backend));
}

void muse_hatch_start(void)
{
    muse_backend_init();
    muse_hatch_vm_start();   /* always: a switch back to Muse needs no restart */
    muse_hook_start();
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    if (muse_backend_webhook()) {
        muse_hook_status(out);
    } else {
        muse_hatch_vm_status(out);
    }
}

void muse_hatch_test(void)
{
    if (muse_backend_webhook()) {
        muse_hook_test();
    } else {
        muse_hatch_vm_test();
    }
}

void muse_hatch_config_changed(void)
{
    muse_hatch_vm_config_changed();   /* MUSE_SETTING_HATCH: Muse's server, VM or token */
}

void muse_hatch_set_resting(bool resting)
{
    muse_hatch_vm_set_resting(resting);
    muse_hook_set_resting(resting);
}

bool muse_hatch_ready(void)
{
    return muse_backend_webhook() ? muse_hook_ready() : muse_hatch_vm_ready();
}

void muse_hatch_turn_begin(void)
{
    /* Whatever the last turn was on, it's over now. */
    if (WEBHOOK_TURN() != muse_backend_webhook()) {
        if (WEBHOOK_TURN()) {
            muse_hook_turn_cancel();
        } else {
            muse_hatch_vm_turn_cancel();
        }
    }
    latch();
    if (WEBHOOK_TURN()) {
        muse_hook_turn_begin();
    } else {
        muse_hatch_vm_turn_begin();
    }
}

void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    if (WEBHOOK_TURN()) {
        muse_hook_turn_audio(pcm, frames);
    } else {
        muse_hatch_vm_turn_audio(pcm, frames);
    }
}

size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    return WEBHOOK_TURN() ? muse_hook_turn_audio_wait(pcm, frames, wait_ms)
                          : muse_hatch_vm_turn_audio_wait(pcm, frames, wait_ms);
}

void muse_hatch_turn_end(void)
{
    if (WEBHOOK_TURN()) {
        muse_hook_turn_end();
    } else {
        muse_hatch_vm_turn_end();
    }
}

void muse_hatch_turn_cancel(void)
{
    if (WEBHOOK_TURN()) {
        muse_hook_turn_cancel();
    } else {
        muse_hatch_vm_turn_cancel();
    }
}

muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    return WEBHOOK_TURN() ? muse_hook_turn_event(text, cap) : muse_hatch_vm_turn_event(text, cap);
}

bool muse_hatch_incoming(void)
{
    /* Only Muse holds replies for later; with the webhook chosen they wait in the Muse app. */
    if (muse_backend_webhook()) {
        return false;
    }
    if (!muse_hatch_vm_incoming()) {
        return false;
    }
    atomic_store(&s_turn_backend, MUSE_BACKEND_MUSE);
    return true;
}

bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    return WEBHOOK_TURN() ? muse_hook_turn_caption(played, out, cap) : muse_hatch_vm_turn_caption(played, out, cap);
}

float muse_turn_progress(size_t played, int *message)
{
    return WEBHOOK_TURN() ? muse_hook_turn_progress(played, message) : muse_hatch_vm_turn_progress(played, message);
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    return WEBHOOK_TURN() ? muse_hook_turn_read(pcm, frames, wait_ms) : muse_hatch_vm_turn_read(pcm, frames, wait_ms);
}

size_t muse_hatch_mp3_selftest(int16_t **pcm)
{
    return muse_hatch_vm_mp3_selftest(pcm);
}

void muse_hatch_text_turn(char *text)
{
    if (muse_backend_webhook()) {
        muse_hook_text_turn(text);
    } else {
        muse_hatch_vm_text_turn(text);
    }
}

void muse_hatch_text_cancel(void)
{
    muse_hook_text_cancel();
    muse_hatch_vm_text_cancel();
}
