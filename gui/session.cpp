// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include "device_info.h"
#include "process_utils.h"
#include "i18n.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpServer>

Session::Session(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    m_killBridge.setSingleShot(true);
    m_killServer.setSingleShot(true);
    m_retry.setInterval(250);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail("otg.error.connection_timeout");
    });
    connect(&m_killBridge, &QTimer::timeout, this, [this] {
        emit log(qtTrId("otg.log.bridge_killed"));
        m_bridge.kill();
    });
    connect(&m_killServer, &QTimer::timeout, &m_server, &QProcess::kill);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (m_state == State::Starting && m_probe.state() == QAbstractSocket::UnconnectedState)
            m_probe.connectToHost("127.0.0.1", quint16(m_options.port));
    });
    connect(&m_probe, &QTcpSocket::connected, this, [this] {
        m_probe.abort();
        m_retry.stop();
        if (m_state == State::Starting && m_server.state() == QProcess::Running)
            launchBridge();
    });
    connect(&m_server, &QProcess::started, this, [this] {
        if (m_state == State::Stopping)
            stopServer();
        else
            m_retry.start();
    });
    connect(&m_bridge, &QProcess::started, this, [this] {
        if (m_state == State::Stopping) {
            m_bridge.write("stop\n");
            m_bridge.closeWriteChannel();
        }
    });
    connect(&m_bridge, &QProcess::readyReadStandardOutput, this, &Session::readBridgeEvents);
    connect(&m_bridge, &QProcess::readyReadStandardError, this, [this] {
        emit log(qtTrId("otg.log.bridge_prefix") + QString::fromUtf8(m_bridge.readAllStandardError()).trimmed());
    });
    m_server.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_server, &QProcess::readyReadStandardOutput, this, [this] {
        emit log("[Deskflow] " + QString::fromUtf8(m_server.readAllStandardOutput()).trimmed());
    });
    connect(&m_bridge, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            fail("otg.error.bridge_start", m_bridge.errorString());
            stopServer();
        }
    });
    connect(&m_server, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            fail("otg.error.deskflow_start", m_server.errorString());
            finishStop();
        }
    });
    connect(&m_bridge, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_killBridge.stop();
        readBridgeEvents();
        if (m_state != State::Stopping)
            fail("otg.error.bridge_exited", QString::number(code));
        stopServer();
    });
    connect(&m_server, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_killServer.stop();
        if (m_state != State::Stopping)
            fail("otg.error.deskflow_exited", QString::number(code));
        finishStop();
    });
}

QString Session::statusMessage() const
{
    if (m_statusId.isEmpty())
        return {};
    const auto message = qtTrId(m_statusId.constData());
    return message.contains("%1") ? message.arg(m_statusDetail) : message;
}

void Session::setState(State state, const char *id, const QString &detail)
{
    m_state = state;
    m_statusId = id;
    m_statusDetail = detail;
    emit changed(state, statusMessage());
}

void Session::start(const ConnectionOptions &options)
{
    if (m_state != State::Idle)
        return;
    m_options = options;
    m_failureId.clear();
    m_failureDetail.clear();
    m_events.clear();
    const auto generation = ++m_generation;
    setState(State::Starting, "otg.status.connecting");
    if (options.serial.isEmpty() || !validScreenName(options.phone) ||
        options.host.trimmed().isEmpty() || options.port < 1 || options.port > 65535 ||
        options.width < 1 || options.width > 32767 || options.height < 1 || options.height > 32767) {
        fail("otg.error.invalid_options");
        return;
    }
    m_deadline.start(30000);
    if (!options.manageServer) {
        launchBridge();
        return;
    }
    // Bind before spawning; don't accidentally use an already-running server.
    QTcpServer portCheck;
    if (!portCheck.listen(QHostAddress::LocalHost, quint16(options.port))) {
        fail("otg.error.port_in_use");
        return;
    }
    runCommand(this, options.deskflow, {"--help"},
               [this, generation](bool ok, const QString &help) {
        if (generation != m_generation || m_state != State::Starting)
            return;
        if (!ok) {
            fail("otg.error.deskflow_probe", help);
            return;
        }
        emit log(qtTrId("otg.log.deskflow_probe") + help);
        launchServer(help);
    });
}

void Session::launchServer(const QString &help)
{
    const auto config = serverConfiguration(m_options.computer, m_options.phone,
                                            m_options.direction);
    if (config.isEmpty() || !m_configDir.isValid()) {
        fail("otg.error.screen_names");
        return;
    }
    const QString layout = m_configDir.filePath("screens.conf");
    QFile file(layout);
    const auto bytes = config.toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
        fail("otg.error.write_layout", file.errorString());
        return;
    }
    file.close();
    const QString settingsPath = m_configDir.filePath("deskflow.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.clear();
    settings.setValue("core/computerName", m_options.computer);
    settings.setValue("core/screenName", m_options.computer); // older settings schema
    settings.setValue("core/coreMode", 2);
    settings.setValue("core/processMode", 1);
    settings.setValue("core/interface", "127.0.0.1");
    settings.setValue("core/port", m_options.port);
    settings.setValue("security/tlsEnabled", false);
    settings.setValue("server/externalConfig", true);
    settings.setValue("server/externalConfigFile", layout);
    settings.setValue("server/enableClipboard", false);
    settings.setValue("log/level", 3); // INFO
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        fail("otg.error.write_settings");
        return;
    }
    QStringList args;
    if (help.contains("--settings") && help.contains("coremode")) {
        args = {"server", "--settings", settingsPath};
    } else if (help.contains("--config") && help.contains("--name") &&
               help.contains("--address") && help.contains("--no-daemon")) {
        args = {"--no-daemon", "--name", m_options.computer, "--config", layout,
                "--address", "127.0.0.1:" + QString::number(m_options.port)};
        if (help.contains("--disable-crypto"))
            args << "--disable-crypto";
        else if (help.contains("--settings"))
            args << "--settings" << settingsPath;
        else if (help.contains("--enable-crypto")) {
            // Legacy Deskflow (e.g. 1.17) defaults to plaintext and opts in
            // with --enable-crypto. Its ArgsBase::m_enableCrypto is false.
        }
        else {
            fail("otg.error.disable_tls");
            return;
        }
    } else {
        fail("otg.error.deskflow_interface");
        return;
    }
    emit log(qtTrId("otg.log.screen_config") + config);
    m_server.start(m_options.deskflow, args);
}

void Session::launchBridge()
{
    if (m_bridge.state() != QProcess::NotRunning)
        return;
    emit log(qtTrId("otg.log.usb_initializing"));
    m_bridge.start(m_options.bridge,
                  {"--gui", "--host", m_options.manageServer ? "127.0.0.1" : m_options.host,
                   "--port", QString::number(m_options.port), "--name", m_options.phone,
                   "--serial", m_options.serial, "--width", QString::number(m_options.width),
                   "--height", QString::number(m_options.height), "--mouse-mode", m_options.mouseMode});
}

void Session::readBridgeEvents()
{
    m_events += m_bridge.readAllStandardOutput();
    if (m_events.size() > 65536) {
        fail("otg.error.bridge_output");
        m_events.clear();
        return;
    }
    int end;
    while ((end = m_events.indexOf('\n')) >= 0) {
        const auto line = m_events.left(end);
        m_events.remove(0, end + 1);
        const auto event = QJsonDocument::fromJson(line).object().value("event").toString();
        if (event == "connected" && m_state == State::Starting) {
            m_deadline.stop();
            setState(State::Connected, "otg.status.connected");
        } else if (event == "usb-ready") {
            emit log(qtTrId("otg.log.usb_ready"));
        }
    }
}

void Session::fail(const char *id, const QString &detail)
{
    if (m_failureId.isEmpty()) {
        m_failureId = id;
        m_failureDetail = detail;
    }
    const auto text = qtTrId(id);
    emit log(text.contains("%1") ? text.arg(detail) : text);
    stop();
}

void Session::stop()
{
    if (m_state == State::Idle || m_state == State::Stopping)
        return;
    ++m_generation;
    m_retry.stop();
    m_deadline.stop();
    m_probe.abort();
    setState(State::Stopping, "otg.status.disconnecting");
    if (m_bridge.state() != QProcess::NotRunning) {
        m_bridge.write("stop\n");
        m_bridge.closeWriteChannel();
        m_killBridge.start(8000);
    } else {
        stopServer();
    }
}

void Session::stopServer()
{
    if (m_server.state() != QProcess::NotRunning) {
        m_server.terminate();
        if (!m_killServer.isActive())
            m_killServer.start(3000);
    } else {
        finishStop();
    }
}

void Session::finishStop()
{
    if (m_bridge.state() != QProcess::NotRunning || m_server.state() != QProcess::NotRunning)
        return;
    m_killBridge.stop();
    m_killServer.stop();
    setState(State::Idle, m_failureId.isEmpty() ? "otg.status.disconnected" : m_failureId.constData(), m_failureDetail);
}
