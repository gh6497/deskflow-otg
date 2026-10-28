/* Testable cancellation semantics for the WinRT worker. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdexcept>
#include <string>

namespace otg_ble_win_detail {
enum class WaitReason { none, stopping, session_changed, timed_out };
class WaitAborted : public std::runtime_error {
public:
    WaitReason reason;
    WaitAborted(const char *name, WaitReason why)
        : std::runtime_error(std::string(name) + (why == WaitReason::session_changed ?
            ": session changed" : why == WaitReason::stopping ? ": stopping" : ": timed out")), reason(why) {}
};

template<class Pending, class Cancel, class Result, class Check, class Pause>
auto wait_operation(const char *name, Pending pending, Cancel cancel, Result result,
    Check check, Pause pause)
{
    while (pending()) {
        const auto reason = check(true);
        if (reason != WaitReason::none) {
            try { cancel(); } catch (...) {} // keep the original cancellation reason
            throw WaitAborted(name, reason);
        }
        pause();
    }
    // Completion racing a disconnect must not commit old-session results.
    const auto reason = check(false);
    if (reason != WaitReason::none) throw WaitAborted(name, reason);
    return result();
}
}
