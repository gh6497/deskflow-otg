// SPDX-License-Identifier: Apache-2.0
#include "companion_app.h"
#include "process_utils.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>

namespace {
const QString package = "com.github.gh6497.deskflowotg";
const QString listener = package + "/" + package + ".DeskflowNotificationListenerService";
}

QString CompanionApp::bundledApk()
{
    const auto path = QCoreApplication::applicationDirPath() + "/deskflow-otg-companion.apk";
    return QFileInfo(path).isFile() && QFileInfo(path).isReadable() ? path : QString();
}

void CompanionApp::finish(const char *statusId, const QString &detail)
{
    m_busy = false;
    emit completed(QString::fromLatin1(statusId), detail);
}

void CompanionApp::start(Action action, const QString &adb, const QString &serial, const QString &apk)
{
    if (m_busy) return;
    // Always pin the selected device, including wireless ADB. Never let adb pick
    // an arbitrary phone, pair/connect it, or grant notification access silently.
    if (adb.trimmed().isEmpty() || serial.isEmpty() || serial.startsWith('-') ||
        serial.contains(QRegularExpression("[\\x00-\\x20\\x7f]"))) {
        finish("otg.companion.target_required");
        return;
    }
    if (action == Action::Install &&
        (!QFileInfo(apk).isFile() || !QFileInfo(apk).isReadable() ||
         QFileInfo(apk).suffix().compare("apk", Qt::CaseInsensitive) != 0)) {
        finish("otg.companion.apk_required");
        return;
    }
    m_busy = true;
    m_adb = adb;
    m_target = {"-s", serial};
    const auto absoluteApk = QFileInfo(apk).absoluteFilePath();
    runCommand(this, m_adb, m_target + QStringList{"get-state"},
               [this, action, absoluteApk](bool ok, const QString &output) {
        if (!ok || !output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts).contains("device")) {
            finish("otg.companion.adb_failed", output);
            return;
        }
        execute(action, absoluteApk);
    });
}

void CompanionApp::execute(Action action, const QString &apk)
{
    if (action == Action::Install) {
        runCommand(this, m_adb, m_target + QStringList{"install", "-r", "--user", "current", apk},
                   [this](bool ok, const QString &output) {
            const bool success = ok && output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts).contains("Success");
            // Never uninstall or downgrade to work around signature/version errors.
            finish(success ? "otg.companion.installed" : "otg.companion.install_failed", output);
        }, 60000);
        return;
    }
    runCommand(this, m_adb, m_target + QStringList{"shell", "pm", "path", "--user", "current", package},
               [this, action](bool ok, const QString &output) {
        if (!ok) { finish("otg.companion.command_failed", output); return; }
        if (!output.contains(QRegularExpression("(?:^|[\\r\\n])package:[^\\r\\n]+"))) {
            finish("otg.companion.not_installed");
            return;
        }
        if (action == Action::Open) {
            runCommand(this, m_adb, m_target + QStringList{"shell", "am", "start", "--user", "current", "-W", "-n", package + "/.MainActivity"},
                       [this](bool opened, const QString &text) {
                const bool success = opened && text.contains(QRegularExpression("(?:^|[\\r\\n])Status: ok(?:[\\r\\n]|$)"));
                finish(success ? "otg.companion.opened" : "otg.companion.command_failed", text);
            });
            return;
        }
        runCommand(this, m_adb, m_target + QStringList{"shell", "settings", "--user", "current", "get", "secure", "enabled_notification_listeners"},
                   [this](bool read, const QString &text) {
            if (!read || text.contains(QRegularExpression("(?:^|[\\r\\n])(?:Error|Exception|SecurityException)"))) {
                finish("otg.companion.command_failed", text);
                return;
            }
            const auto enabled = text.trimmed().split(':');
            const bool granted = enabled.contains(listener) ||
                enabled.contains(package + "/.DeskflowNotificationListenerService");
            // Permission is not proof that the OS has bound the service, nor that
            // this ROM emits accessory-detached for the AOA HID-only path.
            finish(granted ? "otg.companion.access_enabled" : "otg.companion.access_required");
        });
    });
}
