// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include "device_info.h"
#include "process_utils.h"

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
        fail("连接超时：请检查 Deskflow 权限、TLS 设置、USB 驱动及日志。");
    });
    connect(&m_killBridge, &QTimer::timeout, this, [this] {
        emit log("桥接退出超时，强制终止。请检查 USB 设备状态。");
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
        emit log("[桥接] " + QString::fromUtf8(m_bridge.readAllStandardError()).trimmed());
    });
    m_server.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_server, &QProcess::readyReadStandardOutput, this, [this] {
        emit log("[Deskflow] " + QString::fromUtf8(m_server.readAllStandardOutput()).trimmed());
    });
    connect(&m_bridge, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            fail("桥接启动失败：" + m_bridge.errorString());
            stopServer();
        }
    });
    connect(&m_server, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            fail("Deskflow 启动失败：" + m_server.errorString());
            finishStop();
        }
    });
    connect(&m_bridge, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_killBridge.stop();
        readBridgeEvents();
        if (m_state != State::Stopping)
            fail(QString("桥接已退出（%1），请查看日志。").arg(code));
        stopServer();
    });
    connect(&m_server, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_killServer.stop();
        if (m_state != State::Stopping)
            fail(QString("Deskflow 已退出（%1），请查看权限、端口及配置日志。").arg(code));
        finishStop();
    });
}

void Session::setState(State state, const QString &message)
{
    m_state = state;
    emit changed(state, message);
}

void Session::start(const ConnectionOptions &options)
{
    if (m_state != State::Idle)
        return;
    m_options = options;
    m_failure.clear();
    m_events.clear();
    const auto generation = ++m_generation;
    setState(State::Starting, "正在连接…");
    if (options.serial.isEmpty() || !validScreenName(options.phone) ||
        options.host.trimmed().isEmpty() || options.port < 1 || options.port > 65535 ||
        options.width < 1 || options.width > 32767 || options.height < 1 || options.height > 32767) {
        fail("设备序列号、屏幕名称、主机或尺寸无效。");
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
        fail("端口已被占用：停止原 Deskflow 服务、修改端口，或选择连接已有服务。");
        return;
    }
    runCommand(this, options.deskflow, {"--help"},
               [this, generation](bool ok, const QString &help) {
        if (generation != m_generation || m_state != State::Starting)
            return;
        if (!ok) {
            fail("无法检测 Deskflow 启动参数：" + help);
            return;
        }
        emit log("[Deskflow 参数检测]\n" + help);
        launchServer(help);
    });
}

void Session::launchServer(const QString &help)
{
    const auto config = serverConfiguration(m_options.computer, m_options.phone,
                                            m_options.direction);
    if (config.isEmpty() || !m_configDir.isValid()) {
        fail("电脑/手机名称必须不同，且仅包含字母、数字、下划线、点或连字符。");
        return;
    }
    const QString layout = m_configDir.filePath("screens.conf");
    QFile file(layout);
    const auto bytes = config.toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
        fail("无法写入 Deskflow 屏幕配置：" + file.errorString());
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
        fail("无法写入 Deskflow 启动设置。");
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
            fail("此 Deskflow 启动接口无法显式关闭 TLS，请使用连接已有服务模式。");
            return;
        }
    } else {
        fail("未识别的 Deskflow 启动接口。请选择 deskflow-core / deskflow-server，或连接已有服务。");
        return;
    }
    emit log("使用独立屏幕配置：\n" + config);
    m_server.start(m_options.deskflow, args);
}

void Session::launchBridge()
{
    if (m_bridge.state() != QProcess::NotRunning)
        return;
    emit log("正在初始化 USB HID 并等待 Deskflow 接受屏幕…");
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
        fail("桥接状态输出无效。");
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
            setState(State::Connected, "已连接 — 将鼠标移到手机一侧即可控制");
        } else if (event == "usb-ready") {
            emit log("USB HID 已就绪。");
        }
    }
}

void Session::fail(const QString &message)
{
    if (m_failure.isEmpty())
        m_failure = message;
    emit log(message);
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
    setState(State::Stopping, "正在释放键鼠并断开…");
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
    setState(State::Idle, m_failure.isEmpty() ? "已断开" : m_failure);
}
