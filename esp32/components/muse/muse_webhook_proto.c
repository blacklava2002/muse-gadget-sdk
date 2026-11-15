/* SPDX-License-Identifier: Apache-2.0 */

#include "muse_webhook_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "minimp3.h"

#define LINE_MAX_BYTES (2u * 1024 * 1024)   /* one NDJSON line, or the whole application/json body */
#define MP3_KEEP (64 * 1024)                /* undecoded MP3 held between pieces */
#define MP3_HOLD (1441 + 4)                 /* the largest frame and the next header */
#define WAV_HEAD_MAX 4096                   /* RIFF chunks before "data" */

static void *(*s_alloc)(size_t) = malloc;
static void *(*s_grow)(void *, size_t) = realloc;
static void (*s_free)(void *) = free;

void hook_parser_set_alloc(void *(*alloc)(size_t), void *(*grow)(void *, size_t), void (*release)(void *))
{
    s_alloc = alloc ? alloc : malloc;
    s_grow = grow ? grow : realloc;
    s_free = release ? release : free;
}

typedef enum { BODY_JSON, BODY_NDJSON, BODY_AUDIO, BODY_TEXT, BODY_IGNORE } body_t;
typedef enum { AU_NONE, AU_PCM, AU_WAV, AU_MP3 } au_fmt_t;

typedef struct {
    uint32_t in, out;   /* rates */
    uint64_t n;         /* index of the next input sample */
    uint64_t next;      /* next output's time, in units of 1/out input samples: j * in */
    int16_t prev;
} resampler_t;

struct hook_parser {
    hook_sink_t sink;
    body_t body;
    bool failed, stopped;

    /* Lines / the JSON body */
    char *line;
    size_t len, cap;

    /* Audio */
    au_fmt_t fmt;
    uint32_t rate;
    int channels;
    bool wav_ready;          /* past the WAV header */
    uint8_t *held;           /* MP3 not yet decoded, WAV header, or an odd PCM byte */
    size_t held_len, held_cap;
    mp3dec_t *mp3;
    int16_t *mp3_pcm;
    resampler_t rs;
    int16_t out[512];
    size_t out_n;
    size_t frames;
    size_t au_bytes;        /* this audio stream: bytes in, and frames out before it began */
    size_t au_start;
};

/* ---- Small helpers ---- */

static void fail(hook_parser_t *p, const char *why)
{
    if (!p->failed && p->sink.error) {
        p->sink.error(p->sink.ctx, why);
    }
    p->failed = true;
}

static bool ok(const hook_parser_t *p)
{
    return !p->failed && !p->stopped;
}

static bool reserve(uint8_t **buf, size_t *cap, size_t need, size_t max)
{
    if (need <= *cap) {
        return true;
    }
    if (need > max) {
        return false;
    }
    size_t want = *cap ? *cap : 4096;
    while (want < need) {
        want *= 2;
    }
    if (want > max) {
        want = max;
    }
    uint8_t *n = s_grow(*buf, want);
    if (!n) {
        return false;
    }
    *buf = n;
    *cap = want;
    return true;
}

static int ci_prefix(const char *s, const char *prefix)
{
    return s && !strncasecmp(s, prefix, strlen(prefix));
}

void hook_percent_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else if (*s == '+') {
            *o++ = ' ';
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

/* Base64 (standard or URL-safe, padding optional, whitespace skipped), in place-safe into out. */
static size_t b64_decode(const char *in, uint8_t *out)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in; in++) {
        int c = (unsigned char)*in, v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

/* ---- Audio out: resample to 16 kHz, hand over in blocks ---- */

static void rs_init(resampler_t *rs, uint32_t in)
{
    memset(rs, 0, sizeof(*rs));
    rs->in = in ? in : HOOK_OUT_RATE;
    rs->out = HOOK_OUT_RATE;
}

static void flush_out(hook_parser_t *p)
{
    if (p->out_n && ok(p)) {
        if (p->sink.pcm && !p->sink.pcm(p->sink.ctx, p->out, p->out_n)) {
            p->stopped = true;
        }
        p->frames += p->out_n;
    }
    p->out_n = 0;
}

static void put_sample(hook_parser_t *p, int16_t y)
{
    p->out[p->out_n++] = y;
    if (p->out_n == sizeof(p->out) / sizeof(p->out[0])) {
        flush_out(p);
    }
}

/* Linear interpolation; output j sits at input time j * in / out. */
static void emit_mono(hook_parser_t *p, const int16_t *x, size_t n)
{
    resampler_t *rs = &p->rs;
    for (size_t i = 0; i < n && ok(p); i++) {
        if (rs->in == rs->out) {
            put_sample(p, x[i]);
            continue;
        }
        if (rs->n == 0) {
            rs->prev = x[i];
        }
        uint64_t here = rs->n * rs->out;   /* this sample's time, in the same units as next */
        while (rs->next <= here && ok(p)) {
            uint64_t back = here - rs->next;           /* 0..out before this sample */
            int32_t y = x[i] - (int32_t)((int64_t)(x[i] - rs->prev) * (int64_t)back / rs->out);
            put_sample(p, (int16_t)y);
            rs->next += rs->in;
        }
        rs->prev = x[i];
        rs->n++;
    }
}

/* Interleaved 16-bit little-endian frames, mixed to mono. */
static void emit_le16(hook_parser_t *p, const uint8_t *b, size_t frames, int channels)
{
    int16_t mono[256];
    while (frames && ok(p)) {
        size_t k = frames < 256 ? frames : 256;
        for (size_t i = 0; i < k; i++) {
            int32_t sum = 0;
            for (int c = 0; c < channels; c++) {
                const uint8_t *s = b + (i * channels + c) * 2;
                sum += (int16_t)(s[0] | s[1] << 8);
            }
            mono[i] = (int16_t)(sum / channels);
        }
        emit_mono(p, mono, k);
        b += k * channels * 2;
        frames -= k;
    }
}

static void audio_end(hook_parser_t *p);

static bool audio_begin(hook_parser_t *p, const char *format, int rate)
{
    au_fmt_t fmt = !format ? AU_NONE
                   : (!strcasecmp(format, "mp3") || !strcasecmp(format, "mpeg") || ci_prefix(format, "audio/mpeg")) ? AU_MP3
                   : (!strcasecmp(format, "wav") || ci_prefix(format, "audio/wav") || ci_prefix(format, "audio/x-wav") ||
                      ci_prefix(format, "audio/wave")) ? AU_WAV
                   : (!strcasecmp(format, "pcm16") || !strcasecmp(format, "pcm_s16le") || !strcasecmp(format, "pcm")) ? AU_PCM
                   : AU_NONE;
    if (fmt == AU_NONE) {
        fail(p, "WEBHOOK: UNKNOWN AUDIO FORMAT");
        return false;
    }
    if (p->fmt == fmt && (fmt != AU_PCM || (uint32_t)rate == p->rate || rate <= 0)) {
        return true;   /* the same stream goes on */
    }
    audio_end(p);
    p->fmt = fmt;
    p->channels = 1;
    p->rate = rate > 0 ? (uint32_t)rate : HOOK_OUT_RATE;
    p->wav_ready = false;
    p->held_len = 0;
    p->au_bytes = 0;
    p->au_start = p->frames + p->out_n;
    rs_init(&p->rs, fmt == AU_PCM ? p->rate : HOOK_OUT_RATE);   /* WAV and MP3 learn theirs from the stream */
    if (fmt == AU_MP3 && !p->mp3) {
        p->mp3 = s_alloc(sizeof(mp3dec_t));
        p->mp3_pcm = s_alloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
        if (!p->mp3 || !p->mp3_pcm) {
            fail(p, "WEBHOOK: OUT OF MEMORY");
            return false;
        }
    }
    if (fmt == AU_MP3) {
        mp3dec_init(p->mp3);
    }
    return true;
}

static void mp3_decode(hook_parser_t *p, bool final)
{
    size_t off = 0;
    size_t hold = final ? 0 : MP3_HOLD;
    while (p->held_len - off > hold && ok(p)) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(p->mp3, p->held + off, (int)(p->held_len - off), p->mp3_pcm, &info);
        if (!info.frame_bytes) {
            if (final) {
                off = p->held_len;
            }
            break;   /* needs more: minimp3 wants to see the next header */
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                p->mp3_pcm[k] = (int16_t)((p->mp3_pcm[2 * k] + p->mp3_pcm[2 * k + 1]) / 2);
            }
        }
        if (p->rs.in != (uint32_t)info.hz) {
            rs_init(&p->rs, (uint32_t)info.hz);
        }
        emit_mono(p, p->mp3_pcm, (size_t)samples);
    }
    memmove(p->held, p->held + off, p->held_len - off);
    p->held_len -= off;
}

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }

/* Finds fmt and data in what's held; true once the data starts (held then holds only samples). */
static bool wav_header(hook_parser_t *p)
{
    const uint8_t *b = p->held;
    size_t n = p->held_len;
    if (n < 12) return false;
    if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        fail(p, "WEBHOOK: AUDIO IS NOT A WAV");
        return false;
    }
    size_t at = 12;
    bool have_fmt = false;
    while (at + 8 <= n) {
        uint32_t size = le32(b + at + 4);
        if (!memcmp(b + at, "data", 4)) {
            if (!have_fmt) {
                fail(p, "WEBHOOK: WAV HAS NO FMT");
                return false;
            }
            at += 8;
            memmove(p->held, p->held + at, n - at);
            p->held_len = n - at;
            return true;
        }
        if (at + 8 + size > n) return false;   /* wait for the whole chunk */
        if (!memcmp(b + at, "fmt ", 4) && size >= 16) {
            uint16_t format = le16(b + at + 8);
            p->channels = le16(b + at + 10);
            p->rate = le32(b + at + 12);
            uint16_t bits = le16(b + at + 22);
            if ((format != 1 && format != 0xFFFE) || bits != 16 || p->channels < 1 || p->channels > 2 || !p->rate) {
                fail(p, "WEBHOOK: WAV MUST BE 16-BIT PCM");
                return false;
            }
            rs_init(&p->rs, p->rate);
            have_fmt = true;
        }
        at += 8 + size + (size & 1);
    }
    if (n >= WAV_HEAD_MAX) {
        fail(p, "WEBHOOK: WAV HEADER TOO LONG");
    }
    return false;
}

static void audio_feed(hook_parser_t *p, const uint8_t *d, size_t n)
{
    if (p->sink.skip_audio || p->fmt == AU_NONE) {
        return;
    }
    p->au_bytes += n;
    while (n && ok(p)) {
        if (p->fmt == AU_MP3) {
            size_t room = MP3_KEEP - p->held_len;
            size_t k = n < room ? n : room;
            if (!k) {
                fail(p, "WEBHOOK: AUDIO IS NOT MP3");   /* 64 KB without a frame */
                return;
            }
            if (!reserve(&p->held, &p->held_cap, p->held_len + k, MP3_KEEP)) {
                fail(p, "WEBHOOK: OUT OF MEMORY");
                return;
            }
            memcpy(p->held + p->held_len, d, k);
            p->held_len += k;
            d += k;
            n -= k;
            mp3_decode(p, false);
            continue;
        }
        if (p->fmt == AU_WAV && !p->wav_ready) {
            size_t k = n < WAV_HEAD_MAX ? n : WAV_HEAD_MAX;
            if (!reserve(&p->held, &p->held_cap, p->held_len + k, WAV_HEAD_MAX * 2)) {
                fail(p, "WEBHOOK: OUT OF MEMORY");
                return;
            }
            memcpy(p->held + p->held_len, d, k);
            p->held_len += k;
            d += k;
            n -= k;
            if (!wav_header(p)) {
                continue;
            }
            p->wav_ready = true;
            /* The samples that came with the header go round again, below. */
            size_t got = p->held_len;
            uint8_t *tmp = s_alloc(got ? got : 1);
            if (!tmp) {
                fail(p, "WEBHOOK: OUT OF MEMORY");
                return;
            }
            memcpy(tmp, p->held, got);
            p->held_len = 0;
            audio_feed(p, tmp, got);
            s_free(tmp);
            continue;
        }
        /* PCM, or a WAV's samples: whole frames, carrying a partial one over. */
        size_t frame = (size_t)p->channels * 2;
        if (p->held_len) {
            size_t k = frame - p->held_len;
            k = k < n ? k : n;
            if (!reserve(&p->held, &p->held_cap, frame, 64)) {
                fail(p, "WEBHOOK: OUT OF MEMORY");
                return;
            }
            memcpy(p->held + p->held_len, d, k);
            p->held_len += k;
            d += k;
            n -= k;
            if (p->held_len == frame) {
                emit_le16(p, p->held, 1, p->channels);
                p->held_len = 0;
            }
            continue;
        }
        size_t frames = n / frame;
        emit_le16(p, d, frames, p->channels);
        d += frames * frame;
        n -= frames * frame;
        if (n) {
            if (!reserve(&p->held, &p->held_cap, frame, 64)) {
                fail(p, "WEBHOOK: OUT OF MEMORY");
                return;
            }
            memcpy(p->held, d, n);
            p->held_len = n;
            n = 0;
        }
    }
}

static void audio_end(hook_parser_t *p)
{
    if (p->fmt == AU_MP3 && !p->sink.skip_audio && ok(p)) {
        mp3_decode(p, true);
        /* minimp3 steps over what it can't read, so junk just comes out silent: say so. */
        if (p->au_bytes > 4096 && p->frames + p->out_n == p->au_start) {
            fail(p, "WEBHOOK: AUDIO IS NOT MP3");
        }
    }
    flush_out(p);
    p->fmt = AU_NONE;
    p->held_len = 0;
}

/* ---- JSON ---- */

static const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int int_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

/* {"format","rate","data_base64"}: decoded in place over the string cJSON holds. */
static void json_audio(hook_parser_t *p, cJSON *o)
{
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(o, "data_base64");
    if (!cJSON_IsString(data) || p->sink.skip_audio) {
        return;
    }
    if (!audio_begin(p, str_of(o, "format") ? str_of(o, "format") : "mp3", int_of(o, "rate"))) {
        return;
    }
    char *s = data->valuestring;
    size_t n = b64_decode(s, (uint8_t *)s);   /* 3 bytes out per 4 in: never overtakes */
    audio_feed(p, (const uint8_t *)s, n);
}

static void json_error(hook_parser_t *p, const char *text)
{
    char why[96];
    snprintf(why, sizeof(why), "WEBHOOK: %s", text && text[0] ? text : "ERROR");
    fail(p, why);
}

static void handle_object(hook_parser_t *p, cJSON *o)
{
    if (!cJSON_IsObject(o)) {
        fail(p, "WEBHOOK: REPLY IS NOT A JSON OBJECT");
        return;
    }
    const char *type = str_of(o, "type");
    if (!type) {
        /* application/json: the whole reply at once */
        if (str_of(o, "error")) {
            json_error(p, str_of(o, "error"));
            return;
        }
        if (str_of(o, "heard") && p->sink.heard) p->sink.heard(p->sink.ctx, str_of(o, "heard"));
        if (str_of(o, "reply") && p->sink.reply) p->sink.reply(p->sink.ctx, str_of(o, "reply"), false);
        cJSON *audio = cJSON_GetObjectItemCaseSensitive(o, "audio");
        if (cJSON_IsObject(audio)) {
            json_audio(p, audio);
            audio_end(p);
        }
        return;
    }
    if (!strcmp(type, "heard")) {
        if (str_of(o, "text") && p->sink.heard) p->sink.heard(p->sink.ctx, str_of(o, "text"));
    } else if (!strcmp(type, "reply")) {
        /* A piece of the reply, appended; "replace":true starts it over. */
        const cJSON *rep = cJSON_GetObjectItemCaseSensitive(o, "replace");
        if (str_of(o, "text") && p->sink.reply) p->sink.reply(p->sink.ctx, str_of(o, "text"), !cJSON_IsTrue(rep));
    } else if (!strcmp(type, "audio")) {
        json_audio(p, o);
    } else if (!strcmp(type, "audio_end")) {
        audio_end(p);
    } else if (!strcmp(type, "error")) {
        json_error(p, str_of(o, "text"));
    } else if (!strcmp(type, "done")) {
        audio_end(p);
    }
    /* Anything else ("ping", "status", ...) is skipped: newer servers may say more. */
}

static void handle_json(hook_parser_t *p, char *text, size_t len)
{
    while (len && isspace((unsigned char)*text)) {
        text++;
        len--;
    }
    if (!len) {
        return;
    }
    cJSON *o = cJSON_ParseWithLength(text, len);
    if (!o) {
        fail(p, "WEBHOOK: BAD JSON IN REPLY");
        return;
    }
    handle_object(p, o);
    cJSON_Delete(o);
}

/* ---- The body ---- */

hook_parser_t *hook_parser_new(const hook_sink_t *sink, const char *content_type, const char *reply_text)
{
    hook_parser_t *p = s_alloc(sizeof(*p));
    if (!p) {
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->sink = *sink;
    const char *ct = content_type ? content_type : "";
    while (*ct == ' ') ct++;
    if (ci_prefix(ct, "application/x-ndjson") || ci_prefix(ct, "application/jsonl") ||
        ci_prefix(ct, "application/ndjson")) {
        p->body = BODY_NDJSON;
    } else if (ci_prefix(ct, "application/json") || !*ct) {
        p->body = BODY_JSON;
    } else if (ci_prefix(ct, "audio/")) {
        p->body = BODY_AUDIO;
        if (reply_text && reply_text[0] && p->sink.reply) {
            size_t n = strlen(reply_text);
            char *t = s_alloc(n + 1);
            if (t) {
                memcpy(t, reply_text, n + 1);
                hook_percent_decode(t);
                p->sink.reply(p->sink.ctx, t, false);
                s_free(t);
            }
        }
        if (!p->sink.skip_audio) {
            char fmt[48];
            strncpy(fmt, ct, sizeof(fmt) - 1);
            fmt[sizeof(fmt) - 1] = '\0';
            char *semi = strchr(fmt, ';');
            if (semi) *semi = '\0';
            if (!audio_begin(p, fmt, 0)) {
                p->body = BODY_IGNORE;
            }
        }
    } else if (ci_prefix(ct, "text/")) {
        p->body = BODY_TEXT;
    } else {
        fail(p, "WEBHOOK: UNKNOWN REPLY TYPE");
        p->body = BODY_IGNORE;
    }
    return p;
}

bool hook_parser_feed(hook_parser_t *p, const uint8_t *data, size_t n)
{
    if (!ok(p)) {
        return false;
    }
    switch (p->body) {
    case BODY_AUDIO:
        audio_feed(p, data, n);
        break;
    case BODY_TEXT:
    case BODY_JSON:
        if (!reserve((uint8_t **)&p->line, &p->cap, p->len + n + 1, LINE_MAX_BYTES)) {
            fail(p, "WEBHOOK: REPLY TOO LONG");
            break;
        }
        memcpy(p->line + p->len, data, n);
        p->len += n;
        break;
    case BODY_NDJSON:
        for (size_t i = 0; i < n && ok(p); i++) {
            if (data[i] == '\n') {
                handle_json(p, p->line, p->len);
                p->len = 0;
                continue;
            }
            if (!reserve((uint8_t **)&p->line, &p->cap, p->len + 2, LINE_MAX_BYTES)) {
                fail(p, "WEBHOOK: REPLY LINE TOO LONG");
                break;
            }
            p->line[p->len++] = (char)data[i];
        }
        break;
    case BODY_IGNORE:
        break;
    }
    return ok(p);
}

bool hook_parser_finish(hook_parser_t *p)
{
    if (ok(p)) {
        if (p->body == BODY_NDJSON || p->body == BODY_JSON) {
            handle_json(p, p->line, p->len);
        } else if (p->body == BODY_TEXT && p->len && p->sink.reply) {
            p->line[p->len] = '\0';
            p->sink.reply(p->sink.ctx, p->line, false);
        }
        p->len = 0;
    }
    if (ok(p)) {
        audio_end(p);
    }
    return ok(p);
}

size_t hook_parser_frames(const hook_parser_t *p)
{
    return p->frames;
}

void hook_parser_free(hook_parser_t *p)
{
    if (!p) return;
    s_free(p->line);
    s_free(p->held);
    s_free(p->mp3);
    s_free(p->mp3_pcm);
    s_free(p);
}
