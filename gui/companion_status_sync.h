// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

// Optional, best-effort status channel. Never connects/pairs ADB or changes HID.
class CompanionStatusSync : public QObject {
    Q_OBJECT
public:
    explicit CompanionStatusSync(QObject *parent = nullptr);
    void update(const QString &adb, const QString &target, const QString &state,
                bool playbackReady = true);

private:
    void send();
    QTimer m_heartbeat;
    QString m_adb, m_target, m_state;
    QString m_connectionId;
    QElapsedTimer m_playDeadline;
    bool m_playbackReady = false, m_playPending = false, m_playIssued = false;
    quint64 m_revision = 0;
    bool m_busy = false, m_pending = false;
};
