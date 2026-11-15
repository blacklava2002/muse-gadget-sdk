/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/*
 * The webhook's reply, parsed (docs/webhook.md). Portable C with no FreeRTOS,
 * so it builds and is tested on a PC too (tests/webhook/).
 *
 * The reply body is fed in as it arrives, in pieces of any size. By its
 * Content-Type it is:
 *   application/json       one object: {"heard","reply","audio":{...},"error"}
 *   application/x-ndjson   one event per line: {"type":"heard"|"reply"|"audio"|"done"|"error",...}
 *   audio/mpeg, audio/wav  the spoken reply itself; its text, if any, in the
 *                          X-Reply-Text header (percent-encoded UTF-8)
 *   text/plain             the reply text
 * Audio (MP3, WAV or raw 16-bit PCM) comes out as 16 kHz mono PCM.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOOK_OUT_RATE 16000

typedef struct {
    void *ctx;
    void (*heard)(void *ctx, const char *text);               /* what was said, as the agent heard it */
    void (*reply)(void *ctx, const char *text, bool append);  /* reply text: replaces, or appends a piece */
    /* Reply speech, 16 kHz mono. Returns false to stop (the turn was cancelled). */
    bool (*pcm)(void *ctx, const int16_t *pcm, size_t frames);
    void (*error)(void *ctx, const char *text);
    bool skip_audio;   /* speaker off: audio is parsed past, not decoded */
} hook_sink_t;

typedef struct hook_parser hook_parser_t;

/* content_type and reply_text (the X-Reply-Text header) may be NULL. NULL if out of memory. */
hook_parser_t *hook_parser_new(const hook_sink_t *sink, const char *content_type, const char *reply_text);
/* False once parsing has failed or the sink said stop; the error has been reported. */
bool hook_parser_feed(hook_parser_t *p, const uint8_t *data, size_t n);
/* End of the body: flushes the last line or object and any audio still held. */
bool hook_parser_finish(hook_parser_t *p);
/* Total reply speech handed to the sink, in frames. */
size_t hook_parser_frames(const hook_parser_t *p);
void hook_parser_free(hook_parser_t *p);

/* Decodes %XX escapes (and '+' as a space) in place. */
void hook_percent_decode(char *s);

/* Hooks for the allocator (PSRAM on the gadget). Default malloc/realloc/free. */
void hook_parser_set_alloc(void *(*alloc)(size_t), void *(*grow)(void *, size_t), void (*release)(void *));

#ifdef __cplusplus
}
#endif
