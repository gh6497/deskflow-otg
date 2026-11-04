// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QTimer>

// Optional, best-effort status channel. Never connects/pairs ADB or changes HID.
class CompanionStatusSync : public QObject {
    Q_OBJECT
public:
    explicit CompanionStatusSync(QObject *parent = nullptr);
    void update(const QString &adb, const QString &target, const QString &state);

private:
    void send();
    QTimer m_heartbeat;
    QString m_adb, m_target, m_state;
    quint64 m_revision = 0;
    bool m_busy = false, m_pending = false;
};
