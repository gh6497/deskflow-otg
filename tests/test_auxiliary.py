"""Hermetic scrcpy handshake/lifecycle test. No adb, USB, Bluetooth or sound card."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

FAKE_ADB = r'''
import os, pathlib, socket, struct, sys, time
args = sys.argv[1:]
assert args[:2] == ['-s', 'FAKE_PHONE']
args = args[2:]
root = pathlib.Path(os.environ['OTG_TEST_AUX_ROOT'])
with (root / 'calls').open('a') as f:
    f.write(args[0] + ' ' + ' '.join(args[1:]) + '\n')
if args[0] == 'push':
    sys.exit(1 if os.environ.get('OTG_TEST_AUX_FAIL') else 0)
if args[0] == 'forward':
    if args[1] == '--remove':
        (root / 'port').unlink(missing_ok=True)
    else:
        assert args[2].startswith('localabstract:scrcpy_')
        (root / 'port').write_text(args[1].split(':')[1])
    sys.exit(0)
assert args[0] == 'shell'
assert 'video=false' in args and 'send_device_meta=false' in args and 'send_dummy_byte=true' in args
audio = 'audio=true' in args
control = 'control=true' in args
deadline = time.monotonic() + 5
while not (root / 'port').exists():
    assert time.monotonic() < deadline
    time.sleep(.01)
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(('127.0.0.1', int((root / 'port').read_text())))
listener.listen(2)
first, _ = listener.accept()
first.sendall(b'\0')  # Sent before accepting the second socket, like scrcpy.
if audio and control:
    channel, _ = listener.accept()
else:
    channel = first
if audio:
    first.sendall(b'\0\0\0\0')  # Disabled audio; must not prevent control.
listener.close()
def read(n):
    out = b''
    while len(out) < n:
        part = channel.recv(n - len(out))
        if not part:
            return None
        out += part
    return out
if control:
    header = read(14)
    assert header[0] == 9 and header[9] == 0
    assert read(struct.unpack('>I', header[10:14])[0]) == b'computer'
    channel.sendall(b'\x01' + header[1:9] + b'\x00\x00\x00\x00\x07android')
while channel.recv(1):
    pass
'''

helper, audio_enabled = sys.argv[1:]
modes = ["clipboard", "fail"]
if audio_enabled == "ON":
    modes += ["audio", "both"]
for mode in modes:
    with tempfile.TemporaryDirectory(prefix="otg-aux-") as directory:
        root = Path(directory)
        adb = root / "fake-adb"
        adb.write_text(f"#!{sys.executable}\n" + FAKE_ADB)
        adb.chmod(0o700)
        env = dict(os.environ, OTG_TEST_AUX_ROOT=directory)
        if mode == "fail":
            env["OTG_TEST_AUX_FAIL"] = "1"
        subprocess.run([helper, str(adb), mode], env=env, check=True, timeout=15)
        calls = (root / "calls").read_text().splitlines()
        assert sum(line.startswith("push ") for line in calls) == 1, calls
        assert sum(line.startswith("shell ") for line in calls) == (mode != "fail"), calls
        assert sum(line.startswith("forward tcp:") for line in calls) == (mode != "fail"), calls
        assert sum(line.startswith("forward --remove ") for line in calls) == (mode != "fail"), calls
        assert not (root / "port").exists(), calls
print("PASS: fake ADB lifecycle modes:", ", ".join(modes))
