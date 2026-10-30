// SPDX-License-Identifier: Apache-2.0
#include "deskflow_server.h"
#include "device_info.h"
#include "process_utils.h"
#include <QFile>
#include <QSettings>
#include <QTcpServer>

DeskflowServer::DeskflowServer(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    m_kill.setSingleShot(true);
    m_retry.setInterval(250);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail("otg.error.connection_timeout"); });
    connect(&m_kill, &QTimer::timeout, &m_process, &QProcess::kill);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (m_state == State::Starting && m_probe.state() == QAbstractSocket::UnconnectedState)
            m_probe.connectToHost("127.0.0.1", quint16(m_options.port));
    });
    connect(&m_probe, &QTcpSocket::connected, this, [this] {
        m_probe.abort();
        if (m_state != State::Starting || m_process.state() != QProcess::Running) return;
        m_retry.stop();
        m_deadline.stop();
        setState(State::Running);
        emit ready();
    });
    connect(&m_process, &QProcess::started, this, [this] {
        if (m_state == State::Stopping) stop();
        else m_retry.start();
    });
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        emit log("[Deskflow] " + QString::fromUtf8(m_process.readAllStandardOutput()).trimmed());
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            fail("otg.error.deskflow_start", m_process.errorString());
            finish();
        }
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (m_state != State::Stopping) fail("otg.error.deskflow_exited", QString::number(code));
        finish();
    });
}

void DeskflowServer::setState(State state)
{
    m_state = state;
    emit changed();
}

DeskflowServer::~DeskflowServer()
{
    m_process.disconnect(this);
    m_probe.disconnect(this);
}

void DeskflowServer::start(const ServerOptions &options)
{
    if (m_state != State::Idle) return;
    m_options = options;
    const auto generation = ++m_generation;
    setState(State::Starting);
    if (!validScreenName(options.computer) ||
        options.configuration.isEmpty() || options.port < 1 || options.port > 65535) {
        fail("otg.error.invalid_options");
        return;
    }
    QTcpServer check;
    if (!check.listen(QHostAddress::LocalHost, quint16(options.port))) {
        fail("otg.error.port_in_use");
        return;
    }
    check.close();
    if (options.program.isEmpty()) { fail("otg.error.invalid_options"); return; }
    m_deadline.start(30000);
    runCommand(this, options.program, {"--help"}, [this, generation](bool ok, const QString &help) {
        if (generation != m_generation || m_state != State::Starting) return;
        if (!ok) { fail("otg.error.deskflow_probe", help); return; }
        emit log(qtTrId("otg.log.deskflow_probe") + help);
        launch(help);
    });
}

void DeskflowServer::launch(const QString &help)
{
    if (!m_directory.isValid()) { fail("otg.error.write_layout"); return; }
    const auto layout = m_directory.filePath("screens.conf");
    QFile file(layout);
    const auto bytes = m_options.configuration.toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
        fail("otg.error.write_layout", file.errorString()); return;
    }
    file.close();
    const auto path = m_directory.filePath("deskflow.ini");
    QSettings settings(path, QSettings::IniFormat);
    settings.clear();
    settings.setValue("core/computerName", m_options.computer);
    settings.setValue("core/screenName", m_options.computer);
    settings.setValue("core/coreMode", 2);
    settings.setValue("core/processMode", 1);
    settings.setValue("core/interface", "127.0.0.1");
    settings.setValue("core/port", m_options.port);
    settings.setValue("security/tlsEnabled", false);
    settings.setValue("server/externalConfig", true);
    settings.setValue("server/externalConfigFile", layout);
    settings.setValue("server/enableClipboard", false);
    settings.setValue("log/level", 3);
    settings.sync();
    if (settings.status() != QSettings::NoError) { fail("otg.error.write_settings"); return; }
    QStringList args;
    if (help.contains("--settings") && help.contains("coremode")) {
        args = {"server", "--settings", path};
    } else if (help.contains("--config") && help.contains("--name") &&
               help.contains("--address") && help.contains("--no-daemon")) {
        args = {"--no-daemon", "--name", m_options.computer, "--config", layout,
                "--address", "127.0.0.1:" + QString::number(m_options.port)};
        if (help.contains("--disable-crypto")) args << "--disable-crypto";
        else if (help.contains("--settings")) args << "--settings" << path;
        else if (!help.contains("--enable-crypto")) { fail("otg.error.disable_tls"); return; }
    } else { fail("otg.error.deskflow_interface"); return; }
    emit log(qtTrId("otg.log.screen_config") + m_options.configuration);
    m_process.start(m_options.program, args);
}

void DeskflowServer::fail(const char *id, const QString &detail)
{
    // Mark stopping before callbacks: they may synchronously request stop().
    if (m_state == State::Idle || m_state == State::Stopping) return;
    setState(State::Stopping);
    emit failed(QString::fromLatin1(id), detail);
    stop();
}

void DeskflowServer::stop()
{
    if (m_state == State::Idle) return;
    ++m_generation;
    m_retry.stop();
    m_deadline.stop();
    m_probe.abort();
    if (m_state != State::Stopping) setState(State::Stopping);
    if (m_process.state() == QProcess::NotRunning) finish();
    else {
        m_process.terminate();
        if (!m_kill.isActive()) m_kill.start(3000);
    }
}

void DeskflowServer::finish()
{
    if (m_state == State::Idle) return;
    m_kill.stop();
    m_retry.stop();
    m_deadline.stop();
    m_probe.abort();
    setState(State::Idle);
    emit stopped();
}
