// SPDX-License-Identifier: Apache-2.0
#include "process_utils.h"
#include "i18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <memory>

void runCommand(QObject *owner, const QString &program, const QStringList &args,
                CommandResult done, int timeoutMs)
{
    auto *process = new QProcess(owner);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto output = std::make_shared<QByteArray>();
    auto completed = std::make_shared<bool>(false);
    const QPointer<QObject> guard(owner);
    auto finish = [process, timer, output, completed, done, guard](bool ok, const QString &error) {
        if (*completed)
            return;
        *completed = true;
        timer->stop();
        output->append(process->readAll());
        const QString text = QString::fromUtf8(*output);
        process->deleteLater();
        if (guard) done(ok, error.isEmpty() ? text : error + "\n" + text);
    };
    QObject::connect(process, &QProcess::readyRead, process, [process, output] {
        output->append(process->readAll());
        if (output->size() > 4 * 1024 * 1024) {
            process->setProperty("commandError", qtTrId("otg.error.command_output"));
            process->kill();
        }
    });
    QObject::connect(process, &QProcess::errorOccurred, process,
                     [finish, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false, process->errorString());
    });
    QObject::connect(process, &QProcess::finished, process,
                     [finish, process](int code, QProcess::ExitStatus status) {
        const auto error = process->property("commandError").toString();
        finish(code == 0 && status == QProcess::NormalExit && error.isEmpty(), error);
    });
    QObject::connect(timer, &QTimer::timeout, process, [process] {
        process->setProperty("commandError", qtTrId("otg.error.command_timeout"));
        process->kill();
    });
    timer->start(timeoutMs);
    process->start(program, args);
}

static QString executable(const QString &path)
{
#ifdef Q_OS_WIN
    if (QFileInfo(path + ".exe").isExecutable())
        return path + ".exe";
#endif
    return QFileInfo(path).isExecutable() && QFileInfo(path).isFile() ? path : QString();
}

QString bridgePath()
{
    return executable(QCoreApplication::applicationDirPath() + "/deskflow-otg");
}

QString findAdb()
{
    const auto bundled = executable(QCoreApplication::applicationDirPath() + "/platform-tools/adb");
    return bundled.isEmpty() ? QStandardPaths::findExecutable("adb") : bundled;
}

QString findScrcpyServer()
{
    QStringList paths{
        QCoreApplication::applicationDirPath() + "/scrcpy-server",
        QCoreApplication::applicationDirPath() + "/share/scrcpy/scrcpy-server",
        QCoreApplication::applicationDirPath() + "/../share/deskflow-otg/scrcpy-server",
        QDir::homePath() + "/.local/share/scrcpy/scrcpy-server",
        "/usr/local/share/scrcpy/scrcpy-server",
        "/usr/share/scrcpy/scrcpy-server",
        "/usr/local/share/deskflow-otg/scrcpy-server",
        "/usr/share/deskflow-otg/scrcpy-server",
    };
    for (const auto &path : paths) {
        if (QFileInfo(path).isFile() && QFileInfo(path).isReadable())
            return path;
    }
    return {};
}

QString findScrcpyVersion()
{
    const QStringList versionFiles{
        QCoreApplication::applicationDirPath() + "/../share/deskflow-otg/SCRCPY_VERSION.txt",
        QCoreApplication::applicationDirPath() + "/share/deskflow-otg/SCRCPY_VERSION.txt",
        "/usr/local/share/deskflow-otg/SCRCPY_VERSION.txt",
        "/usr/share/deskflow-otg/SCRCPY_VERSION.txt",
    };
    for (const auto &path : versionFiles) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const auto version = QString::fromUtf8(file.readAll()).trimmed();
            if (QRegularExpression("^[0-9]+\\.[0-9]+\\.[0-9]+$").match(version).hasMatch())
                return version;
        }
    }
    QString program = QStandardPaths::findExecutable("scrcpy");
    if (program.isEmpty() && QFileInfo(QDir::homePath() + "/.local/bin/scrcpy").isExecutable())
        program = QDir::homePath() + "/.local/bin/scrcpy";
    if (program.isEmpty()) return {};
    QProcess process;
    process.start(program, {"--version"});
    if (!process.waitForFinished(3000)) return {};
    const auto text = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
    const auto match = QRegularExpression("scrcpy\\s+([0-9]+\\.[0-9]+\\.[0-9]+)").match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString findDeskflow()
{
    QStringList dirs{QCoreApplication::applicationDirPath(),
                     "/Applications/Deskflow.app/Contents/MacOS",
                     QDir::homePath() + "/Applications/Deskflow.app/Contents/MacOS",
                     "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"};
#ifdef Q_OS_WIN
    dirs.prepend(qEnvironmentVariable("ProgramFiles") + "/Deskflow");
    dirs.prepend(qEnvironmentVariable("ProgramFiles(x86)") + "/Deskflow");
#endif
    for (const QString &name : {QString("deskflow-core"), QString("deskflow-server")}) {
        const auto path = QStandardPaths::findExecutable(name, dirs);
        if (!path.isEmpty())
            return path;
        const auto systemPath = QStandardPaths::findExecutable(name);
        if (!systemPath.isEmpty())
            return systemPath;
    }
    return {};
}
