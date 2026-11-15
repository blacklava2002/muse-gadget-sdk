/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* The webhook backend (CONFIG_MUSE_WEBHOOK): the muse_hatch_* turn API of
 * muse_chat.h, answered by an HTTP(S) endpoint of your own. muse_backend.c
 * calls these when the webhook is chosen; see docs/webhook.md. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_chat.h"

#ifdef __cplusplus
extern "C" {
#endif

void muse_hook_start(void);
void muse_hook_status(muse_hatch_status_t *out);
void muse_hook_test(void);
void muse_hook_config_changed(void);
void muse_hook_set_resting(bool resting);
bool muse_hook_ready(void);
void muse_hook_turn_begin(void);
void muse_hook_turn_audio(const int16_t *pcm, size_t frames);
size_t muse_hook_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms);
void muse_hook_turn_end(void);
void muse_hook_turn_cancel(void);
muse_hatch_ev_t muse_hook_turn_event(char *text, size_t cap);
bool muse_hook_turn_caption(size_t played, char *out, size_t cap);
float muse_hook_turn_progress(size_t played, int *message);
size_t muse_hook_turn_read(int16_t *pcm, size_t frames, int wait_ms);
void muse_hook_text_turn(char *text);
void muse_hook_text_cancel(void);

#ifdef __cplusplus
}
#endif
