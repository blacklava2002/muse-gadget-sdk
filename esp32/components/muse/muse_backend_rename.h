/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Force-included (-include) into the Muse backend's own sources when
 * CONFIG_MUSE_WEBHOOK is on: muse_chat.c, muse_chat_session.cpp and, with live
 * calls, muse_voice_call.cpp. Their muse_hatch_* definitions, and their calls
 * to each other, become muse_hatch_vm_*; muse_backend.c then defines the
 * public muse_hatch_* names and hands each call to the chosen backend.
 *
 * Shared helpers in muse_chat_text.c (captions, console lines, WAV header,
 * base64) and muse_hatch_state_name keep their names: both backends use them.
 */

#pragma once

#define muse_hatch_start          muse_hatch_vm_start
#define muse_hatch_status         muse_hatch_vm_status
#define muse_hatch_test           muse_hatch_vm_test
#define muse_hatch_config_changed muse_hatch_vm_config_changed
#define muse_hatch_set_resting    muse_hatch_vm_set_resting
#define muse_hatch_ready          muse_hatch_vm_ready
#define muse_hatch_turn_begin     muse_hatch_vm_turn_begin
#define muse_hatch_turn_audio     muse_hatch_vm_turn_audio
#define muse_hatch_turn_audio_wait muse_hatch_vm_turn_audio_wait
#define muse_hatch_turn_end       muse_hatch_vm_turn_end
#define muse_hatch_turn_cancel    muse_hatch_vm_turn_cancel
#define muse_hatch_turn_event     muse_hatch_vm_turn_event
#define muse_hatch_incoming       muse_hatch_vm_incoming
#define muse_hatch_turn_caption   muse_hatch_vm_turn_caption
#define muse_turn_progress        muse_hatch_vm_turn_progress
#define muse_hatch_turn_read      muse_hatch_vm_turn_read
#define muse_hatch_mp3_selftest   muse_hatch_vm_mp3_selftest
#define muse_hatch_text_turn      muse_hatch_vm_text_turn
#define muse_hatch_text_cancel    muse_hatch_vm_text_cancel
/* muse_chat_priv.h, between muse_chat.c and the session task */
#define muse_hatch_configured     muse_hatch_vm_configured
#define muse_hatch_report         muse_hatch_vm_report
#define muse_hatch_report_vm      muse_hatch_vm_report_vm
#define muse_hatch_chat_connect   muse_hatch_vm_chat_connect
#define muse_hatch_chat_forget    muse_hatch_vm_chat_forget
