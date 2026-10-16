"""Privileged, session-only BLE interval negotiation. SPDX-License-Identifier: Apache-2.0

Used by the isolation supervisor, never by the unprivileged bridge directly.
Only a single observed, encrypted peripheral link is eligible, and only after
the child reports both HID input subscriptions ready. No addresses or keys are
logged. A command status is NOT treated as confirmation of new parameters.
"""
import select
import socket
import struct
import time


UPDATE_OPCODE = 0x2013


class LatencyMonitor:
    def __init__(self, index, channel, hci, log=print):
        self.index, self.channel, self.hci, self.log = index, channel, hci, log
        self.links = {}
        self.ready = False
        self.attempted = set()
        self.pending = None
        self.deadline = 0

    @classmethod
    def open(cls, index, channel):
        hci = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW,
                            socket.BTPROTO_HCI)
        try:
            hci.bind((index,))
            # HCI event packets only: disconnect, encryption, command status,
            # encryption-v2, and LE meta. Do not receive ACL/key material.
            events = (5, 8, 15, 0x59, 0x3e)
            mask = sum(1 << (event & 63) for event in events)
            hci.setsockopt(0, 2, struct.pack('=IIIH', 1 << 4,
                                             mask & 0xffffffff, mask >> 32, 0))
            hci.setblocking(False)
            return cls(index, channel, hci,
                       lambda text: print(text, flush=True))
        except BaseException:
            hci.close()
            raise

    def publish(self, interval, latency):
        # A nonzero peripheral latency permits skipped events. Do not claim
        # the interval alone bounds input delivery in that case.
        try:
            self.channel.send(f'interval_us={interval * 1250}\n'.encode())
        except (BlockingIOError, BrokenPipeError, ConnectionRefusedError):
            pass
        self.log(f'INFO: BLE actual interval={interval * 1.25:g}ms '
                 f'latency={latency} (controller event)')

    def event(self, packet):
        if len(packet) < 3 or packet[0] != 4:
            return
        event, length = packet[1:3]
        data = packet[3:]
        if len(data) != length:
            return
        if event == 5 and len(data) == 4:
            if data[0]:
                return
            handle = struct.unpack_from('<H', data, 1)[0]
            if handle not in self.links:
                return
            self.links.pop(handle, None)
            self.attempted.discard(handle)
            self.ready = False
            if self.pending == handle:
                self.pending = None
        elif event in (8, 0x59) and len(data) >= 4:
            status, handle, encrypted = struct.unpack_from('<BHB', data)
            if handle in self.links:
                self.links[handle]['encrypted'] = status == 0 and encrypted != 0
        elif event == 15 and len(data) == 4:
            status, _, opcode = struct.unpack('<BBH', data)
            if opcode == UPDATE_OPCODE and status and self.pending is not None:
                self.log(f'WARN: BLE interval request rejected: HCI status=0x{status:02x}')
                self.pending = None
        elif event == 0x3e and data:
            sub = data[0]
            # LE Connection Complete / Enhanced Connection Complete v1.
            if sub in (1, 0x0a) and len(data) == (19 if sub == 1 else 31):
                if data[1]:
                    return
                handle = struct.unpack_from('<H', data, 2)[0]
                offset = 12 if sub == 1 else 24
                interval, latency = struct.unpack_from('<HH', data, offset)
                self.links[handle] = dict(role=data[4], encrypted=False,
                                          interval=interval, latency=latency)
                self.ready = False
                self.attempted.discard(handle)
            elif sub == 3 and len(data) == 10:
                status, handle, interval, latency, _ = struct.unpack('<BHHHH', data[1:])
                if handle not in self.links:
                    return
                if status:
                    self.log(f'WARN: BLE interval update failed: status=0x{status:02x}')
                else:
                    self.links[handle].update(interval=interval, latency=latency)
                    if self.ready and len(self.links) == 1:
                        self.publish(interval, latency)
                if self.pending == handle:
                    self.pending = None

    def message(self, message):
        if message == f'ready:hci{self.index}\n'.encode():
            self.ready = True
        elif message == f'not-ready:hci{self.index}\n'.encode():
            self.ready = False

    def negotiate(self, now):
        if self.pending is not None and now >= self.deadline:
            self.log('WARN: BLE interval update not confirmed within 5s; keeping observed parameters')
            self.pending = None
        # Ambiguous links are never selected. Never tune central/unencrypted
        # connections, or accept handles/commands supplied by the child.
        if not self.ready or len(self.links) != 1 or self.pending is not None:
            return
        handle, link = next(iter(self.links.items()))
        if link['role'] != 1 or not link['encrypted'] or handle in self.attempted:
            return
        self.attempted.add(handle)
        self.publish(link['interval'], link['latency'])
        if 6 <= link['interval'] <= 12 and link['latency'] == 0:
            return
        params = struct.pack('<7H', handle, 6, 12, 0, 500, 0, 0)
        try:
            self.hci.send(struct.pack('<BHB', 1, UPDATE_OPCODE, len(params)) + params)
        except OSError as exc:
            self.log(f'WARN: BLE interval request failed: {exc}')
            return
        self.pending = handle
        self.deadline = now + 5
        self.log('INFO: BLE requesting interval 7.5–15ms; waiting for controller confirmation')

    def wait(self, child):
        while child.poll() is None:
            readable, _, _ = select.select([self.hci, self.channel], [], [], 0.05)
            # Drain controller events before acting on child readiness.
            if self.hci in readable:
                while True:
                    try:
                        self.event(self.hci.recv(260))
                    except BlockingIOError:
                        break
            if self.channel in readable:
                while True:
                    try:
                        self.message(self.channel.recv(128))
                    except BlockingIOError:
                        break
            self.negotiate(time.monotonic())
        return child.returncode

    def close(self):
        self.hci.close()
