#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A small receiver for the gadget's webhook backend (docs/webhook.md).

Standard library only. Run it on your own machine and put it on the internet
with a Cloudflare Tunnel (or anything else that forwards HTTPS to it):

    export GADGET_WEBHOOK_TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
    python3 receiver.py --agent-cmd 'my-agent --once'
    cloudflared tunnel --url http://127.0.0.1:8787

Each turn runs up to three commands of your own, none through a shell:

  --stt-cmd    speech to text. {audio} is replaced with the note's WAV path;
               prints the words. Leave it out and your agent gets the audio.
  --agent-cmd  your agent. Gets the words on stdin and GADGET_AUDIO,
               GADGET_TURN, GADGET_SOURCE (gadget, ring or text) in its
               environment; prints the reply. Default: a built-in echo.
  --tts-cmd    text to speech. Gets the reply on stdin; {out} is replaced with
               a path to write a WAV or MP3 to. Leave it out for text only.

Or pass --agent-module my_agent:handle: handle(turn: dict) returns the reply
text, or a dict with "heard", "reply" and/or "audio" (bytes of WAV or MP3).

The gadget's own POSTs (audio/wav, or JSON {"type":"text"|"ping"}) get a reply
it can show and speak. With --stream, the words heard go back as soon as
they're known (NDJSON). A Pebble Index 01 webhook POST (multipart, with its own
transcription) runs the same agent; the ring ignores the reply.

The token is read from the environment only, never from a file or a flag, so
it doesn't end up in shell history or `ps`.
"""
from __future__ import annotations

import argparse
import base64
import email.parser
import email.policy
import hmac
import importlib
import json
import logging
import os
import secrets
import shlex
import subprocess
import sys
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

TOKEN_ENV = 'GADGET_WEBHOOK_TOKEN'
MAX_BODY = 16 * 1024 * 1024
log = logging.getLogger('receiver')


class TurnError(Exception):
    """Shown on the gadget as WEBHOOK: <message>."""


# ---- The pipeline ----

def run_cmd(template: str, *, stdin: str = '', subs: dict[str, str] | None = None,
            env: dict[str, str] | None = None, timeout: float = 120) -> str:
    args = shlex.split(template)
    for key, value in (subs or {}).items():
        args = [a.replace('{' + key + '}', value) for a in args]
    try:
        done = subprocess.run(args, input=stdin, capture_output=True, text=True, timeout=timeout,
                              env={**os.environ, **(env or {})})
    except FileNotFoundError:
        raise TurnError(f'{args[0]} not found')
    except subprocess.TimeoutExpired:
        raise TurnError(f'{Path(args[0]).name} timed out')
    if done.returncode != 0:
        log.warning('%s exited %d: %s', args[0], done.returncode, done.stderr.strip()[-500:])
        raise TurnError(f'{Path(args[0]).name} failed')
    return done.stdout.strip()


def sniff_audio(data: bytes) -> str | None:
    if data[:4] == b'RIFF' and data[8:12] == b'WAVE':
        return 'wav'
    if data[:3] == b'ID3' or (len(data) > 1 and data[0] == 0xFF and data[1] & 0xE0 == 0xE0):
        return 'mp3'
    return None


class Pipeline:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.module = None
        if args.agent_module:
            name, _, func = args.agent_module.partition(':')
            self.module = getattr(importlib.import_module(name), func or 'handle')

    def heard(self, turn: dict) -> str:
        if turn.get('text') is not None:
            return turn['text']
        if self.args.stt_cmd and turn.get('audio_path'):
            return run_cmd(self.args.stt_cmd, subs={'audio': turn['audio_path']}, timeout=self.args.timeout)
        return ''

    def answer(self, turn: dict) -> dict:
        """turn: source, turn_id, heard, audio_path. Returns heard/reply/audio."""
        if self.module:
            out = self.module(dict(turn))
            out = {'reply': out} if isinstance(out, str) else dict(out or {})
        elif self.args.agent_cmd:
            out = {'reply': run_cmd(self.args.agent_cmd, stdin=turn['heard'], timeout=self.args.timeout, env={
                'GADGET_AUDIO': turn.get('audio_path') or '',
                'GADGET_TURN': turn['turn_id'],
                'GADGET_SOURCE': turn['source'],
            })}
        else:
            out = {'reply': f"You said: {turn['heard']}" if turn['heard'] else 'I got your note.'}
        if 'audio' not in out and self.args.tts_cmd and out.get('reply') and turn.get('speak'):
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'reply.audio'
                run_cmd(self.args.tts_cmd, stdin=out['reply'], subs={'out': str(path)}, timeout=self.args.timeout)
                if path.exists():
                    out['audio'] = path.read_bytes()
        return out


# ---- HTTP ----

class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'gadget-receiver/1'
    pipeline: Pipeline
    token: str | None
    stream: bool

    def log_message(self, fmt, *a):  # quieter, and never logs headers
        log.info('%s %s', self.address_string(), fmt % a)

    # Replies always carry a Content-Length or are chunked: the gadget's HTTP
    # client needs one or the other to know where the body ends.
    def send_json(self, status: int, obj: dict, close: bool = False) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        if close:   # refused before the request body was read: don't read it as the next request
            self.send_header('Connection', 'close')
            self.close_connection = True
        self.end_headers()
        self.wfile.write(body)

    def start_chunked(self) -> None:
        self.send_response(200)
        self.send_header('Content-Type', 'application/x-ndjson')
        self.send_header('Transfer-Encoding', 'chunked')
        self.send_header('Cache-Control', 'no-cache')
        self.end_headers()

    def chunk(self, obj: dict | None) -> None:
        data = b'' if obj is None else (json.dumps(obj, ensure_ascii=False) + '\n').encode()
        self.wfile.write(b'%x\r\n%s\r\n' % (len(data), data))
        self.wfile.flush()

    def authorized(self) -> bool:
        if self.token is None:
            return True
        got = self.headers.get('Authorization', '')
        want = 'Bearer ' + self.token
        return hmac.compare_digest(got.encode(), want.encode())

    def do_GET(self):
        self.send_json(200, {'ok': True, 'service': 'gadget-receiver'})

    def do_POST(self):
        if not self.authorized():
            self.send_json(401, {'error': 'bad token'}, close=True)
            return
        try:
            length = int(self.headers.get('Content-Length', '-1'))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            self.send_json(411 if length < 0 else 413, {'error': 'need a Content-Length under 16 MB'}, close=True)
            return
        body = self.rfile.read(length)
        ctype = (self.headers.get('Content-Type') or '').split(';')[0].strip().lower()
        turn_id = self.headers.get('X-Gadget-Turn') or secrets.token_hex(4)
        speak = (self.headers.get('X-Gadget-Speaker') or 'on').lower() != 'off'

        with tempfile.TemporaryDirectory() as tmp:
            turn = {'turn_id': turn_id, 'speak': speak, 'audio_path': None, 'text': None}
            if ctype in ('audio/wav', 'audio/x-wav', 'audio/wave'):
                turn['source'] = 'gadget'
                path = Path(tmp) / 'note.wav'
                path.write_bytes(body)
                turn['audio_path'] = str(path)
            elif ctype == 'application/json':
                try:
                    msg = json.loads(body or b'{}')
                except ValueError:
                    self.send_json(400, {'error': 'bad JSON'})
                    return
                if msg.get('type') == 'ping':
                    self.send_json(200, {'reply': 'pong'})
                    return
                turn['source'] = 'text'
                turn['text'] = str(msg.get('text', ''))
                turn['speak'] = False
            elif ctype == 'multipart/form-data':
                turn.update(self.pebble(body, Path(tmp)))
                turn['speak'] = False
            else:
                self.send_json(415, {'error': f'unsupported Content-Type {ctype or "(none)"}'})
                return
            self.run_turn(turn)

    def pebble(self, body: bytes, tmp: Path) -> dict:
        """Pebble Index 01: audio (m4a), transcription, recordedAt, client."""
        head = f'Content-Type: {self.headers.get("Content-Type")}\r\n\r\n'.encode()
        msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(head + body)
        turn = {'source': 'ring', 'text': ''}
        for part in msg.iter_parts():
            name = part.get_param('name', header='content-disposition')
            if name == 'transcription':
                turn['text'] = part.get_content() if part.get_content_maintype() == 'text' else \
                    part.get_payload(decode=True).decode('utf-8', 'replace')
            elif name == 'audio':
                path = tmp / 'note.m4a'
                path.write_bytes(part.get_payload(decode=True) or b'')
                turn['audio_path'] = str(path)
        return turn

    def run_turn(self, turn: dict) -> None:
        t0 = time.monotonic()
        pipe = self.pipeline
        streaming = self.stream and turn['source'] == 'gadget'
        try:
            if streaming:
                self.start_chunked()
            turn['heard'] = pipe.heard(turn)
            if streaming and turn['heard']:
                self.chunk({'type': 'heard', 'text': turn['heard']})
            out = pipe.answer(turn)
        except TurnError as e:
            log.warning('turn %s: %s', turn['turn_id'], e)
            if streaming:
                self.chunk({'type': 'error', 'text': str(e)})
                self.chunk(None)
            else:
                self.send_json(200, {'error': str(e)})
            return
        except Exception:  # noqa: BLE001 - the gadget should hear something either way
            log.exception('turn %s failed', turn['turn_id'])
            if streaming:
                self.chunk({'type': 'error', 'text': 'receiver error'})
                self.chunk(None)
            else:
                self.send_json(500, {'error': 'receiver error'})
            return

        heard = out.get('heard', turn['heard'])
        reply = out.get('reply', '')
        audio = out.get('audio') if turn['speak'] else None
        fmt = sniff_audio(audio) if audio else None
        if audio and not fmt:
            log.warning('turn %s: TTS output is neither WAV nor MP3; sending text only', turn['turn_id'])
            audio = None
        log.info('turn %s (%s) in %.1fs: heard %d chars, reply %d chars%s', turn['turn_id'], turn['source'],
                 time.monotonic() - t0, len(heard), len(reply), f', {fmt} {len(audio)} bytes' if audio else '')

        if streaming:
            if reply:
                self.chunk({'type': 'reply', 'text': reply})
            if audio:
                for i in range(0, len(audio), 48 * 1024):
                    self.chunk({'type': 'audio', 'format': fmt,
                                'data_base64': base64.b64encode(audio[i:i + 48 * 1024]).decode()})
            self.chunk({'type': 'done'})
            self.chunk(None)
            return
        obj = {'heard': heard, 'reply': reply}
        if audio:
            obj['audio'] = {'format': fmt, 'data_base64': base64.b64encode(audio).decode()}
        self.send_json(200, obj)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--host', default='127.0.0.1', help='default 127.0.0.1: the tunnel connects locally')
    ap.add_argument('--port', type=int, default=8787)
    ap.add_argument('--stt-cmd', help="e.g. 'whisper-cli -m ggml-base.en.bin -nt -np -f {audio}'")
    ap.add_argument('--agent-cmd', help='your agent: words on stdin, reply on stdout')
    ap.add_argument('--agent-module', help='module:function to call instead of --agent-cmd')
    ap.add_argument('--tts-cmd', help="e.g. 'piper --model en_US-amy-medium.onnx --output_file {out}'")
    ap.add_argument('--stream', action='store_true', help='send the words heard back first (NDJSON)')
    ap.add_argument('--timeout', type=float, default=80, help='seconds per command (the gadget waits 90)')
    ap.add_argument('--no-auth', action='store_true', help='accept requests without a token (local tests only)')
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')

    token = os.environ.get(TOKEN_ENV)
    if not token and not args.no_auth:
        print(f'Set {TOKEN_ENV} first (or pass --no-auth for a local test).', file=sys.stderr)
        return 2
    Handler.token = None if args.no_auth else token
    Handler.pipeline = Pipeline(args)
    Handler.stream = args.stream
    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    log.info('listening on http://%s:%d%s', args.host, args.port, ' (no auth)' if args.no_auth else '')
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
