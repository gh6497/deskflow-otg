// SPDX-License-Identifier: Apache-2.0
#include "process_utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
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
    auto finish = [process, timer, output, completed, done](bool ok, const QString &error) {
        if (*completed)
            return;
        *completed = true;
        timer->stop();
        output->append(process->readAll());
        const QString text = QString::fromUtf8(*output);
        process->deleteLater();
        done(ok, error.isEmpty() ? text : error + "\n" + text);
    };
    QObject::connect(process, &QProcess::readyRead, process, [process, output] {
        output->append(process->readAll());
        if (output->size() > 4 * 1024 * 1024) {
            process->setProperty("commandError", QStringLiteral("命令输出过大"));
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
        process->setProperty("commandError", QStringLiteral("命令超时"));
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
