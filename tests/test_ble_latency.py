"""Offline HCI fixtures; tests never open an adapter or require root."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    'ble_latency', Path(__file__).resolve().parents[1] / 'scripts/ble_latency.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Wire:
    def __init__(self):
        self.sent = []

    def send(self, data):
        self.sent.append(data)
        return len(data)


def event(code, data):
    return bytes([4, code, len(data)]) + data


def connected(handle=2048, role=1, enhanced=True):
    # Anonymous peer; interval=36*1.25ms, latency=0, timeout=5s.
    body = struct.pack('<BHBB6s', 0, handle, role, 0, bytes(6))
    if enhanced:
        body += bytes(12)
    body += struct.pack('<HHHB', 36, 0, 500, 0)
    return event(0x3e, bytes([10 if enhanced else 1]) + body)


class LatencyTest(unittest.TestCase):
    def setUp(self):
        self.channel, self.hci, self.logs = Wire(), Wire(), []
        self.monitor = module.LatencyMonitor(0, self.channel, self.hci, self.logs.append)

    def prepare(self):
        self.monitor.event(connected())
        self.monitor.event(event(8, struct.pack('<BHB', 0, 2048, 1)))
        self.monitor.message(b'ready:hci0\n')

    def test_requires_ready_encrypted_single_peripheral(self):
        self.monitor.event(connected())
        self.monitor.negotiate(0)
        self.assertEqual(self.hci.sent, [])
        self.monitor.message(b'ready:hci0\n')
        self.monitor.negotiate(0)
        self.assertEqual(self.hci.sent, [])
        self.prepare()
        self.monitor.event(connected(2049))
        self.monitor.message(b'ready:hci0\n')
        self.monitor.negotiate(0)
        self.assertEqual(self.hci.sent, [])

    def test_request_and_actual_confirmation(self):
        self.prepare()
        self.monitor.negotiate(10)
        self.assertEqual(self.channel.sent[-1], b'interval_us=45000\n')
        packet = self.hci.sent[0]
        self.assertEqual(struct.unpack('<BHB7H', packet),
                         (1, 0x2013, 14, 2048, 6, 12, 0, 500, 0, 0))
        # Successful Command Status means accepted, NOT completed.
        self.monitor.event(event(15, struct.pack('<BBH', 0, 1, 0x2013)))
        self.assertEqual(self.channel.sent[-1], b'interval_us=45000\n')
        self.monitor.event(event(0x3e, b'\x03' + struct.pack('<BHHHH', 0, 2048, 6, 0, 500)))
        self.assertEqual(self.channel.sent[-1], b'interval_us=7500\n')
        self.monitor.negotiate(11)
        self.assertEqual(len(self.hci.sent), 1)

    def test_rejected_or_timeout_keeps_observed_interval(self):
        self.prepare()
        self.monitor.negotiate(0)
        self.monitor.event(event(15, struct.pack('<BBH', 12, 1, 0x2013)))
        self.assertIsNone(self.monitor.pending)
        self.assertEqual(len(self.channel.sent), 1)
        self.assertIn('rejected', self.logs[-1])
        self.monitor.negotiate(6)
        self.assertEqual(len(self.hci.sent), 1)
        self.setUp()
        self.prepare()
        self.monitor.negotiate(0)
        self.monitor.negotiate(6)
        self.assertIn('not confirmed', self.logs[-1])
        self.assertEqual(len(self.channel.sent), 1)

    def test_reconnect_discards_old_handle(self):
        self.prepare()
        self.monitor.negotiate(0)
        self.monitor.event(event(5, struct.pack('<BHB', 0, 2048, 19)))
        self.assertFalse(self.monitor.ready)
        self.monitor.event(connected(2049, enhanced=False))
        self.monitor.event(event(8, struct.pack('<BHB', 0, 2049, 1)))
        self.monitor.negotiate(1)
        self.assertEqual(len(self.hci.sent), 1)
        self.monitor.message(b'ready:hci0\n')
        self.monitor.negotiate(2)
        self.assertEqual(struct.unpack_from('<H', self.hci.sent[-1], 4)[0], 2049)

    def test_wrong_adapter_central_and_truncated_packets(self):
        for n in range(len(connected())):
            self.monitor.event(connected()[:n])
        self.assertEqual(self.monitor.links, {})
        self.prepare()
        self.monitor.ready = False
        self.monitor.message(b'ready:hci1\n')
        self.monitor.negotiate(0)
        self.assertEqual(self.hci.sent, [])
        self.monitor.links[2048]['role'] = 0
        self.monitor.message(b'ready:hci0\n')
        self.monitor.negotiate(0)
        self.assertEqual(self.hci.sent, [])


if __name__ == '__main__':
    unittest.main()
