/* SPDX-License-Identifier: Apache-2.0 */
/* Host test of the webhook reply parser (components/muse/muse_webhook_proto.c).
 * argv[1]: an MP3 file (components/muse/test_reply.mp3). */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minimp3.h"
#include "muse_webhook_proto.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    char heard[256];
    char reply[1024];
    char error[128];
    int replies, errors;
    int16_t *pcm;
    size_t frames, cap;
    size_t stop_after;   /* 0: never */
    int pcm_calls;
} sink_t;

static void on_heard(void *c, const char *t) { snprintf(((sink_t *)c)->heard, 256, "%s", t); }
static void on_reply(void *c, const char *t, bool append)
{
    sink_t *s = c;
    if (!append) s->reply[0] = '\0';
    strncat(s->reply, t, sizeof(s->reply) - strlen(s->reply) - 1);
    s->replies++;
}
static bool on_pcm(void *c, const int16_t *pcm, size_t n)
{
    sink_t *s = c;
    CHECK(n > 0 && n <= 512);
    if (s->frames + n > s->cap) {
        s->cap = (s->frames + n) * 2;
        s->pcm = realloc(s->pcm, s->cap * sizeof(int16_t));
        CHECK(s->pcm);
    }
    memcpy(s->pcm + s->frames, pcm, n * sizeof(int16_t));
    s->frames += n;
    s->pcm_calls++;
    return !(s->stop_after && s->frames >= s->stop_after);
}
static void on_error(void *c, const char *t)
{
    sink_t *s = c;
    snprintf(s->error, sizeof(s->error), "%s", t);
    s->errors++;
}

static hook_sink_t make_sink(sink_t *s)
{
    memset(s, 0, sizeof(*s));
    hook_sink_t k = { s, on_heard, on_reply, on_pcm, on_error, false };
    return k;
}

static uint32_t rng = 12345;
static size_t piece(size_t max) { rng = rng * 1103515245u + 12345u; return 1 + (rng >> 8) % max; }

/* Feeds in random pieces of 1..max bytes (max 0: all at once). */
static bool feed(hook_parser_t *p, const void *data, size_t n, size_t max)
{
    const uint8_t *d = data;
    while (n) {
        size_t k = max ? piece(max) : n;
        if (k > n) k = n;
        if (!hook_parser_feed(p, d, k)) return false;
        d += k;
        n -= k;
    }
    return true;
}

static bool run(sink_t *s, const char *ct, const char *rt, const void *data, size_t n, size_t max, bool skip)
{
    hook_sink_t k = make_sink(s);
    k.skip_audio = skip;
    hook_parser_t *p = hook_parser_new(&k, ct, rt);
    CHECK(p);
    bool okay = feed(p, data, n, max);
    okay = hook_parser_finish(p) && okay;
    CHECK(hook_parser_frames(p) == s->frames);
    hook_parser_free(p);
    return okay;
}

static void sink_free(sink_t *s) { free(s->pcm); s->pcm = NULL; }

/* ---- Test data ---- */

static int16_t *sine(uint32_t rate, int channels, double secs, double hz, size_t *frames)
{
    size_t n = (size_t)(rate * secs);
    int16_t *x = malloc(n * channels * sizeof(int16_t));
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < channels; c++)
            x[i * channels + c] = (int16_t)(12000 * sin(2 * M_PI * hz * i / rate));
    *frames = n;
    return x;
}

static uint8_t *wav(const int16_t *x, size_t frames, uint32_t rate, int channels, int bits, bool list_chunk, size_t *len)
{
    size_t data = frames * channels * 2, extra = list_chunk ? 8 + 5 + 1 : 0;
    uint8_t *b = calloc(1, 44 + extra + data);
    uint8_t *h = b;
    memcpy(h, "RIFF", 4); uint32_t riff = (uint32_t)(36 + extra + data); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); uint32_t sixteen = 16; memcpy(h + 16, &sixteen, 4);
    uint16_t one = 1, ch = (uint16_t)channels, b16 = (uint16_t)bits, align = (uint16_t)(channels * bits / 8);
    uint32_t byterate = rate * align;
    memcpy(h + 20, &one, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &byterate, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &b16, 2);
    size_t at = 36;
    if (list_chunk) {   /* an odd-sized chunk before data: padded to even */
        memcpy(h + at, "LIST", 4); uint32_t five = 5; memcpy(h + at + 4, &five, 4); memcpy(h + at + 8, "hello", 5);
        at += 8 + 5 + 1;
    }
    memcpy(h + at, "data", 4); uint32_t d32 = (uint32_t)data; memcpy(h + at + 4, &d32, 4);
    memcpy(h + at + 8, x, data);
    *len = at + 8 + data;
    return b;
}

static char *b64(const uint8_t *d, size_t n, bool url)
{
    const char *t = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = malloc(n / 3 * 4 + 8), *w = o;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | d[i + 1] << 8 | d[i + 2];
        *w++ = t[v >> 18]; *w++ = t[v >> 12 & 63]; *w++ = t[v >> 6 & 63]; *w++ = t[v & 63];
    }
    if (n - i == 1) { uint32_t v = (uint32_t)d[i] << 16; *w++ = t[v >> 18]; *w++ = t[v >> 12 & 63]; if (!url) { *w++ = '='; *w++ = '='; } }
    if (n - i == 2) { uint32_t v = (uint32_t)d[i] << 16 | d[i + 1] << 8; *w++ = t[v >> 18]; *w++ = t[v >> 12 & 63]; *w++ = t[v >> 6 & 63]; if (!url) *w++ = '='; }
    *w = '\0';
    return o;
}

/* Rising zero crossings per second, as a rough frequency. */
static double freq(const int16_t *x, size_t n, uint32_t rate)
{
    int up = 0;
    for (size_t i = 1; i < n; i++) if (x[i - 1] < 0 && x[i] >= 0) up++;
    return up * (double)rate / n;
}

static bool near(double a, double b, double tol) { return fabs(a - b) <= tol; }

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len);
    CHECK(fread(b, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    *n = (size_t)len;
    return b;
}

/* ---- Tests ---- */

static void test_json(void)
{
    size_t frames;
    int16_t *x = sine(16000, 1, 0.5, 440, &frames);
    char *data = b64((const uint8_t *)x, frames * 2, false);
    char *body = malloc(strlen(data) + 256);
    sprintf(body, "{\"heard\":\"what time is it\",\"reply\":\"It's \\u00e9 noon.\","
                  "\"audio\":{\"format\":\"pcm16\",\"rate\":16000,\"data_base64\":\"%s\"}}", data);
    sink_t s;
    CHECK(run(&s, "application/json; charset=utf-8", NULL, body, strlen(body), 97, false));
    CHECK(!strcmp(s.heard, "what time is it"));
    CHECK(!strcmp(s.reply, "It's \xc3\xa9 noon."));
    CHECK(s.frames == frames);
    CHECK(!memcmp(s.pcm, x, frames * 2));
    CHECK(s.errors == 0);
    sink_free(&s);

    /* No Content-Type is read as JSON; reply only. */
    CHECK(run(&s, NULL, NULL, "{\"reply\":\"hi\"}", 14, 0, false));
    CHECK(!strcmp(s.reply, "hi") && s.frames == 0);
    sink_free(&s);

    /* An error object */
    CHECK(!run(&s, "application/json", NULL, "{\"error\":\"agent offline\"}", 25, 3, false));
    CHECK(!strcmp(s.error, "WEBHOOK: agent offline") && s.errors == 1);
    sink_free(&s);

    /* Bad JSON, and not an object */
    CHECK(!run(&s, "application/json", NULL, "{\"reply\":", 9, 0, false));
    CHECK(!strcmp(s.error, "WEBHOOK: BAD JSON IN REPLY"));
    sink_free(&s);
    CHECK(!run(&s, "application/json", NULL, "[1,2]", 5, 0, false));
    CHECK(!strcmp(s.error, "WEBHOOK: REPLY IS NOT A JSON OBJECT"));
    sink_free(&s);

    /* Speaker off: the text still comes, no audio */
    CHECK(run(&s, "application/json", NULL, body, strlen(body), 0, true));
    CHECK(!strcmp(s.reply, "It's \xc3\xa9 noon.") && s.frames == 0 && s.pcm_calls == 0);
    sink_free(&s);

    free(body);
    free(data);
    free(x);
    puts("json ok");
}

static void test_ndjson(void)
{
    size_t frames;
    int16_t *x = sine(24000, 1, 1.0, 300, &frames);
    size_t half = frames / 2 * 2;   /* bytes in the first half */
    char *a = b64((const uint8_t *)x, half, true);
    char *b = b64((const uint8_t *)x + half, frames * 2 - half, false);
    char *body = malloc(strlen(a) + strlen(b) + 1024);
    sprintf(body,
            "{\"type\":\"heard\",\"text\":\"tell me a joke\"}\r\n"
            "{\"type\":\"status\",\"text\":\"thinking\"}\n"
            "\n"
            "{\"type\":\"reply\",\"text\":\"Wrong \"}\n"
            "{\"type\":\"reply\",\"text\":\"Why did \",\"replace\":true}\n"
            "{\"type\":\"reply\",\"text\":\"the chicken...\"}\n"
            "{\"type\":\"audio\",\"format\":\"pcm16\",\"rate\":24000,\"data_base64\":\"%s\"}\n"
            "{\"type\":\"audio\",\"format\":\"pcm16\",\"rate\":24000,\"data_base64\":\"%s\"}\n"
            "{\"type\":\"done\"}", a, b);   /* no newline after the last line */
    for (size_t max = 1; max <= 4096; max *= 8) {
        sink_t s;
        CHECK(run(&s, "application/x-ndjson", NULL, body, strlen(body), max, false));
        CHECK(!strcmp(s.heard, "tell me a joke"));
        CHECK(!strcmp(s.reply, "Why did the chicken..."));
        CHECK(s.errors == 0);
        size_t want = frames * 16000 / 24000;
        CHECK(s.frames + 2 >= want && s.frames <= want + 2);
        CHECK(near(freq(s.pcm, s.frames, 16000), 300, 3));
        sink_free(&s);
    }

    /* An error event stops it */
    sink_t s;
    const char *err = "{\"type\":\"reply\",\"text\":\"one\"}\n{\"type\":\"error\",\"text\":\"rate limited\"}\n{\"type\":\"reply\",\"text\":\" two\"}\n";
    CHECK(!run(&s, "application/x-ndjson", NULL, err, strlen(err), 5, false));
    CHECK(!strcmp(s.reply, "one") && !strcmp(s.error, "WEBHOOK: rate limited"));
    sink_free(&s);

    /* An unknown audio format */
    const char *fmt = "{\"type\":\"audio\",\"format\":\"opus\",\"data_base64\":\"AAAA\"}\n";
    CHECK(!run(&s, "application/x-ndjson", NULL, fmt, strlen(fmt), 0, false));
    CHECK(!strcmp(s.error, "WEBHOOK: UNKNOWN AUDIO FORMAT"));
    sink_free(&s);

    free(body);
    free(a);
    free(b);
    free(x);
    puts("ndjson ok");
}

static void test_wav(void)
{
    static const struct { uint32_t rate; int ch; bool list; } cases[] = {
        { 16000, 1, false }, { 22050, 2, true }, { 44100, 1, false }, { 8000, 1, true }, { 48000, 2, false },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        size_t frames, len;
        int16_t *x = sine(cases[c].rate, cases[c].ch, 0.75, 500, &frames);
        uint8_t *w = wav(x, frames, cases[c].rate, cases[c].ch, 16, cases[c].list, &len);
        for (size_t max = 1; max <= 8192; max *= 16) {
            sink_t s;
            CHECK(run(&s, "audio/wav", "Here%27s%20the%20tone.", w, len, max, false));
            CHECK(!strcmp(s.reply, "Here's the tone."));
            size_t want = (size_t)((double)frames * 16000 / cases[c].rate);
            CHECK(s.frames + 2 >= want && s.frames <= want + 2);
            CHECK(near(freq(s.pcm, s.frames, 16000), 500, 4));
            if (cases[c].rate == 16000) CHECK(!memcmp(s.pcm, x, frames * 2));
            sink_free(&s);
        }
        free(w);
        free(x);
    }

    /* 8-bit, and not a WAV at all */
    size_t frames, len;
    int16_t *x = sine(16000, 1, 0.1, 500, &frames);
    uint8_t *w = wav(x, frames, 16000, 1, 8, false, &len);
    sink_t s;
    CHECK(!run(&s, "audio/x-wav", NULL, w, len, 0, false));
    CHECK(!strcmp(s.error, "WEBHOOK: WAV MUST BE 16-BIT PCM"));
    sink_free(&s);
    memcpy(w, "RIFX", 4);
    CHECK(!run(&s, "audio/wav", NULL, w, len, 0, false));
    CHECK(!strcmp(s.error, "WEBHOOK: AUDIO IS NOT A WAV"));
    sink_free(&s);
    free(w);
    free(x);
    puts("wav ok");
}

static void test_mp3(const char *path)
{
    size_t n;
    uint8_t *mp3 = slurp(path, &n);

    /* What minimp3 makes of it in one go, resampled by the same rule */
    mp3dec_t dec;
    mp3dec_init(&dec);
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t off = 0, total = 0;
    int hz = 0;
    while (off < n) {
        mp3dec_frame_info_t info;
        int k = mp3dec_decode_frame(&dec, mp3 + off, (int)(n - off), pcm, &info);
        if (!info.frame_bytes) break;
        off += info.frame_bytes;
        total += (size_t)k;
        if (k) hz = info.hz;
    }
    CHECK(total > 0 && hz > 0);
    size_t want = (size_t)((double)total * 16000 / hz);

    size_t first = 0;
    for (size_t max = 1; max <= 65536; max *= 4) {
        sink_t s;
        CHECK(run(&s, "audio/mpeg", "Hi+there%2C%20you", mp3, n, max, false));
        CHECK(!strcmp(s.reply, "Hi there, you"));
        CHECK(s.errors == 0);
        CHECK(s.frames + 2 >= want && s.frames <= want + 2);
        if (!first) first = s.frames;
        CHECK(s.frames == first);   /* the same, however it's cut up */
        sink_free(&s);
    }

    /* Inside NDJSON, cut across events */
    size_t cut = n / 3;
    char *a = b64(mp3, cut, false), *b = b64(mp3 + cut, n - cut, false);
    char *body = malloc(strlen(a) + strlen(b) + 256);
    sprintf(body, "{\"type\":\"audio\",\"format\":\"mp3\",\"data_base64\":\"%s\"}\n"
                  "{\"type\":\"audio\",\"format\":\"mp3\",\"data_base64\":\"%s\"}\n"
                  "{\"type\":\"audio_end\"}\n{\"type\":\"done\"}\n", a, b);
    sink_t s;
    CHECK(run(&s, "application/x-ndjson", NULL, body, strlen(body), 333, false));
    CHECK(s.frames == first);
    sink_free(&s);
    free(body);
    free(a);
    free(b);

    /* Stop part way: the sink says no more */
    hook_sink_t k = make_sink(&s);
    s.stop_after = 2000;
    hook_parser_t *p = hook_parser_new(&k, "audio/mpeg", NULL);
    CHECK(!feed(p, mp3, n, 700));
    CHECK(!hook_parser_finish(p));
    CHECK(s.frames >= 2000 && s.frames < 2000 + 512 && s.errors == 0);
    hook_parser_free(p);
    sink_free(&s);

    /* 70 KB of noise is not MP3 */
    uint8_t *junk = malloc(70000);
    for (size_t i = 0; i < 70000; i++) junk[i] = (uint8_t)(i * 7 + 3);
    CHECK(!run(&s, "audio/mpeg", NULL, junk, 70000, 4096, false));
    CHECK(!strcmp(s.error, "WEBHOOK: AUDIO IS NOT MP3") && s.frames == 0);
    sink_free(&s);
    free(junk);
    free(mp3);
    puts("mp3 ok");
}

static void test_text_and_types(void)
{
    sink_t s;
    CHECK(run(&s, "text/plain; charset=utf-8", NULL, "Sure, I'll remind you.", 22, 4, false));
    CHECK(!strcmp(s.reply, "Sure, I'll remind you.") && s.replies == 1);
    sink_free(&s);

    CHECK(!run(&s, "application/octet-stream", NULL, "xx", 2, 0, false));
    CHECK(!strcmp(s.error, "WEBHOOK: UNKNOWN REPLY TYPE"));
    sink_free(&s);

    CHECK(!run(&s, "audio/ogg", NULL, "OggS", 4, 0, false));
    CHECK(!strcmp(s.error, "WEBHOOK: UNKNOWN AUDIO FORMAT"));
    sink_free(&s);

    char t[] = "a%2Fb+c%zz%4";
    hook_percent_decode(t);
    CHECK(!strcmp(t, "a/b c%zz%4"));
    puts("text ok");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    test_json();
    test_ndjson();
    test_wav();
    test_mp3(argv[1]);
    test_text_and_types();
    puts("ok");
    return 0;
}
