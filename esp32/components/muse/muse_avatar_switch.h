#pragma once

/*
 * The multi-avatar switcher (avatar/muse_pixel.c is the dispatcher; the
 * implementations live in avatar/impl/). The choice is persisted by
 * muse_settings under the "avatar" key.
 */

#define MUSE_AVATAR_COUNT 5

/* Display names, in switcher order; index 0 is the default. */
extern const char *const muse_avatar_names[MUSE_AVATAR_COUNT];

/* The active avatar's index into muse_avatar_names. */
int muse_pixel_avatar(void);

/* Switch now and remember the choice; the face changes on the next frame. */
void muse_pixel_set_avatar(int idx);
