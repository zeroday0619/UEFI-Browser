#!/usr/bin/env python3
"""Fetch the pinned host Bazel executable used by Mozc generators."""
import hashlib
from pathlib import Path
import urllib.request

project = Path(__file__).resolve().parents[1]
output = project / 'build/tools/bazel-9.0.2'
expected = '422e7a1690b76d7e615c29091d3aca28d0bd3a93fe3c93cbefb8f72d774926d5'
if not output.exists():
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.download')
    with urllib.request.urlopen('https://releases.bazel.build/9.0.2/release/bazel-9.0.2-linux-x86_64', timeout=90) as response:
        temporary.write_bytes(response.read())
    if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
        temporary.unlink()
        raise SystemExit('Bazel checksum mismatch.')
    temporary.replace(output)
if hashlib.sha256(output.read_bytes()).hexdigest() != expected:
    raise SystemExit('Existing Bazel checksum mismatch.')
output.chmod(0o755)
print(output)
