// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QStringList>

// Optional APK setup only; never participates in the HID/session lifecycle.
class CompanionApp : public QObject {
    Q_OBJECT
public:
    enum class Action { Install, Open, Check };
    explicit CompanionApp(QObject *parent = nullptr) : QObject(parent) {}
    bool busy() const { return m_busy; }
    void start(Action action, const QString &adb, const QString &serial, const QString &apk = {});
    static QString bundledApk();
signals:
    void completed(const QString &statusId, const QString &detail);
private:
    void execute(Action action, const QString &apk);
    void finish(const char *statusId, const QString &detail = {});
    bool m_busy = false;
    QString m_adb;
    QStringList m_target;
};
