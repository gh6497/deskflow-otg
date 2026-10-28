/* Fake operations only; no Bluetooth or sleeps. SPDX-License-Identifier: Apache-2.0 */
#include "ble_win_wait.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #test); std::exit(1); } } while (0)
using namespace otg_ble_win_detail;

int main()
{
    for (auto expected : {WaitReason::none, WaitReason::stopping, WaitReason::session_changed, WaitReason::timed_out}) {
        int ticks = 0, cancels = 0, results = 0;
        bool pending = true;
        try {
            auto value = wait_operation("notify pointer", [&] { return pending; }, [&] { ++cancels; },
                [&] { ++results; return 42; },
                [&](bool) { return ticks >= 2 ? expected : WaitReason::none; },
                [&] { if (++ticks == 3) pending = false; });
            CHECK(value == 42 && expected == WaitReason::none && results == 1 && cancels == 0);
        } catch (const WaitAborted& e) {
            CHECK(e.reason == expected && results == 0 && cancels == 1);
            CHECK(std::strstr(e.what(), "notify pointer") != nullptr);
            // Cancel is only a REQUEST. Fake operation deliberately stays
            // pending: the caller must retain it and not start another notify.
            CHECK(pending);
        }
    }
    int results = 0;
    try {
        wait_operation("resolve subscriber", [] { return false; }, [] {}, [&] { ++results; return 1; },
            [](bool) { return WaitReason::session_changed; }, [] {});
        CHECK(false);
    } catch (const WaitAborted& e) { CHECK(e.reason == WaitReason::session_changed && results == 0); }
    CHECK(wait_operation("already complete", [] { return false; }, [] {}, [] { return 7; },
        [](bool pending) { return pending ? WaitReason::timed_out : WaitReason::none; }, [] {}) == 7);
    std::puts("PASS: named operation completion, stop, timeout, session cancellation and completion/disconnect race");
    return 0;
}
