// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QProcess>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include "deskflow_server.h"
class QJsonObject;

struct ConnectionOptions {
    QString deskflow;
    QString bridge;
    QString serial;
    QString host = "127.0.0.1";
    QString computer = "computer";
    QString phone = "android";
    QString direction = "right";
    QString mouseMode = "absolute";
    QString transport = "usb";
#ifdef Q_OS_WIN
    QString adapter; // Windows BLE only supports the default adapter.
#else
    QString adapter = "hci0";
#endif
    QString blePeer;
    bool bleIsolate = false;
    int port = 24800;
    int width = 1080;
    int height = 1920;
    bool manageServer = true;
};

// Only owned bridge/server processes are stopped. A resident BLE service is
// deliberately retained across sessions and GUI exits to preserve BlueZ CCCs.
// Windows BLE is parked between GUI sessions; only explicit service stop or
// GUI shutdown terminates its provider. It is not a detached system service.
class Session : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Starting, Connected, Stopping };
    explicit Session(QObject *parent = nullptr);
    ~Session() override;
    State state() const { return m_state; }
    QString statusMessage() const;
    bool hasFailure() const { return !m_failureId.isEmpty(); }
    void start(const ConnectionOptions &options);
    void stop();
    void shutdown();
    void stopBleProvider();
    bool hasBleProvider() const { return m_bleProvider.state() != QProcess::NotRunning; }

signals:
    void changed(Session::State state, const QString &message);
    void log(const QString &message);
    void usbAttached(const QString &id, const QString &serial);
    void bleProviderChanged();

private:
    void setState(State state, const char *id, const QString &detail = {});
    void fail(const char *id, const QString &detail = {});
    void launchBridge();
    void prepareBle(bool started = false, int attempts = 0);
    void startConnection();
    void stopServer();
    void finishStop();
    void readBridgeEvents();
    void readBleEvents();
    void handleBridgeEvent(const QJsonObject &object);
    bool windowsBle() const;
    void endBleProvider();
    ConnectionOptions m_options;
    State m_state = State::Idle;
    DeskflowServer m_server;
    QProcess m_bridge, m_bleProvider;
    QTimer m_deadline, m_killBridge, m_killBle;
    QByteArray m_events;
    QByteArray m_bleEvents;
    QString m_bleProgram, m_blePeer;
    bool m_bleActive = false, m_shutdown = false, m_stoppingBleProvider = false;
    QByteArray m_failureId, m_statusId;
    QString m_failureDetail, m_statusDetail;
    quint64 m_generation = 0;
};
