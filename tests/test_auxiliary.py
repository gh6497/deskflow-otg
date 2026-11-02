"""Hermetic scrcpy handshake/lifecycle test. No adb, USB, Bluetooth or sound card."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

FAKE_ADB = r'''
import os, pathlib, socket, struct, sys, time
args = sys.argv[1:]
root = pathlib.Path(os.environ['OTG_TEST_AUX_ROOT'])
target = os.environ.get('OTG_TEST_AUX_TARGET', 'FAKE_PHONE')
if args[0] == '-s':
    assert args[:2] == ['-s', target]
    args = args[2:]
else:
    assert args == ['connect', target]
with (root / 'calls').open('a') as f:
    f.write(' '.join(args) + '\n')
scenario = os.environ.get('OTG_TEST_AUX_WIRELESS', '')
if args[0] == 'get-state':
    ready = scenario == 'online' or (root / 'connected').exists()
    if scenario in ('offline', 'unauthorized', 'timeout', 'connect-fail'):
        ready = False
    print('device' if ready else 'unauthorized' if scenario == 'unauthorized' else 'offline')
    # A process success alone is not proof that the target is online.
    sys.exit(0)
if args[0] == 'connect':
    assert scenario != 'online'
    if scenario == 'timeout':
        time.sleep(30)
    if scenario == 'connect-fail':
        print('failed to connect', file=sys.stderr)
        sys.exit(1)
    if scenario not in ('offline', 'unauthorized'):
        (root / 'connected').touch()
    print('connected to ' + target)
    sys.exit(0)
if args[0] == 'push':
    if scenario:
        assert scenario == 'online' or (root / 'connected').exists()
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
cases = [(mode, "", "FAKE_PHONE") for mode in modes]
for scenario in ("online", "connect", "connect-fail", "offline", "unauthorized", "timeout"):
    cases.append(("clipboard", scenario, "192.0.2.1:40195"))
cases.append(("clipboard", "connect", "[::1]:5555"))
if audio_enabled == "ON":
    cases += [(mode, "connect", "phone.local:5555") for mode in ("audio", "both")]
for mode, scenario, target in cases:
    with tempfile.TemporaryDirectory(prefix="otg-aux-") as directory:
        root = Path(directory)
        adb = root / "fake-adb"
        adb.write_text(f"#!{sys.executable}\n" + FAKE_ADB)
        adb.chmod(0o700)
        env = dict(os.environ, OTG_TEST_AUX_ROOT=directory)
        env["OTG_TEST_AUX_TARGET"] = target
        env["OTG_TEST_AUX_WIRELESS"] = scenario
        wireless_failure = scenario in ("connect-fail", "offline", "unauthorized", "timeout")
        if wireless_failure:
            env["OTG_TEST_AUX_EXPECT_FAIL"] = "1"
        if mode == "fail":
            env["OTG_TEST_AUX_FAIL"] = "1"
        completed = subprocess.run([helper, str(adb), mode, target], env=env,
                                   check=True, timeout=15, capture_output=True, text=True)
        if wireless_failure:
            assert 'wireless debugging connection port' in completed.stderr, completed
        if scenario == 'connect':
            assert 'connected to ' + target in completed.stderr, completed
            assert 'connected to ' + target not in completed.stdout, completed
        calls = (root / "calls").read_text().splitlines()
        if scenario:
            assert calls[0] == 'get-state', calls
            if scenario == 'online':
                assert calls[1].startswith('push '), calls
            else:
                assert calls[1] == 'connect ' + target, calls
                if scenario not in ('connect-fail', 'timeout'):
                    assert calls[2] == 'get-state', calls
        else:
            assert not any(line.startswith(('get-state', 'connect ')) for line in calls), calls
        assert sum(line.startswith('connect ') for line in calls) == bool(scenario and scenario != 'online'), calls
        assert sum(line.startswith("push ") for line in calls) == (not wireless_failure), calls
        started = mode != "fail" and not wireless_failure
        assert sum(line.startswith("shell ") for line in calls) == started, calls
        assert sum(line.startswith("forward tcp:") for line in calls) == started, calls
        assert sum(line.startswith("forward --remove ") for line in calls) == started, calls
        assert not any(line.startswith(('disconnect', 'pair ', 'kill-server')) for line in calls), calls
        assert not (root / "port").exists(), calls
print("PASS: fake ADB lifecycle modes:", ", ".join(modes), "and wireless readiness/connect/failure/timeout")
