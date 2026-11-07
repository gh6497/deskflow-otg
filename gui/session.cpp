// SPDX-License-Identifier: Apache-2.0
#include "session.h"
#include "device_info.h"
#include "process_utils.h"
#include "i18n.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QRegularExpression>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTcpServer>

Session::Session(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    m_killBridge.setSingleShot(true);
    m_killBle.setSingleShot(true);
    connect(&m_killBle, &QTimer::timeout, this, [this] {
        if (!m_shutdown && !m_stoppingBleProvider) {
            m_failureId = "otg.ble.pause_failed";
            emit log(qtTrId("otg.ble.pause_failed"));
        }
        m_bleProvider.kill(); // Only this GUI's provider, never another CLI.
    });
    connect(&m_bleProvider, &QProcess::started, this, [this] {
        emit bleProviderChanged();
        if (m_shutdown || m_stoppingBleProvider) endBleProvider();
        else if (m_state == State::Stopping) m_bleProvider.write("pause\n");
    });
    connect(&m_bleProvider, &QProcess::readyReadStandardOutput, this, &Session::readBleEvents);
    connect(&m_bleProvider, &QProcess::readyReadStandardError, this, [this] {
        emit log(qtTrId("otg.log.bridge_prefix") + QString::fromUtf8(m_bleProvider.readAllStandardError()).trimmed());
    });
    connect(&m_bleProvider, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_bleActive = false;
            fail("otg.error.bridge_start", m_bleProvider.errorString());
            stopServer();
            emit bleProviderChanged();
        }
    });
    connect(&m_bleProvider, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_killBle.stop();
        readBleEvents();
        const bool wasActive = m_bleActive;
        m_bleActive = false;
        m_blePeer.clear();
        if (wasActive && m_state != State::Stopping) fail("otg.error.bridge_exited", QString::number(code));
        if (wasActive || m_state == State::Stopping) stopServer();
        else emit log(qtTrId("otg.ble.provider_ended"));
        emit bleProviderChanged();
    });
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail("otg.error.connection_timeout");
    });
    connect(&m_killBridge, &QTimer::timeout, this, [this] {
        // Never SIGKILL a privileged supervisor: it owns mode recovery.
        if (m_options.transport == "ble" && m_options.bleIsolate) {
            emit log(qtTrId("otg.ble.recovery_wait"));
            return;
        }
        emit log(qtTrId("otg.log.bridge_killed"));
        m_bridge.kill();
    });
    connect(&m_server, &DeskflowServer::ready, this, [this] {
        if (m_state == State::Starting) launchBridge();
    });
    connect(&m_server, &DeskflowServer::log, this, &Session::log);
    connect(&m_server, &DeskflowServer::failed, this, [this](const QString &id, const QString &detail) {
        fail(id.toLatin1().constData(), detail);
    });
    connect(&m_server, &DeskflowServer::stopped, this, &Session::finishStop);
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
    connect(&m_bridge, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            fail("otg.error.bridge_start", m_bridge.errorString());
            stopServer();
        }
    });
    connect(&m_bridge, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        m_killBridge.stop();
        readBridgeEvents();
        if (m_state != State::Stopping)
            fail("otg.error.bridge_exited", QString::number(code));
        stopServer();
    });
}

QString Session::statusMessage() const
{
    if (m_statusId.isEmpty())
        return {};
    const auto message = qtTrId(m_statusId.constData());
    return message.contains("%1") ? message.arg(m_statusDetail) : message;
}

Session::~Session()
{
    m_bridge.disconnect(this);
    m_bleProvider.disconnect(this);
    m_server.disconnect(this);
}

bool Session::windowsBle() const
{
#if defined(Q_OS_WIN) && defined(OTG_GUI_HAVE_BLE_WIN)
    return m_options.transport == "ble";
#else
    return false;
#endif
}

void Session::setState(State state, const char *id, const QString &detail)
{
    const bool transition = m_state != state;
    m_state = state;
    m_statusId = id;
    m_statusDetail = detail;
    if (transition) syncCompanionStatus();
    emit changed(state, statusMessage());
}

void Session::syncCompanionStatus()
{
    if (!m_options.adbEnabled) {
        m_companionStatus.update({}, {}, QStringLiteral("disconnected"), true);
        return;
    }
    // BLE has no implicit ADB identity. Never use its MAC as an ADB target.
    const auto target = !m_options.adbSerial.isEmpty() ? m_options.adbSerial :
        m_options.transport == "usb" ? m_options.serial : QString();
    const auto status = m_state == State::Connected ? "connected" :
        m_state == State::Starting ? "connecting" :
        m_state == State::Stopping ? "disconnecting" : "disconnected";
    m_companionStatus.update(m_options.adb, target, QString::fromLatin1(status),
                             !m_options.audio || m_audioReady);
}

static bool isWirelessAdbTarget(const QString &target)
{
    static const QRegularExpression pattern("^(?:\\[[^\\]]+\\]|[^:]+):[0-9]+$");
    return pattern.match(target).hasMatch();
}

static bool adbDeviceState(const QString &output)
{
    return output.trimmed() == QStringLiteral("device");
}

/* Keep the ADB boundary in one place.  HID itself never needs ADB, while
 * these three bridge features do: scrcpy audio/clipboard and PIN unlock. */
static bool adbRequested(const ConnectionOptions &options)
{
    return options.audio || options.clipboard || options.unlockAdb;
}

static bool adbRequired(const ConnectionOptions &options)
{
    return options.adbEnabled && adbRequested(options);
}

static QString adbTarget(const ConnectionOptions &options)
{
    if (!options.adbSerial.trimmed().isEmpty()) return options.adbSerial.trimmed();
#ifdef Q_OS_WIN
    return {};
#else
    return options.transport == "usb" ? options.serial.trimmed() : QString();
#endif
}

static QStringList auxiliarySignature(const ConnectionOptions &options)
{
    if (!adbRequired(options)) return {};
    /* Windows BLE keeps the provider alive between sessions.  Every value
     * consumed by the bridge's ADB path must therefore invalidate a retained
     * provider; otherwise a later session silently reuses the old target. */
    return {options.adb, options.adbSerial, options.scrcpyServer, options.scrcpyVersion,
            options.audio ? QStringLiteral("audio") : QStringLiteral("no-audio"),
            options.clipboard ? QStringLiteral("clipboard") : QStringLiteral("no-clipboard"),
            options.unlockAdb ? QStringLiteral("unlock") : QStringLiteral("no-unlock"),
            options.unlockPin};
}

void Session::start(const ConnectionOptions &options)
{
    if (m_state != State::Idle)
        return;
    m_shutdown = false;
    m_options = options;
    m_audioReady = false;
    m_adbServerStarted = false;
    m_adbTargetPrepared = false;
    m_clipboardStatusId = options.clipboard ? "otg.clipboard.waiting" : "otg.clipboard.off";
    emit clipboardChanged();
    m_failureId.clear();
    m_failureDetail.clear();
    m_events.clear();
    ++m_generation;
    setState(State::Starting, "otg.status.connecting");
    const bool ble = options.transport == "ble";
    if (adbRequested(options) && !options.adbEnabled) {
        fail("otg.error.adb_disabled");
        return;
    }
    if ((options.audio || options.clipboard) &&
        (options.scrcpyServer.isEmpty() || options.scrcpyVersion.isEmpty())) {
        fail("otg.error.scrcpy_runtime");
        return;
    }
    static const QRegularExpression peerPattern("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$");
    if ((!ble && options.serial.isEmpty()) ||
        (adbRequired(options) && (options.adb.isEmpty() || adbTarget(options).isEmpty())) ||
        (options.transport != "usb" && !ble) ||
        (ble && (!peerPattern.match(options.blePeer).hasMatch() || options.mouseMode != "absolute")) ||
        (options.middleButtonAction != "normal" && options.middleButtonAction != "home") ||
        (options.rightButtonAction != "normal" && options.rightButtonAction != "back") ||
        (!options.unlockPin.isEmpty() && (options.unlockPin.size() < 4 || options.unlockPin.size() > 32 ||
            !options.unlockPin.contains(QRegularExpression("^[0-9]+$")))) ||
        (options.tls && options.tlsFingerprint.isEmpty() && options.tlsFingerprintFile.isEmpty()) ||
        (options.tls && !options.tlsFingerprint.isEmpty() && !options.tlsFingerprintFile.isEmpty()) ||
        !validScreenName(options.phone) ||
        options.host.trimmed().isEmpty() || options.host.contains(QRegularExpression("[\\x00-\\x20\\x7f]")) ||
        options.host.toUtf8().size() > 1024 || options.phone.toUtf8().size() > 255 ||
        options.port < 1 || options.port > 65535 ||
        options.width < 1 || options.width > 32767 || options.height < 1 || options.height > 32767) {
        fail("otg.error.invalid_options");
        return;
    }
    if (options.unlockAdb && options.transport == "ble" && options.bleIsolate) {
        /* pkexec deliberately drops arbitrary environment variables. Keep
         * the PIN out of argv and hand it to the unprivileged child through
         * a private temporary file whose path survives the supervisor. */
        if (!m_unlockPinFile.fileName().isEmpty()) m_unlockPinFile.remove();
        m_unlockPinFile.setFileTemplate(QDir::tempPath() + "/deskflow-otg-unlock-XXXXXX");
        const auto pin = options.unlockPin.toUtf8();
        if (!m_unlockPinFile.open() || m_unlockPinFile.write(pin) != pin.size() ||
            !m_unlockPinFile.flush() ||
            !m_unlockPinFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            fail("otg.error.bridge_start", m_unlockPinFile.errorString());
            return;
        }
        m_unlockPinFile.close();
    }
#if defined(Q_OS_LINUX)
    static const QRegularExpression adapterPattern("^hci[0-9]{1,5}$");
    if (ble && (!adapterPattern.match(options.adapter).hasMatch() ||
                options.adapter.mid(3).toUInt() >= 65535)) {
        fail("otg.error.invalid_options");
        return;
    }
#elif defined(Q_OS_WIN) && defined(OTG_GUI_HAVE_BLE_WIN)
    if (ble && (options.bleIsolate || !options.adapter.isEmpty())) {
        fail("otg.error.invalid_options");
        return;
    }
#else
    if (ble) {
        fail("otg.ble.unsupported");
        return;
    }
#endif
    if (windowsBle() && hasBleProvider() &&
        (m_blePeer.compare(options.blePeer, Qt::CaseInsensitive) != 0 || m_bleProgram != options.bridge ||
         m_bleAuxiliary != auxiliarySignature(options))) {
        fail("otg.ble.provider_target");
        return;
    }
#ifdef Q_OS_LINUX
    if (ble) {
        prepareBle();
        return;
    }
#endif
    startConnection();
}

void Session::prepareBle(bool started, int attempts)
{
    const auto generation = m_generation;
    runCommand(this, m_options.bridge, {"--ble-status", "--adapter", m_options.adapter},
               [this, generation, started, attempts](bool ok, const QString &output) {
        if (generation != m_generation || m_state != State::Starting) return;
        const auto status = QJsonDocument::fromJson(output.toUtf8()).object();
        if (ok && status.value("version").toInt() == 2 && status.value("available").toBool()) {
            if (!status.value("idle").toBool()) {
                fail("otg.ble.busy");
                return;
            }
            startConnection();
            return;
        }
        if (started) {
            if (attempts >= 20) {
                fail("otg.ble.service_failed", output);
                return;
            }
            QTimer::singleShot(100, this, [this, generation, attempts] {
                if (generation == m_generation && m_state == State::Starting)
                    prepareBle(true, attempts + 1);
            });
            return;
        }
        // Detached, ordinary-user process. No PID-based stop/kill: another GUI
        // or a CLI may reuse it. Its existing private adapter lock wins races.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (dir.isEmpty() || !QDir().mkpath(dir)) {
            fail("otg.ble.service_failed", dir);
            return;
        }
        QProcess service;
        service.setProgram(m_options.bridge);
        service.setArguments({"--ble-service", "--adapter", m_options.adapter});
        service.setStandardInputFile(QProcess::nullDevice());
        service.setStandardOutputFile(QProcess::nullDevice());
        service.setStandardErrorFile(dir + "/ble-" + m_options.adapter + ".log", QIODevice::Append);
        if (!service.startDetached()) {
            fail("otg.ble.service_failed", service.errorString());
            return;
        }
        emit log(qtTrId("otg.ble.service_retained"));
        prepareBle(true);
    });
}

void Session::startConnection()
{
    const bool needsAdb = adbRequired(m_options);
    if (needsAdb && !m_adbServerStarted) {
        m_adbServerStarted = true;
        const auto generation = m_generation;
        emit log(QStringLiteral("[ADB] start-server: %1").arg(m_options.adb));
        runCommand(this, m_options.adb, {"start-server"},
                   [this, generation](bool ok, const QString &output) {
            if (generation != m_generation || m_state != State::Starting) return;
            emit log(QStringLiteral("[ADB] start-server %1: %2")
                         .arg(ok ? "ok" : "failed", output.trimmed()));
            if (!ok) {
                fail("otg.error.bridge_start", output.trimmed());
                return;
            }
            startConnection();
        });
        return;
    }
    const auto target = adbTarget(m_options);
    if (needsAdb && !m_adbTargetPrepared) {
        m_adbTargetPrepared = true;
        const auto generation = m_generation;
        emit log(QStringLiteral("[ADB] get-state: %1").arg(target));
        runCommand(this, m_options.adb, {"-s", target, "get-state"},
                   [this, generation, target](bool ok, const QString &output) {
            if (generation != m_generation || m_state != State::Starting) return;
            emit log(QStringLiteral("[ADB] get-state %1: %2 (%3)")
                         .arg(target, ok ? "ok" : "failed", output.trimmed()));
            if (ok && adbDeviceState(output)) {
                startConnection();
                return;
            }
            if (!isWirelessAdbTarget(target)) {
                fail("otg.error.bridge_start",
                     QStringLiteral("ADB target %1 is not ready").arg(target));
                return;
            }
            emit log(QStringLiteral("[ADB] wireless target not ready; connecting %1").arg(target));
            runCommand(this, m_options.adb, {"connect", target},
                       [this, generation, target](bool connected, const QString &connectOutput) {
                if (generation != m_generation || m_state != State::Starting) return;
                emit log(QStringLiteral("[ADB] connect %1: %2 (%3)")
                             .arg(target, connected ? "ok" : "failed", connectOutput.trimmed()));
                if (!connected) {
                    fail("otg.error.bridge_start", connectOutput.trimmed());
                    return;
                }
                emit log(QStringLiteral("[ADB] get-state after connect: %1").arg(target));
                runCommand(this, m_options.adb, {"-s", target, "get-state"},
                           [this, generation, target](bool ready, const QString &stateOutput) {
                    if (generation != m_generation || m_state != State::Starting) return;
                    emit log(QStringLiteral("[ADB] get-state after connect %1: %2 (%3)")
                                 .arg(target, ready ? "ok" : "failed", stateOutput.trimmed()));
                    if (!ready || !adbDeviceState(stateOutput)) {
                        fail("otg.error.bridge_start",
                             QStringLiteral("ADB target %1 is not ready").arg(target));
                        return;
                    }
                    startConnection();
                });
            });
        });
        return;
    }
    m_deadline.start(30000);
    if (!m_options.manageServer) {
        launchBridge();
        return;
    }
    const auto config = serverConfiguration(m_options.computer, m_options.phone,
                                            m_options.direction);
    if (config.isEmpty()) {
        fail("otg.error.screen_names");
        return;
    }
    m_server.start({m_options.deskflow, m_options.computer, config, m_options.port,
                    m_options.tls, m_options.tlsCertificate});
}

void Session::launchBridge()
{
    if (!windowsBle() && m_bridge.state() != QProcess::NotRunning)
        return;
    const bool ble = m_options.transport == "ble";
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("OTG_UNLOCK_PIN");
    environment.remove("OTG_UNLOCK_ADB_GATE");
    if (!m_options.unlockPin.isEmpty())
        environment.insert("OTG_UNLOCK_PIN", m_options.unlockPin);
    if (m_options.unlockAdb)
        environment.insert("OTG_UNLOCK_ADB_GATE", "1");
    m_bridge.setProcessEnvironment(environment);
    emit log(qtTrId(ble ? "otg.ble.waiting" : "otg.log.usb_initializing"));
    QStringList args = {"--gui", "--host", m_options.manageServer ? "127.0.0.1" : m_options.host,
                    "--port", QString::number(m_options.port), "--name", m_options.phone,
                     "--width", QString::number(m_options.width),
                     "--height", QString::number(m_options.height), "--mouse-mode", m_options.mouseMode,
                     "--middle-button-action", m_options.middleButtonAction,
                     "--right-button-action", m_options.rightButtonAction};
    if (adbRequired(m_options)) {
        if (m_options.audio) args << "--audio";
        if (m_options.clipboard) args << "--clipboard";
        args << "--adb" << m_options.adb;
        if (m_options.audio || m_options.clipboard) {
            args << "--scrcpy-server" << m_options.scrcpyServer
                 << "--scrcpy-version" << m_options.scrcpyVersion;
        }
        if (!m_options.adbSerial.isEmpty()) args << "--adb-serial" << m_options.adbSerial;
        if (m_options.autoRotate) args << "--auto-rotate";
        if (m_options.adbSerial.isEmpty()) args << "--adb-serial" << m_options.serial;
    }
    if (ble && m_options.bleIsolate && m_options.unlockAdb)
        args << "--unlock-pin-file" << m_unlockPinFile.fileName();
    if (m_options.tls) {
        args << "--tls";
        if (!m_options.tlsCertificate.isEmpty()) args << "--tls-cert" << m_options.tlsCertificate;
        if (!m_options.tlsFingerprint.isEmpty()) args << "--tls-fingerprint" << m_options.tlsFingerprint;
        else args << "--tls-fingerprint-file" << m_options.tlsFingerprintFile;
    }
    if (ble) {
        m_deadline.stop(); // Pairing and desktop authentication are user paced.
        args << "--transport" << "ble" << "--ble-peer" << m_options.blePeer;
        setState(State::Starting, "otg.ble.waiting");
#ifdef Q_OS_LINUX
        args << "--ble-session" << "--adapter" << m_options.adapter;
        if (m_options.bleIsolate) {
            args.prepend(m_options.bridge);
            args << "--ble-isolate";
            m_bridge.start("/usr/bin/pkexec", args);
            return;
        }
#endif
    } else {
        args << "--serial" << m_options.serial;
    }
    if (windowsBle()) {
        m_bleProvider.setProcessEnvironment(environment);
        m_bleActive = true;
        if (hasBleProvider()) {
            const auto resume = QString("resume\t%1\t%2\t%3\t%4\t%5\t%6\t%7\n")
                .arg(m_options.manageServer ? "127.0.0.1" : m_options.host)
                .arg(m_options.port).arg(m_options.phone).arg(m_options.width).arg(m_options.height)
                .arg(m_options.middleButtonAction).arg(m_options.rightButtonAction).toUtf8();
            m_bleProvider.write(resume);
            emit log(qtTrId("otg.ble.provider_reused"));
        } else {
            m_bleEvents.clear();
            m_blePeer = m_options.blePeer;
            m_bleProgram = m_options.bridge;
            m_bleAuxiliary = auxiliarySignature(m_options);
            args << "--gui-ble-control";
            m_bleProvider.start(m_options.bridge, args);
        }
        return;
    }
    m_bridge.start(m_options.bridge, args);
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
        handleBridgeEvent(QJsonDocument::fromJson(line).object());
    }
}

void Session::readBleEvents()
{
    m_bleEvents += m_bleProvider.readAllStandardOutput();
    if (m_bleEvents.size() > 65536) {
        m_bleEvents.clear();
        fail("otg.error.bridge_output");
        endBleProvider();
        return;
    }
    int end;
    while ((end = m_bleEvents.indexOf('\n')) >= 0) {
        const auto object = QJsonDocument::fromJson(m_bleEvents.left(end)).object();
        m_bleEvents.remove(0, end + 1);
        if (object.value("event") == "ble-idle") {
            m_bleActive = false;
            if (!m_shutdown && !m_stoppingBleProvider) {
                m_killBle.stop();
                emit log(qtTrId("otg.ble.provider_retained"));
                if (m_state == State::Stopping) stopServer();
            }
        } else if (m_bleActive && windowsBle()) {
            handleBridgeEvent(object);
        }
    }
}

void Session::handleBridgeEvent(const QJsonObject &object)
{
    const auto event = object.value("event").toString();
    if (m_options.audio && (event == "audio-ready" || event == "audio-unavailable") &&
        (m_state == State::Starting || m_state == State::Connected)) {
        const bool ready = event == "audio-ready";
        if (m_audioReady != ready) {
            m_audioReady = ready;
            syncCompanionStatus();
        }
        return;
    }
    if (event == "display-size" && m_state == State::Connected && m_options.autoRotate) {
        const int width = object.value("width").toInt(), height = object.value("height").toInt();
        const int vw = object.value("virtual-width").toInt(), vh = object.value("virtual-height").toInt();
        if (width > 0 && width <= 32767 && height > 0 && height <= 32767 &&
            vw > 0 && vw <= 32767 && vh > 0 && vh <= 32767) {
            m_options.width = vw; m_options.height = vh;
            emit displaySizeChanged(QSize(width, height));
        }
        return;
    }
    if (m_options.clipboard && (event == "clipboard-ready" || event == "clipboard-unavailable" ||
                               event == "clipboard-disabled")) {
        m_clipboardStatusId = event == "clipboard-ready" ? "otg.clipboard.ready" :
            event == "clipboard-disabled" ? "otg.clipboard.disabled" : "otg.clipboard.unavailable";
        emit clipboardChanged();
        return;
    }
    if (event == "connected" && m_state == State::Starting) {
        m_deadline.stop();
        setState(State::Connected, "otg.status.connected");
    } else if (event == "disconnected" && m_options.transport == "ble" &&
               m_state == State::Connected) {
        m_audioReady = false;
        setState(State::Starting, "otg.ble.reconnecting");
    } else if (event == "usb-ready") {
        const auto id = object.value("usb-id").toString();
        if (!id.isEmpty())
            emit usbAttached(id, m_options.serial);
        emit log(qtTrId("otg.log.usb_ready"));
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
    m_deadline.stop();
    setState(State::Stopping, "otg.status.disconnecting");
    if (m_shutdown) endBleProvider();
    if (windowsBle() && m_bleActive && hasBleProvider()) {
        if (!m_shutdown) {
            m_bleProvider.write("pause\n");
            m_killBle.start(8000);
        }
        return;
    }
    if (m_bridge.state() != QProcess::NotRunning) {
        m_bridge.write("stop\n");
        m_bridge.closeWriteChannel();
        m_killBridge.start(8000);
    } else {
        stopServer();
    }
}

void Session::endBleProvider()
{
    if (!hasBleProvider()) return;
    m_bleProvider.write("stop\n");
    m_bleProvider.closeWriteChannel();
    m_killBle.start(8000);
}

void Session::stopBleProvider()
{
    if (m_state != State::Idle || !hasBleProvider()) return;
    m_stoppingBleProvider = true;
    m_failureId.clear();
    m_failureDetail.clear();
    setState(State::Stopping, "otg.status.disconnecting");
    endBleProvider();
}

void Session::shutdown()
{
    m_shutdown = true;
    if (m_state == State::Idle && hasBleProvider()) {
        setState(State::Stopping, "otg.status.disconnecting");
        endBleProvider();
    } else if (m_state == State::Stopping) {
        endBleProvider();
    } else {
        stop();
    }
}

void Session::stopServer()
{
    if (m_server.state() != DeskflowServer::State::Idle) {
        m_server.stop();
    } else {
        finishStop();
    }
}

void Session::finishStop()
{
    if (m_bridge.state() != QProcess::NotRunning || m_server.state() != DeskflowServer::State::Idle ||
        m_bleActive || ((m_shutdown || m_stoppingBleProvider) && hasBleProvider()))
        return;
    m_killBridge.stop();
    m_stoppingBleProvider = false;
    if (!m_unlockPinFile.fileName().isEmpty()) m_unlockPinFile.remove();
    m_clipboardStatusId = m_options.clipboard ? "otg.clipboard.disconnected" : "otg.clipboard.off";
    emit clipboardChanged();
    setState(State::Idle, m_failureId.isEmpty() ? "otg.status.disconnected" : m_failureId.constData(), m_failureDetail);
}

QString Session::clipboardStatusMessage() const
{
    return qtTrId(m_clipboardStatusId.constData());
}
