# SPDX-License-Identifier: Apache-2.0
"""Build the webhook reply parser on the host and run it over JSON, NDJSON, WAV, MP3 and text replies."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))


@unittest.skipUnless((JSON / 'cJSON.c').exists(), 'set CJSON_SOURCE_DIR to a cJSON checkout')
class WebhookProto(unittest.TestCase):
    def test_replies_parse_in_any_pieces(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'webhook_proto'
            built = subprocess.run(
                [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-O1',
                 '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
                 '-I', str(JSON), '-I', str(ROOT / 'components/muse'),
                 '-I', str(ROOT / 'components/minimp3/include'),
                 str(ROOT / 'tests/webhook_proto_harness.c'),
                 str(ROOT / 'components/muse/muse_webhook_proto.c'),
                 str(JSON / 'cJSON.c'),
                 str(ROOT / 'components/minimp3/src/minimp3.c'),
                 '-lm', '-o', str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary), str(ROOT / 'components/muse/test_reply.mp3')],
                                 capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('ok', ran.stdout.splitlines()[-1])


if __name__ == '__main__':
    unittest.main()
