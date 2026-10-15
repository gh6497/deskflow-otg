// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QMap>
#include <QSet>
#include <QTimer>

class QProcess;

// Prefer bridge hotplug notifications, falling back to polling when unavailable.
// Descriptor reads stay in short-lived subprocesses and never block the GUI.
class UsbAutoConnect : public QObject {
    Q_OBJECT
public:
    explicit UsbAutoConnect(QObject *parent = nullptr);
    ~UsbAutoConnect() override;
    void setProgram(const QString &program) { m_program = program; }
    void setEnabled(bool enabled);
    void setReady(bool ready);
    void setKnownSerials(const QStringList &serials);
    void suppressSerial(const QString &serial);
    void suppressAttachment(const QString &id, const QString &serial);
    bool acceptSnapshot(const QByteArray &json);
    void poll();
    bool usesHotplug() const { return m_hotplug; }
signals:
    void connectRequested(const QString &serial);
    void scanFailed();
    void detectionModeChanged(bool hotplug);
private:
    void startWatcher();
    void stopWatcher();
    void fallbackToPolling();
    void readNotifications();
    void scheduleScan(int attempts = 1);
    QTimer m_timer;
    QTimer m_watchDeadline, m_rescan;
    QProcess *m_watcher = nullptr;
    QByteArray m_notifications;
    int m_rescansLeft = 0;
    QString m_program;
    QSet<QString> m_known, m_attempted, m_suppressed;
    QMap<QString, QString> m_present;
    bool m_enabled = false;
    bool m_ready = false;
    bool m_scanning = false;
    bool m_scanPending = false;
    bool m_hotplug = false;
    bool m_reportedError = false;
    quint64 m_generation = 0;
};
