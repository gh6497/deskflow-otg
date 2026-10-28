/* Windows adapter's bounded HID queue; caller serializes access. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace otg_ble_win_detail {
// Fallback for Windows 10 and adapters which do not expose negotiated
// connection parameters. Windows 11 uses the live interval when available.
inline constexpr unsigned notification_interval_ms = 30;
struct Report {
    uint16_t id = 0;
    std::array<uint8_t, 8> bytes{};
    bool movement = false;
    uint64_t epoch = 0;
};

class Queue {
    std::deque<Report> reports;
    std::array<uint8_t, 8> last_mouse{};
    bool mouse_seen = false;
public:
    static constexpr size_t capacity = 64;
    bool push(Report report)
    {
        if (report.id != 1 && report.id != 2) return false;
        report.movement = false;
        if (report.id == 2) {
            report.movement = mouse_seen && report.bytes[0] == last_mouse[0] &&
                report.bytes[1] == last_mouse[1] && !report.bytes[6] && !report.bytes[7] &&
                !last_mouse[6] && !last_mouse[7];
        }
        if (report.movement && !reports.empty() && reports.back().movement &&
            reports.back().epoch == report.epoch &&
            reports.back().bytes[0] == report.bytes[0] &&
            reports.back().bytes[1] == report.bytes[1]) {
            reports.back() = report;
        } else {
            if (reports.size() == capacity) return false;
            reports.push_back(report);
        }
        if (report.id == 2) { last_mouse = report.bytes; mouse_seen = true; }
        return true;
    }
    bool empty() const { return reports.empty(); }
    size_t size() const { return reports.size(); }
    const Report& front() const { return reports.front(); }
    Report pop() { auto report = reports.front(); reports.pop_front(); return report; }
    void clear() { reports.clear(); mouse_seen = false; last_mouse = {}; }
};

/* Keep any already-issued notification outside this queue in flight. The
 * worker must finish it before these releases; no old queued press survives. */
inline void queue_releases(Queue& queue, std::array<uint8_t, 8> mouse,
                           uint64_t epoch, bool can_send)
{
    queue.clear();
    if (!can_send) return;
    mouse[1] = mouse[6] = mouse[7] = 0;
    mouse[0] = (mouse[0] & 2) ? 2 : 0;
    queue.push({1, {}, false, epoch});
    queue.push({2, mouse, false, epoch});
    mouse[0] = 0;
    queue.push({2, mouse, false, epoch});
}

/* Bridge descriptors are report-ID-free USB application collections. HOGP
 * combines them and puts the ID in Report Reference, not the notification. */
inline bool append_descriptor(std::vector<uint8_t>& map,
    const uint8_t *desc, size_t size, uint8_t id)
{
    if (!desc || size < 7 || desc[0] != 0x05 || desc[2] != 0x09 ||
        desc[4] != 0xa1 || desc[5] != 1 || desc[size - 1] != 0xc0) return false;
    map.insert(map.end(), desc, desc + 6);
    map.insert(map.end(), {0x85, id});
    map.insert(map.end(), desc + 6, desc + size);
    return true;
}
}
