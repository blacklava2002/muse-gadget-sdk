/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The webhook backend: each push-to-talk turn is one HTTP(S) POST to a URL of
 * your own (docs/webhook.md).
 *
 *   request   POST <url>, Content-Type: audio/wav (16 kHz mono 16-bit), the
 *             whole note; Authorization: Bearer <token> when one is set.
 *             Typed console turns and the settings' test send JSON instead.
 *   reply     JSON, NDJSON events, an audio file, or plain text, parsed by
 *             muse_webhook_proto.c. Speech plays through the usual reply path;
 *             with none (or the speaker off) the text is shown at reading pace,
 *             as Muse's own replies are.
 *
 * The note is kept whole in PSRAM while the button is held and sent on
 * release, with a Content-Length: simplest for small servers and for tunnels.
 * One task does the HTTP; the voice task reaches it through a command queue,
 * an event queue and a stream buffer of reply speech, as with Muse's session.
 * A generation number tags each turn so a cancelled one's events and audio are
 * dropped.
 */
#include "muse_chat_webhook.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "cJSON.h"
#include "muse_backend.h"
#include "muse_chat_priv.h"
#include "muse_settings.h"
#include "muse_webhook_proto.h"
#include "muse_wifi.h"

static const char *TAG = "muse_webhook";

#define MIC_RATE 16000
#define OUT_BYTES (MIC_RATE * 2 * 2)       /* 2 s of reply speech */
#define NOTE_MAX_FRAMES (MIC_RATE * CONFIG_MUSE_WEBHOOK_MAX_NOTE_S)
#define TIMEOUT_MS (CONFIG_MUSE_WEBHOOK_TIMEOUT_S * 1000)
#define EV_TEXT 72
#define REPLY_MAX 4096                     /* reply text kept for captions */
#define REPLY_TEXT_HDR_MAX 2048            /* X-Reply-Text, percent-encoded */
#define SPEECH_CHARS_PER_S 14              /* until the speech's length is known */
#define TEXT_CHARS_PER_S 16                /* no speech: reading pace */
#define TEXT_HOLD_S 2                      /* no speech: the last page stays up this long */
#define READ_CHUNK 512                     /* see the read loop */
#define DEVICE "mcu-wearable"              /* as Muse's spoken turns say (muse_chat_priv.h) */

typedef enum { CMD_SEND, CMD_TEXT, CMD_TEST } cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    uint32_t gen;
    uint8_t *buf;   /* CMD_SEND: the WAV; CMD_TEXT: the text. Freed by the task. */
    size_t len;
} cmd_t;

typedef struct {
    muse_hatch_ev_t type;
    uint32_t gen;
    char text[EV_TEXT];
} ev_t;

typedef enum { MODE_VOICE, MODE_TEXT, MODE_TEST } mode_t_;

static QueueHandle_t s_cmds, s_events;
static StreamBufferHandle_t s_out;
static atomic_uint s_gen;

/* Status, for settings and the BLE status blob. */
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_hatch_state_t s_state = MUSE_HATCH_UNTESTED;
static char s_detail[48];

/* The note being recorded: voice task only, handed to the task on release. */
static uint8_t *s_rec;
static size_t s_rec_len, s_rec_cap;

/* The current turn's reply text and speech length, read by captions. */
static SemaphoreHandle_t s_reply_lock;
static char *s_reply;           /* REPLY_MAX, PSRAM */
static uint32_t s_reply_gen;
static size_t s_final_frames;   /* the reply's speech (or reading time) once known, else 0 */

static void *psram(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void *psram_grow(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static void report(muse_hatch_state_t state, const char *detail)
{
    portENTER_CRITICAL(&s_status_lock);
    s_state = state;
    strlcpy(s_detail, detail ? detail : "", sizeof(s_detail));
    portEXIT_CRITICAL(&s_status_lock);
}

static bool current(uint32_t gen)
{
    return gen == atomic_load(&s_gen);
}

static void emit(uint32_t gen, muse_hatch_ev_t type, const char *text)
{
    ev_t ev = { .type = type, .gen = gen };
    if (text) {
        strlcpy(ev.text, text, sizeof(ev.text));
    }
    if (current(gen) && xQueueSend(s_events, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped %d", type);
    }
}

static void drain_out(void)
{
    static int16_t junk[256];
    while (xStreamBufferReceive(s_out, junk, sizeof(junk), 0)) {
    }
}

static void reply_reset(uint32_t gen)
{
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    s_reply[0] = '\0';
    s_reply_gen = gen;
    s_final_frames = 0;
    xSemaphoreGive(s_reply_lock);
}

/* ---- One request ---- */

typedef struct {
    uint32_t gen;
    mode_t_ mode;
    char ctype[64];
    char *reply_text;       /* X-Reply-Text, PSRAM */
    bool errored;
    bool heard_any;
} turn_t;

static esp_err_t on_http(esp_http_client_event_t *evt)
{
    turn_t *t = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !t) {
        return ESP_OK;
    }
    if (!strcasecmp(evt->header_key, "Content-Type")) {
        strlcpy(t->ctype, evt->header_value, sizeof(t->ctype));
    } else if (!strcasecmp(evt->header_key, "X-Reply-Text") && !t->reply_text) {
        size_t n = strnlen(evt->header_value, REPLY_TEXT_HDR_MAX);
        t->reply_text = psram(n + 1);
        if (t->reply_text) {
            memcpy(t->reply_text, evt->header_value, n);
            t->reply_text[n] = '\0';
        }
    }
    return ESP_OK;
}

static void sink_heard(void *ctx, const char *text)
{
    turn_t *t = ctx;
    if (t->mode == MODE_TEXT) {
        muse_hatch_console("heard", text, NULL);
        return;
    }
    char tail[EV_TEXT];
    muse_hatch_tail_words(text, tail, sizeof(tail));
    ESP_LOGI(TAG, "heard: \"%s\"", text);
    emit(t->gen, MUSE_HATCH_EV_HEARD, tail);
}

static void sink_reply(void *ctx, const char *text, bool append)
{
    turn_t *t = ctx;
    if (t->mode == MODE_TEXT) {
        muse_hatch_console("text", text, "\"msg\":0");
        return;
    }
    if (t->mode != MODE_VOICE || !current(t->gen)) {
        return;
    }
    char tail[EV_TEXT];
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    if (s_reply_gen == t->gen) {
        if (!append) {
            s_reply[0] = '\0';
        }
        strlcat(s_reply, text, REPLY_MAX);
        muse_hatch_tail_words(s_reply, tail, sizeof(tail));
    } else {
        tail[0] = '\0';
    }
    xSemaphoreGive(s_reply_lock);
    if (tail[0]) {
        emit(t->gen, MUSE_HATCH_EV_REPLY, tail);
    }
}

/* Waits for room as the voice task plays; stops when the turn is cancelled. */
static bool push_pcm(uint32_t gen, const int16_t *pcm, size_t frames)
{
    size_t bytes = frames * sizeof(int16_t), sent = 0;
    while (sent < bytes) {
        if (!current(gen)) {
            return false;
        }
        sent += xStreamBufferSend(s_out, (const uint8_t *)pcm + sent, bytes - sent, pdMS_TO_TICKS(100));
    }
    return true;
}

static bool sink_pcm(void *ctx, const int16_t *pcm, size_t frames)
{
    turn_t *t = ctx;
    if (t->mode != MODE_VOICE) {
        return true;   /* typed turns aren't spoken */
    }
    return push_pcm(t->gen, pcm, frames);
}

static void sink_error(void *ctx, const char *text)
{
    turn_t *t = ctx;
    t->errored = true;
    ESP_LOGW(TAG, "%s", text);
    if (t->mode == MODE_TEXT) {
        muse_hatch_console("error", text, NULL);
    } else if (t->mode == MODE_VOICE) {
        emit(t->gen, MUSE_HATCH_EV_ERROR, text);
    }
}

/* No speech: silence at reading pace, so captions page and the turn ends. */
static void pace_silently(uint32_t gen)
{
    static const int16_t zeros[256];
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    size_t len = s_reply_gen == gen ? strlen(s_reply) : 0;
    size_t frames = len * MIC_RATE / TEXT_CHARS_PER_S;
    if (s_reply_gen == gen) {
        s_final_frames = frames;
    }
    xSemaphoreGive(s_reply_lock);
    if (!len) {
        return;
    }
    frames += TEXT_HOLD_S * MIC_RATE;
    while (frames && current(gen)) {
        size_t n = frames < 256 ? frames : 256;
        if (!push_pcm(gen, zeros, n)) {
            return;
        }
        frames -= n;
    }
}

static void fail(turn_t *t, const char *why)
{
    sink_error(t, why);
}

/* POSTs `body` and runs the reply through the parser. */
static void run_request(uint32_t gen, mode_t_ mode, const char *ctype, const uint8_t *body, size_t len)
{
    static char url[MUSE_WEBHOOK_URL_MAX + 1];
    static char token[MUSE_WEBHOOK_TOKEN_MAX + 1];
    static char auth[MUSE_WEBHOOK_TOKEN_MAX + 8];
    turn_t t = { .gen = gen, .mode = mode };

    muse_webhook_url(url);
    if (!url[0]) {
        fail(&t, "WEBHOOK URL NOT SET");
        report(MUSE_HATCH_NOT_SET, "Set a webhook URL");
        return;
    }
    muse_webhook_token(token);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = mode == MODE_TEST ? 10000 : TIMEOUT_MS,
        .event_handler = on_http,
        .user_data = &t,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,   /* room for a long bearer token */
        .disable_auto_redirect = false,
        .max_redirection_count = 3,
    };
    if (!strncasecmp(url, "https://", 8)) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        fail(&t, "WEBHOOK: BAD URL");
        report(MUSE_HATCH_UNREACHABLE, "Bad URL");
        return;
    }
    char turn_id[20];
    snprintf(turn_id, sizeof(turn_id), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    esp_http_client_set_header(c, "Content-Type", ctype);
    esp_http_client_set_header(c, "Accept", "application/x-ndjson, application/json, audio/mpeg, audio/wav, text/plain");
    esp_http_client_set_header(c, "User-Agent", "muse-gadget-webhook/1");
    esp_http_client_set_header(c, "X-Gadget-Protocol", "1");
    esp_http_client_set_header(c, "X-Gadget-Device", DEVICE);
    esp_http_client_set_header(c, "X-Gadget-Turn", turn_id);
    esp_http_client_set_header(c, "X-Gadget-Speaker", muse_settings_speaker_on() ? "on" : "off");
    if (token[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(c, "Authorization", auth);
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(c, (int)len);
    size_t off = 0;
    while (err == ESP_OK && off < len) {
        if (!current(gen) && mode != MODE_TEST) {
            goto out;
        }
        int w = esp_http_client_write(c, (const char *)body + off, (int)(len - off > 8192 ? 8192 : len - off));
        if (w <= 0) {
            err = ESP_FAIL;
            break;
        }
        off += (size_t)w;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't reach the webhook: %s", esp_err_to_name(err));
        fail(&t, "CAN'T REACH THE WEBHOOK");
        report(MUSE_HATCH_UNREACHABLE, "Can't connect");
        goto out;
    }
    if (mode == MODE_VOICE) {
        emit(gen, MUSE_HATCH_EV_SENT, NULL);
    } else if (mode == MODE_TEXT) {
        muse_hatch_console("sent", NULL, "\"bytes\":%u", (unsigned)len);
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        fail(&t, "WEBHOOK DIDN'T ANSWER");
        report(MUSE_HATCH_UNREACHABLE, "No answer");
        goto out;
    }
    int status = esp_http_client_get_status_code(c);
    ESP_LOGI(TAG, "HTTP %d, %s, after %.2fs", status, t.ctype[0] ? t.ctype : "no Content-Type",
             (esp_timer_get_time() - t0) / 1e6);
    if (status < 200 || status > 299) {
        char why[EV_TEXT], detail[48];
        snprintf(why, sizeof(why), status == 401 || status == 403 ? "WEBHOOK REFUSED THE TOKEN (%d)"
                                                                  : "WEBHOOK SAID HTTP %d", status);
        snprintf(detail, sizeof(detail), "HTTP %d", status);
        fail(&t, why);
        report(MUSE_HATCH_UNREACHABLE, detail);
        goto out;
    }
    if (mode == MODE_TEST) {
        report(MUSE_HATCH_REACHABLE, "Webhook answered");
        goto out;
    }
    if (status == 204) {
        t.ctype[0] = '\0';   /* nothing to say: an empty reply */
    }

    hook_sink_t sink = {
        .ctx = &t,
        .heard = sink_heard,
        .reply = sink_reply,
        .pcm = sink_pcm,
        .error = sink_error,
        .skip_audio = mode != MODE_VOICE || !muse_settings_speaker_on(),
    };
    hook_parser_t *p = hook_parser_new(&sink, status == 204 ? "text/plain" : t.ctype, t.reply_text);
    if (!p) {
        fail(&t, "WEBHOOK: OUT OF MEMORY");
        goto out;
    }
    static char chunk[READ_CHUNK];
    bool fine = true;
    for (;;) {
        if (mode != MODE_TEST && !current(gen)) {
            fine = false;
            break;
        }
        /* esp_http_client_read waits until the buffer is full or the body ends, so
         * small reads keep NDJSON events coming as they're sent. */
        int r = esp_http_client_read(c, chunk, sizeof(chunk));
        if (r == -ESP_ERR_HTTP_EAGAIN) {
            sink_error(&t, "WEBHOOK TIMED OUT");
            fine = false;
            break;
        }
        if (r < 0) {
            sink_error(&t, "WEBHOOK REPLY CUT OFF");
            fine = false;
            break;
        }
        if (r == 0) {
            if (!esp_http_client_is_complete_data_received(c)) {
                ESP_LOGW(TAG, "reply ended early");
            }
            break;
        }
        if (!hook_parser_feed(p, (const uint8_t *)chunk, (size_t)r)) {
            fine = false;
            break;
        }
    }
    if (fine) {
        fine = hook_parser_finish(p);
    }
    size_t spoken = hook_parser_frames(p);
    hook_parser_free(p);
    if (!fine || t.errored) {
        if (!t.errored && current(gen)) {
            ESP_LOGI(TAG, "turn cancelled");
        }
        goto out;
    }
    report(MUSE_HATCH_REACHABLE, "Last turn OK");
    if (mode == MODE_TEXT) {
        muse_hatch_console("done", NULL, "\"messages\":1,\"complete\":true");
        goto out;
    }
    if (spoken) {
        xSemaphoreTake(s_reply_lock, portMAX_DELAY);
        if (s_reply_gen == gen) {
            s_final_frames = spoken;
        }
        xSemaphoreGive(s_reply_lock);
    } else {
        pace_silently(gen);
    }
    ESP_LOGI(TAG, "reply done: %.2fs of speech, %.2fs in all", (double)spoken / MIC_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    emit(gen, MUSE_HATCH_EV_DONE, "");

out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(t.reply_text);
    memset(token, 0, sizeof(token));
    memset(auth, 0, sizeof(auth));
}

/* RIFF and data sizes, now that the note's length is known. */
static void wav_sizes(uint8_t *wav, size_t len)
{
    uint32_t riff = (uint32_t)(len - 8), data = (uint32_t)(len - MUSE_HATCH_WAV_HEADER);
    for (int i = 0; i < 4; i++) {
        wav[4 + i] = (uint8_t)(riff >> (8 * i));
        wav[40 + i] = (uint8_t)(data >> (8 * i));
    }
}

static void hook_task(void *arg)
{
    (void)arg;
    cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmds, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd.kind) {
        case CMD_SEND:
            if (current(cmd.gen)) {
                wav_sizes(cmd.buf, cmd.len);
                ESP_LOGI(TAG, "sending a %.1fs note", (double)(cmd.len - MUSE_HATCH_WAV_HEADER) / 2 / MIC_RATE);
                run_request(cmd.gen, MODE_VOICE, "audio/wav", cmd.buf, cmd.len);
            }
            break;
        case CMD_TEXT: {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "type", "text");
            cJSON_AddStringToObject(o, "text", (const char *)cmd.buf);
            char *body = o ? cJSON_PrintUnformatted(o) : NULL;
            cJSON_Delete(o);
            if (body) {
                run_request(cmd.gen, MODE_TEXT, "application/json", (const uint8_t *)body, strlen(body));
                cJSON_free(body);
            } else {
                muse_hatch_console("error", "OUT OF MEMORY", NULL);
            }
            break;
        }
        case CMD_TEST: {
            static const char ping[] = "{\"type\":\"ping\"}";
            report(MUSE_HATCH_TESTING, "Connecting...");
            run_request(cmd.gen, MODE_TEST, "application/json", (const uint8_t *)ping, sizeof(ping) - 1);
            break;
        }
        }
        free(cmd.buf);
    }
}

static bool post(cmd_kind_t kind, uint32_t gen, uint8_t *buf, size_t len)
{
    cmd_t cmd = { kind, gen, buf, len };
    if (!s_cmds || xQueueSend(s_cmds, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        free(buf);
        return false;
    }
    return true;
}

/* ---- The turn API ---- */

void muse_hook_start(void)
{
    if (s_cmds) {
        return;
    }
    hook_parser_set_alloc(psram, psram_grow, free);
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    s_events = xQueueCreateWithCaps(16, sizeof(ev_t), MALLOC_CAP_SPIRAM);
    s_out = xStreamBufferCreateWithCaps(OUT_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_reply_lock = xSemaphoreCreateMutex();
    s_reply = psram(REPLY_MAX);
    if (!s_cmds || !s_events || !s_out || !s_reply_lock || !s_reply ||
        xTaskCreatePinnedToCoreWithCaps(hook_task, "muse_hook", 12 * 1024, NULL, 5, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "start failed");
        return;
    }
    s_reply[0] = '\0';
}

void muse_hook_status(muse_hatch_status_t *out)
{
    char url[MUSE_WEBHOOK_URL_MAX + 1];
    muse_webhook_url(url);
    out->vm_id[0] = '\0';
    if (!url[0]) {
        out->state = MUSE_HATCH_NOT_SET;
        strlcpy(out->detail, "Set a webhook URL", sizeof(out->detail));
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    out->state = s_state;
    strlcpy(out->detail, s_detail, sizeof(out->detail));
    portEXIT_CRITICAL(&s_status_lock);
    if (!muse_wifi_connected() && out->state != MUSE_HATCH_TESTING) {
        out->state = MUSE_HATCH_OFFLINE;
        strlcpy(out->detail, "Waiting for Wi-Fi", sizeof(out->detail));
    } else if (out->state == MUSE_HATCH_UNTESTED && !out->detail[0]) {
        strlcpy(out->detail, "Connects when you talk", sizeof(out->detail));
    }
}

void muse_hook_test(void)
{
    if (muse_wifi_connected()) {
        post(CMD_TEST, 0, NULL, 0);
    }
}

void muse_hook_config_changed(void)
{
    report(MUSE_HATCH_UNTESTED, "");
}

void muse_hook_set_resting(bool resting)
{
    (void)resting;   /* nothing held open between turns */
}

bool muse_hook_ready(void)
{
    char url[MUSE_WEBHOOK_URL_MAX + 1];
    muse_webhook_url(url);
    return s_cmds && url[0] && muse_wifi_connected();
}

void muse_hook_turn_begin(void)
{
    uint32_t gen = atomic_fetch_add(&s_gen, 1) + 1;
    drain_out();
    xQueueReset(s_events);
    reply_reset(gen);
    if (!s_rec) {
        s_rec_cap = MUSE_HATCH_WAV_HEADER + NOTE_MAX_FRAMES * sizeof(int16_t);
        s_rec = psram(s_rec_cap);
    }
    if (!s_rec) {
        emit(gen, MUSE_HATCH_EV_ERROR, "OUT OF MEMORY FOR THE NOTE");
        return;
    }
    muse_hatch_wav_header(s_rec, MIC_RATE);
    s_rec_len = MUSE_HATCH_WAV_HEADER;
}

size_t muse_hook_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    (void)wait_ms;
    if (!s_rec) {
        return frames;   /* the error is already out */
    }
    size_t room = (s_rec_cap - s_rec_len) / sizeof(int16_t);
    size_t k = frames < room ? frames : room;
    memcpy(s_rec + s_rec_len, pcm, k * sizeof(int16_t));
    s_rec_len += k * sizeof(int16_t);
    if (k < frames) {
        ESP_LOGW(TAG, "note longer than %ds: cut", CONFIG_MUSE_WEBHOOK_MAX_NOTE_S);
    }
    return frames;   /* what didn't fit is dropped, not left waiting */
}

void muse_hook_turn_audio(const int16_t *pcm, size_t frames)
{
    muse_hook_turn_audio_wait(pcm, frames, 0);
}

void muse_hook_turn_end(void)
{
    if (!s_rec) {
        return;
    }
    uint32_t gen = atomic_load(&s_gen);
    uint8_t *wav = s_rec;
    size_t len = s_rec_len;
    s_rec = NULL;   /* the task owns it now; the next turn gets a fresh one */
    s_rec_len = 0;
    if (!post(CMD_SEND, gen, wav, len)) {
        emit(gen, MUSE_HATCH_EV_ERROR, "WEBHOOK BUSY");
    }
}

void muse_hook_turn_cancel(void)
{
    atomic_fetch_add(&s_gen, 1);
    drain_out();
    s_rec_len = 0;   /* a note never sent: its buffer is kept for the next */
}

muse_hatch_ev_t muse_hook_turn_event(char *text, size_t cap)
{
    ev_t ev;
    while (s_events && xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        if (current(ev.gen)) {
            strlcpy(text, ev.text, cap);
            return ev.type;
        }
    }
    return MUSE_HATCH_EV_NONE;
}

static size_t reply_frames_locked(size_t len)
{
    return s_final_frames ? s_final_frames : len * MIC_RATE / SPEECH_CHARS_PER_S;
}

bool muse_hook_turn_caption(size_t played, char *out, size_t cap)
{
    bool have = false;
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    size_t len = current(s_reply_gen) ? strlen(s_reply) : 0;
    if (len) {
        size_t frames = reply_frames_locked(len);
        size_t at = frames ? (size_t)((uint64_t)played * len / frames) : 0;
        if (at >= len) {
            at = len - 1;
        }
        have = muse_hatch_caption_at(s_reply, at, out, cap);
    }
    xSemaphoreGive(s_reply_lock);
    return have;
}

float muse_hook_turn_progress(size_t played, int *message)
{
    float f = 0;
    int msg = -1;
    xSemaphoreTake(s_reply_lock, portMAX_DELAY);
    size_t len = current(s_reply_gen) ? strlen(s_reply) : 0;
    if (len) {
        msg = 0;
        size_t frames = reply_frames_locked(len);
        if (s_final_frames && played >= s_final_frames) {
            f = 1;
        } else if (frames) {
            f = (float)played / frames;
            f = f < 0.99f ? f : 0.99f;
        }
    }
    xSemaphoreGive(s_reply_lock);
    if (message) {
        *message = msg;
    }
    return f;
}

size_t muse_hook_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    return xStreamBufferReceive(s_out, pcm, frames * sizeof(int16_t), pdMS_TO_TICKS(wait_ms)) / sizeof(int16_t);
}

void muse_hook_text_turn(char *text)
{
    uint32_t gen = atomic_load(&s_gen);
    if (!muse_hook_ready()) {
        muse_hatch_console("error", "WEBHOOK NOT SET UP", NULL);
        free(text);
        return;
    }
    if (!post(CMD_TEXT, gen, (uint8_t *)text, strlen(text))) {
        muse_hatch_console("error", "BUSY", NULL);
    }
}

void muse_hook_text_cancel(void)
{
    atomic_fetch_add(&s_gen, 1);
}
