"""Hermetic scrcpy handshake/lifecycle test. No adb, USB, Bluetooth or sound card."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

FAKE_ADB = r'''
import os, pathlib, select, signal, socket, struct, sys, time
args = sys.argv[1:]
root = pathlib.Path(os.environ['OTG_TEST_AUX_ROOT'])
signal.signal(signal.SIGTERM, signal.SIG_IGN)
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
    ready = not scenario or scenario == 'online' or (root / 'connected').exists()
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
    if os.environ.get('OTG_TEST_AUX_PUSH_TIMEOUT'):
        time.sleep(30)
    retry_file = root / 'push-failures'
    retry_count = int(os.environ.get('OTG_TEST_AUX_PUSH_RETRIES', '0'))
    if retry_count and not retry_file.exists():
        retry_file.write_text('0')
    if retry_count:
        count = int(retry_file.read_text())
        if count < retry_count:
            retry_file.write_text(str(count + 1))
            sys.exit(1)
    sys.exit(1 if os.environ.get('OTG_TEST_AUX_FAIL') else 0)
if args[0] == 'forward':
    if args[1] == '--remove':
        (root / 'port').unlink(missing_ok=True)
    else:
        assert args[2].startswith('localabstract:scrcpy_')
        (root / 'port').write_text(args[1].split(':')[1])
    sys.exit(0)
assert args[0] == 'shell'
if args[1:] == ['cmd', 'media_session', 'dispatch', 'pause']:
    assert (root / 'port').exists() and (root / 'stream-open').exists()
    assert not (root / 'stream-closed').exists()
    (root / 'pause-requested').touch()
    pause = os.environ.get('OTG_TEST_AUX_PAUSE', '')
    # The command must finish before the audio/control streams are closed.
    time.sleep(.15)
    assert not (root / 'stream-closed').exists()
    if pause == 'timeout':
        time.sleep(30)
    if pause == 'fail':
        print('fake media session failure')
        sys.exit(1)
    (root / 'pause').touch()
    print('fake media session pause dispatched')
    sys.exit(0)
assert 'video=false' in args and 'send_device_meta=false' in args and 'send_dummy_byte=true' in args
assert 'power_on=false' in args
audio = 'audio=true' in args
control = 'control=true' in args
clipboard = os.environ.get('OTG_TEST_AUX_CLIPBOARD') == '1'
assert control == clipboard
deadline = time.monotonic() + 5
while not (root / 'port').exists():
    assert time.monotonic() < deadline
    time.sleep(.01)
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(('127.0.0.1', int((root / 'port').read_text())))
listener.listen(2)
first, _ = listener.accept()
(root / 'stream-open').touch()
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
if clipboard:
    header = read(14)
    assert header[0] == 9 and header[9] == 0
    assert read(struct.unpack('>I', header[10:14])[0]) == b'computer'
    channel.sendall(b'\x01' + header[1:9] + b'\x00\x00\x00\x00\x07android')
# No INJECT_KEYCODE bytes may be written to the shared control stream.
streams = list({first, channel})
while streams:
    readable, _, _ = select.select(streams, [], [], 5)
    assert readable
    for stream in readable:
        assert stream.recv(1) == b''
        (root / 'stream-closed').touch()
        streams.remove(stream)
if audio:
    assert (root / 'pause-requested').exists()
    if not os.environ.get('OTG_TEST_AUX_PAUSE'):
        assert (root / 'pause').exists()
(root / 'server-done').touch()
'''

helper, audio_enabled = sys.argv[1:]
modes = ["clipboard", "fail", "clipboard-retry", "push-timeout"]
if audio_enabled == "ON":
    modes += ["audio", "both", "audio-fail", "audio-retry", "both-retry"]
cases: list[tuple[str, str, str, str]] = [(mode, "", "FAKE_PHONE", "") for mode in modes]
for scenario in ("online", "connect", "connect-fail", "offline", "unauthorized", "timeout"):
    cases.append(("clipboard", scenario, "192.0.2.1:40195", ""))
cases.append(("clipboard", "connect", "[::1]:5555", ""))
if audio_enabled == "ON":
    cases += [(mode, "connect", "phone.local:5555", "") for mode in ("audio", "both")]
    cases += [(mode, "", "FAKE_PHONE", pause)
              for mode in ("audio", "both") for pause in ("fail", "timeout")]
for mode, scenario, target, pause in cases:
    with tempfile.TemporaryDirectory(prefix="otg-aux-") as directory:
        root = Path(directory)
        adb = root / "fake-adb"
        adb.write_text(f"#!{sys.executable}\n" + FAKE_ADB)
        adb.chmod(0o700)
        env = dict(os.environ, OTG_TEST_AUX_ROOT=directory)
        env["OTG_TEST_AUX_TARGET"] = target
        env["OTG_TEST_AUX_WIRELESS"] = scenario
        base_mode = mode.removesuffix('-retry')
        env["OTG_TEST_AUX_CLIPBOARD"] = "1" if base_mode in ("clipboard", "both", "push-timeout") else "0"
        env["OTG_TEST_AUX_PAUSE"] = pause
        wireless_failure = scenario in ("connect-fail", "offline", "unauthorized", "timeout")
        if wireless_failure:
            env["OTG_TEST_AUX_EXPECT_FAIL"] = "1"
        if mode in ("fail", "audio-fail"):
            env["OTG_TEST_AUX_FAIL"] = "1"
        if mode.endswith('-retry'):
            env["OTG_TEST_AUX_PUSH_RETRIES"] = "2"
        if mode == "push-timeout":
            env["OTG_TEST_AUX_PUSH_TIMEOUT"] = "1"
            env["OTG_TEST_AUX_EXPECT_FAIL"] = "1"
        start = time.monotonic()
        completed = subprocess.run([helper, str(adb), base_mode, target], env=env,
                                   timeout=15, capture_output=True, text=True)
        assert completed.returncode == 0, (mode, completed)
        elapsed = time.monotonic() - start
        if pause:
            assert 'media-session pause command failed or timed out' in completed.stderr, completed
        if pause == 'timeout':
            assert 1.8 <= elapsed < 5, elapsed
        if pause == 'fail':
            assert 'fake media session failure' in completed.stderr, completed
        assert 'fake media session' not in completed.stdout, completed
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
            assert calls[0] == 'get-state', calls
            assert calls[1].startswith('push '), calls
        assert sum(line.startswith('connect ') for line in calls) == bool(scenario and scenario != 'online'), calls
        pushes = sum(line.startswith("push ") for line in calls)
        if mode in ("fail", "audio-fail"):
            assert 1 < pushes <= 20 and elapsed < 7, (calls, elapsed)
        else:
            expected_pushes = 3 if mode.endswith('-retry') else 1
            assert pushes == (expected_pushes if not wireless_failure else 0), calls
        if mode == "push-timeout":
            assert 4.8 <= elapsed < 7, elapsed
        started = mode not in ("fail", "audio-fail", "push-timeout") and not wireless_failure
        paused = started and base_mode in ("audio", "both")
        assert sum(line.startswith("shell CLASSPATH=") for line in calls) == started, calls
        pause_command = 'shell cmd media_session dispatch pause'
        assert calls.count(pause_command) == paused, calls
        assert (root / "pause-requested").exists() == paused, calls
        assert (root / "pause").exists() == (paused and not pause), calls
        assert (root / "server-done").exists() == started, calls
        assert sum(line.startswith("forward tcp:") for line in calls) == started, calls
        assert sum(line.startswith("forward --remove ") for line in calls) == started, calls
        if paused:
            assert calls.index(pause_command) < next(i for i, line in enumerate(calls)
                                                    if line.startswith('forward --remove ')), calls
        assert not any(line.startswith(('disconnect', 'pair ', 'kill-server')) for line in calls), calls
        assert not (root / "port").exists(), calls
print("PASS: fake ADB lifecycle modes:", ", ".join(modes),
      "upload recovery/deadline, wireless readiness and audio pause/cleanup/failure/timeout")
