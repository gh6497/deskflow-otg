// SPDX-License-Identifier: Apache-2.0
#include "usb_auto_connect.h"
#include "process_utils.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

UsbAutoConnect::UsbAutoConnect(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, &UsbAutoConnect::poll);
    m_watchDeadline.setSingleShot(true);
    connect(&m_watchDeadline, &QTimer::timeout, this, &UsbAutoConnect::fallbackToPolling);
    m_rescan.setSingleShot(true);
    connect(&m_rescan, &QTimer::timeout, this, [this] {
        poll();
        if (--m_rescansLeft > 0)
            m_rescan.start(750);
    });
}

UsbAutoConnect::~UsbAutoConnect()
{
    // Disconnect callbacks before member destruction, including when a slow
    // descriptor scan is still running during application exit.
    for (auto *process : findChildren<QProcess *>()) {
        process->disconnect();
        process->kill();
        process->waitForFinished(1000);
        delete process;
    }
}

void UsbAutoConnect::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    ++m_generation;
    if (enabled) {
        QTimer::singleShot(0, this, &UsbAutoConnect::startWatcher);
    } else {
        m_timer.stop();
        m_rescan.stop();
        m_rescansLeft = 0;
        m_scanPending = false;
        stopWatcher();
    }
}

void UsbAutoConnect::setReady(bool ready)
{
    const bool wasReady = m_ready;
    m_ready = ready;
    if (ready && !wasReady)
        scheduleScan(); // A phone may have arrived while the session was busy.
}

void UsbAutoConnect::setKnownSerials(const QStringList &serials)
{
    m_known = QSet<QString>(serials.begin(), serials.end());
    scheduleScan();
}

void UsbAutoConnect::startWatcher()
{
    if (!m_enabled || m_watcher || m_timer.isActive())
        return;
    m_notifications.clear();
    m_watcher = new QProcess(this);
    connect(m_watcher, &QProcess::readyReadStandardOutput, this, &UsbAutoConnect::readNotifications);
    connect(m_watcher, &QProcess::readyReadStandardError, this, [this] {
        if (m_watcher)
            m_watcher->readAllStandardError(); // Drain diagnostics; mode change is logged by GUI.
    });
    connect(m_watcher, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fallbackToPolling();
    });
    connect(m_watcher, &QProcess::finished, this, [this] {
        fallbackToPolling(); // Includes unsupported capability, registration failure and crashes.
    });
    m_watchDeadline.start(3000);
    m_watcher->start(m_program, {"--usb-watch"});
}

void UsbAutoConnect::stopWatcher()
{
    m_watchDeadline.stop();
    m_hotplug = false;
    if (!m_watcher)
        return;
    auto *process = m_watcher;
    m_watcher = nullptr;
    disconnect(process, nullptr, this, nullptr);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
    } else {
        connect(process, &QProcess::finished, process, &QObject::deleteLater);
        process->kill(); // Watcher owns no HID state and is safe to stop immediately.
    }
}

void UsbAutoConnect::fallbackToPolling()
{
    stopWatcher();
    if (!m_enabled || m_timer.isActive())
        return;
    m_timer.start();
    emit detectionModeChanged(false);
    scheduleScan();
}

void UsbAutoConnect::scheduleScan(int attempts)
{
    if (!m_enabled)
        return;
    m_rescansLeft = qMax(m_rescansLeft, attempts);
    m_rescan.start(150); // Coalesce bursts and let device permissions settle.
}

void UsbAutoConnect::readNotifications()
{
    if (!m_watcher)
        return;
    m_notifications += m_watcher->readAllStandardOutput();
    if (m_notifications.size() > 65536) {
        fallbackToPolling();
        return;
    }
    int end;
    while ((end = m_notifications.indexOf('\n')) >= 0) {
        const auto line = m_notifications.left(end);
        m_notifications.remove(0, end + 1);
        const auto object = QJsonDocument::fromJson(line).object();
        const auto event = object.value("event").toString();
        if (event == "watch-ready" && object.value("version").toInt() == 1) {
            if (!m_hotplug) {
                m_watchDeadline.stop();
                m_hotplug = true;
                emit detectionModeChanged(true);
                scheduleScan(3); // Includes devices already attached at startup.
            }
        } else if ((event == "usb-added" || event == "usb-removed") &&
                   !object.value("id").toString().isEmpty()) {
            ++m_generation; // Discard any snapshot captured before this event.
            if (event == "usb-removed") {
                const auto id = object.value("id").toString();
                m_attempted.remove(id);
                m_present.remove(id);
            }
            // A bounded retry burst handles udev/driver readiness. No periodic
            // enumeration remains active once this event has settled.
            scheduleScan(3);
        } else {
            fallbackToPolling();
            return;
        }
    }
}

void UsbAutoConnect::suppressSerial(const QString &serial)
{
    ++m_generation; // Discard snapshots captured before the manual attempt.
    // Also suppress before the first snapshot (e.g. a manual connection made
    // immediately after launch). Consume this suppression on the next scan.
    m_suppressed.insert(serial);
    for (auto it = m_present.cbegin(); it != m_present.cend(); ++it) {
        if (it.value() == serial)
            m_attempted.insert(it.key());
    }
}

void UsbAutoConnect::suppressAttachment(const QString &id, const QString &serial)
{
    ++m_generation;
    m_attempted.insert(id);
    m_present.insert(id, serial);
    m_suppressed.remove(serial);
}

bool UsbAutoConnect::acceptSnapshot(const QByteArray &json)
{
    const auto doc = QJsonDocument::fromJson(json);
    if (!doc.isObject() || doc.object().value("version").toInt() != 1 ||
        !doc.object().value("devices").isArray())
        return false;
    QMap<QString, QString> present;
    QMap<QString, int> counts;
    for (const auto &value : doc.object().value("devices").toArray()) {
        const auto device = value.toObject();
        const auto id = device.value("id").toString();
        if (id.isEmpty() || !device.value("serial").isString() || present.contains(id))
            return false; // Never re-arm devices on partial/malformed output.
        const auto serial = device.value("serial").toString();
        present.insert(id, serial);
        if (!serial.isEmpty())
            ++counts[serial];
    }
    const auto ids = present.keys();
    m_attempted.intersect(QSet<QString>(ids.begin(), ids.end()));
    for (auto it = present.cbegin(); it != present.cend(); ++it) {
        if (m_suppressed.contains(it.value()))
            m_attempted.insert(it.key());
    }
    m_suppressed.clear();
    m_present = present;
    if (!m_enabled || !m_ready)
        return true;
    // Do not pick an arbitrary phone if multiple remembered phones are plugged
    // in, including devices sharing a non-unique serial number.
    QString candidate, attachment;
    int eligible = 0;
    for (auto it = present.cbegin(); it != present.cend(); ++it) {
        if (!m_known.contains(it.value()))
            continue;
        ++eligible;
        if (counts.value(it.value()) == 1 && !m_attempted.contains(it.key())) {
            candidate = it.value();
            attachment = it.key();
        }
    }
    if (m_multiple) {
        QStringList candidates;
        for (auto it = present.cbegin(); it != present.cend(); ++it) {
            if (m_known.contains(it.value()) && counts.value(it.value()) == 1 && !m_attempted.contains(it.key())) {
                m_attempted.insert(it.key());
                candidates.append(it.value());
            }
        }
        for (const auto &serial : candidates) {
            if (!m_enabled || !m_ready) break;
            emit connectRequested(serial);
        }
    } else if (eligible == 1 && !candidate.isEmpty()) {
        m_attempted.insert(attachment); // One attempt per USB attachment, even on failure.
        emit connectRequested(candidate);
    }
    return true;
}

void UsbAutoConnect::poll()
{
    if (!m_enabled || m_known.isEmpty())
        return;
    if (m_scanning) {
        m_scanPending = true;
        return;
    }
    m_scanning = true;
    const auto generation = m_generation;
    runCommand(this, m_program, {"--usb-scan"}, [this, generation](bool ok, const QString &text) {
        m_scanning = false;
        if (!m_enabled)
            return;
        if (generation != m_generation) {
            m_scanPending = true;
        } else if (ok && acceptSnapshot(text.toUtf8())) {
            m_reportedError = false;
        } else if (!m_reportedError) {
            m_reportedError = true;
            emit scanFailed();
        }
        if (m_scanPending) {
            m_scanPending = false;
            QTimer::singleShot(0, this, &UsbAutoConnect::poll);
        }
    }, 8000);
}
