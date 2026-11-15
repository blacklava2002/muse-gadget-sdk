# Webhook agent

The gadget can talk to an agent of your own instead of Muse. Each held-button
note becomes one HTTPS POST to a URL you set; the reply is shown and, if it
carries audio, spoken. Put any agent behind it: a script, a local LLM, a
home server reached over a Cloudflare Tunnel.

Build option: `CONFIG_MUSE_WEBHOOK` (on by default where Muse's hatch is).
Muse stays available; the **Agent** setting picks which one a note goes to.

## Set it up

On the gadget: **Settings → Muse → Agent** (tap to switch), **Webhook URL**,
**Webhook token**. Or from `tools/muse/ble_setup.html` (the Agent section), or
over USB serial with setup lines:

```
>webhook.url=https://agent.example.com/gadget
>webhook.token=<your token>
>backend=webhook
>webhook.test
```

Values longer than one BLE write go in pieces: `webhook.url+=` and
`webhook.token+=` append. The URL is up to 255 characters, the token up to
1023. Both are kept in NVS (namespace `muse_hook`) and never logged or echoed
back; the status only says whether a token is set. `backend=muse` switches back.

Live calls (tap to talk) are Muse-only; with the webhook chosen, hold to record
a note. Notes are up to 30 s (`CONFIG_MUSE_WEBHOOK_MAX_NOTE_S`), and the gadget
waits up to 90 s for a reply (`CONFIG_MUSE_WEBHOOK_TIMEOUT_S`).

## Request

```
POST <url>
Content-Type: audio/wav            (16 kHz, mono, 16-bit PCM: the whole note)
Content-Length: <n>
Authorization: Bearer <token>      (when a token is set)
Accept: application/x-ndjson, application/json, audio/mpeg, audio/wav, text/plain
X-Gadget-Protocol: 1
X-Gadget-Device: mcu-wearable
X-Gadget-Turn: <8 hex digits, new each turn>
X-Gadget-Speaker: on | off         (off: audio in the reply is skipped)
```

Two other requests use JSON (`Content-Type: application/json`):

- `{"type":"ping"}` from the settings' test. Any 2xx means it works.
- `{"type":"text","text":"..."}` from a typed console message (`>chat=` over
  USB). Its reply is printed, not spoken.

## Reply

Status: any 2xx is a reply; 204 means "nothing to say". 401/403 show as
"WEBHOOK REFUSED THE TOKEN", anything else as "WEBHOOK SAID HTTP n".

The body must have a `Content-Length` or use chunked transfer encoding (most
frameworks do one or the other). Pick a `Content-Type`:

**`application/json`**, one object, all optional:

```json
{
  "heard": "what the agent heard",
  "reply": "what it says back",
  "audio": {"format": "mp3", "data_base64": "..."},
  "error": "shown instead, as WEBHOOK: <error>"
}
```

**`application/x-ndjson`**, one event per line, for replies that arrive over
time (send `heard` as soon as you have it, then pieces of `reply`):

```
{"type":"heard","text":"turn off the lights"}
{"type":"reply","text":"Done, "}
{"type":"reply","text":"all three are off."}
{"type":"audio","format":"mp3","data_base64":"..."}
{"type":"done"}
```

`reply` pieces append; `"replace": true` starts the text over. Several `audio`
events of the same format are one stream, cut anywhere. `audio_end` closes a
stream, `error` (`text`) ends the turn with that message, and unknown types are
ignored.

**`audio/mpeg`** or **`audio/wav`**: the spoken reply itself, with its text,
if any, in an `X-Reply-Text` header (percent-encoded UTF-8).

**`text/plain`**: the reply text.

Audio formats: `mp3` (any rate, mono or stereo), `wav` (16-bit PCM, 1 or 2
channels, any rate), `pcm16` (raw little-endian mono, with `"rate"`). All are
resampled to 16 kHz. Keep ID3 tags small: more than 64 KB before the first
frame fails. Without audio, or with the speaker off, the text is shown at
reading pace.

## Receiver

`tools/webhook/receiver.py` is a standard-library server for this contract,
with your own speech-to-text, agent and text-to-speech commands plugged in.
See `tools/webhook/README.md`.

## Code

| File | What |
| --- | --- |
| `components/muse/muse_backend.c/.h` | The Agent choice, URL and token, and the `muse_hatch_*` API routed to Muse or the webhook |
| `components/muse/muse_backend_rename.h` | Renames Muse's own session to `muse_hatch_vm_*` at compile time |
| `components/muse/muse_chat_webhook.c/.h` | The HTTP turn: request, reply stream, captions, status |
| `components/muse/muse_webhook_proto.c/.h` | The reply parser and audio decoding (portable; `tests/test_muse_webhook_proto.py`) |
