// SPDX-License-Identifier: Apache-2.0
#include "companion_status_sync.h"
#include "process_utils.h"
#include <QRegularExpression>

CompanionStatusSync::CompanionStatusSync(QObject *parent) : QObject(parent)
{
    m_heartbeat.setInterval(5000);
    connect(&m_heartbeat, &QTimer::timeout, this, &CompanionStatusSync::send);
}

void CompanionStatusSync::update(const QString &adb, const QString &target, const QString &state)
{
    ++m_revision;
    m_adb = adb;
    m_target = target;
    m_state = state;
    m_heartbeat.stop();
    m_pending = false;
    if (adb.trimmed().isEmpty() || target.isEmpty() || target.startsWith('-') ||
        target.contains(QRegularExpression("[\\x00-\\x20\\x7f]")) ||
        (state != "connecting" && state != "connected" &&
         state != "disconnecting" && state != "disconnected")) {
        m_state.clear();
        return;
    }
    if (state != "disconnected") m_heartbeat.start();
    send();
}

void CompanionStatusSync::send()
{
    if (m_state.isEmpty()) return;
    // One pipeline in flight; transitions coalesce to the newest state/target.
    // An old get-state callback must never publish a stale connected event.
    if (m_busy) { m_pending = true; return; }
    m_busy = true;
    m_pending = false;
    const auto revision = m_revision;
    const auto adb = m_adb, target = m_target, state = m_state;
    auto finish = [this] {
        m_busy = false;
        if (m_pending) send();
    };
    runCommand(this, adb, {"-s", target, "get-state"},
               [this, revision, adb, target, state, finish](bool ok, const QString &output) {
        if (revision != m_revision || !ok || output.trimmed() != "device") {
            finish();
            return;
        }
        // Explicit receiver and fixed tokens only. No app launch, permission
        // grants, shell interpolation of screen names, or fallback device.
        runCommand(this, adb, {"-s", target, "shell", "am", "broadcast", "--user", "current",
                   "-n", "com.github.gh6497.deskflowotg/.ConnectionStatusReceiver",
                   "-a", "com.github.gh6497.deskflowotg.CONNECTION_STATUS",
                   "--ei", "version", "1", "--es", "state", state},
                   [finish](bool, const QString &) { finish(); }, 2000);
    }, 2000);
}
