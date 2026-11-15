# Webhook receiver

`receiver.py` answers the gadget's webhook backend (`docs/webhook.md`) with an
agent of your own. Python 3.9+, standard library only.

## Run it

```sh
export GADGET_WEBHOOK_TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
python3 receiver.py \
  --stt-cmd 'whisper-cli -m ggml-base.en.bin -nt -np -f {audio}' \
  --agent-cmd 'my-agent --once' \
  --tts-cmd 'piper --model en_US-amy-medium.onnx --output_file {out}'
```

The token comes from the environment only, so keep it in your shell profile,
a systemd `EnvironmentFile` with mode 600, or a secrets manager, not in a
script. Give the gadget the same token (Settings → Muse → Webhook token, or
`>webhook.token=` over USB).

Each part is optional:

- `--stt-cmd`: `{audio}` becomes the note's WAV path; print the words.
  Without it, your agent gets an empty string on stdin and the audio path in
  `GADGET_AUDIO`.
- `--agent-cmd`: the words on stdin; `GADGET_AUDIO`, `GADGET_TURN` and
  `GADGET_SOURCE` (`gadget`, `ring` or `text`) in its environment; print the
  reply. Without it, a built-in echo answers, which is handy for a first test.
- `--agent-module pkg.mod:handle`: a Python function instead, given a dict
  (`source`, `turn_id`, `heard`, `audio_path`) and returning the reply text or
  a dict with `heard`, `reply` and `audio` (WAV or MP3 bytes).
- `--tts-cmd`: the reply on stdin; write a WAV or MP3 to `{out}`. Without it,
  the gadget shows the text.
- `--stream`: send what was heard back first, then the reply (NDJSON), so the
  gadget shows your words while the agent thinks.

Commands run without a shell, each with `--timeout` seconds (default 80; the
gadget waits 90).

## Put it online with a Cloudflare Tunnel

Quick test, no account needed (the URL changes every run):

```sh
cloudflared tunnel --url http://127.0.0.1:8787
```

For good, with your own hostname:

```sh
cloudflared tunnel login
cloudflared tunnel create gadget
cloudflared tunnel route dns gadget agent.example.com
cat > ~/.cloudflared/config.yml <<'EOF'
tunnel: gadget
credentials-file: /home/you/.cloudflared/<tunnel-id>.json
ingress:
  - hostname: agent.example.com
    service: http://127.0.0.1:8787
  - service: http_status:404
EOF
cloudflared tunnel run gadget
```

Then set the gadget's Webhook URL to `https://agent.example.com/` and run the
settings' test. The receiver listens on 127.0.0.1 only; the tunnel is the way
in, and the bearer token is the lock. Cloudflare Access service tokens don't
fit here: the gadget sends one `Authorization` header and nothing else.

## Pebble Index 01

The ring's webhook (an advanced feature in the Pebble app) can point at the
same receiver: set the URL and add a header `Authorization: Bearer <token>`. Its
POST (multipart: `audio` as M4A, `transcription`, `recordedAt`, `client`)
runs your agent with the ring's transcription and `GADGET_SOURCE=ring`. The
ring doesn't play a reply, so have the agent act or notify you some other way.

## Check it

```sh
python3 receiver.py --no-auth &
curl -s -H 'Content-Type: application/json' -d '{"type":"text","text":"hello"}' http://127.0.0.1:8787/
```
