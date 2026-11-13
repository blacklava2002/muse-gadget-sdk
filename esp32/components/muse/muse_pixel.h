/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "muse_state.h"

/*
 * Procedural pixel-art renderer for the Muse character.
 *
 * Muse is drawn on a coarse MUSE_PX_W x MUSE_PX_H grid with a small palette,
 * ordered dithering and hard outlines, then blown up to the screen with
 * nearest-neighbour blocks so the pixels stay chunky.
 */

/*
 * A custom avatar (components/muse/avatar/, see tools/muse/AVATAR_RECIPE.md)
 * may use another grid: the build then defines MUSE_CUSTOM_AVATAR_GRID and
 * avatar/muse_avatar_grid.h sets the values below. The grid stays square: the
 * UI's canvas is. Defaults are the stock 64x64 avatar's.
 */
#ifdef MUSE_CUSTOM_AVATAR_GRID
#include "avatar/muse_avatar_grid.h"
#endif
#ifndef MUSE_PX_W
#define MUSE_PX_W 64
#define MUSE_PX_H 64
#endif
/* Rows at the bottom of the grid the art never reaches (it tucks behind the meter). */
#ifndef MUSE_PX_BLANK_ROWS
#define MUSE_PX_BLANK_ROWS 3
#endif
/* Screen size of Muse over a reply that's heard / read, or 0 for the UI's own rule (cells of the canvas's). */
#ifndef MUSE_PX_HEARD_PX
#define MUSE_PX_HEARD_PX 0
#endif
#ifndef MUSE_PX_MINI_PX
#define MUSE_PX_MINI_PX 0
#endif

typedef struct {
    muse_mode_t mode;
    float t;         /* seconds since boot */
    float mode_t;    /* seconds in current mode */
    float level;     /* 0..1 live audio level */
    float happy;     /* 0..1 pet reaction */
    float working;   /* 0..1: thinking while Muse works on a call (newer-upstream field the
                      * procedural Martha avatar reads; this fork's UI never sets it, so it
                      * stays 0 - added 2026-11-13 so that avatar compiles unmodified) */
    /* Reactions, each 0 when not happening, else 0..1 through it. An avatar
     * that ignores them still renders; the default one does. */
    float dizzy;     /* shaken: wobble with spiral eyes and stars, sit dazed, stand back up (MUSE_DIZZY_S) */
    float sleepy;    /* before the screen goes dark: yawn, eyes droop and close, snore Zzz (MUSE_SLEEPY_S) */
    float waking;    /* the screen came on: quick blinks, a stretch, alert (MUSE_WAKING_S) */
    float tickle;    /* tickled on the screen: startle, then squirm and giggle while it lasts, then
                      * catch his breath (MUSE_TICKLE_S; held below MUSE_TICKLE_HOLD while it continues) */
} muse_pose_t;

#define MUSE_DIZZY_S 4.0f
#define MUSE_SLEEPY_S 4.0f
#define MUSE_WAKING_S 1.5f
#define MUSE_TICKLE_S 3.0f
#define MUSE_TICKLE_HOLD 0.8f   /* the giggle loop runs from ~0.15 to here while tickling goes on */

/* Accent colour of a mode (for the surrounding UI), as 0xRRGGBB. */
uint32_t muse_pixel_accent(muse_mode_t mode);

/* Optional: the accent of the working pose (pose->working), as 0xRRGGBB, so
 * the UI around the avatar matches it. 0, or a renderer without it (the UI
 * links a weak default), keeps THINKING's accent. */
uint32_t muse_pixel_working_accent(void);

/* Render one frame into Muse's own MUSE_PX_W x MUSE_PX_H grid. */
void muse_pixel_render(const muse_pose_t *pose);

/* Size (square, in screen pixels) muse_pixel_scale() blows the grid up to. */
void muse_pixel_set_size(int px);

/*
 * Write screen pixels [x0, x1] x [y0, y1] of the blown-up frame as RGB565,
 * stride_px apart. Cheap enough to call per display strip, so the full-size
 * image never has to exist in RAM.
 */
void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1);
