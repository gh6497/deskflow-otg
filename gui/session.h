// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QProcess>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

struct ConnectionOptions {
    QString deskflow;
    QString bridge;
    QString serial;
    QString host = "127.0.0.1";
    QString computer = "computer";
    QString phone = "android";
    QString direction = "right";
    QString mouseMode = "absolute";
    int port = 24800;
    int width = 1080;
    int height = 1920;
    bool manageServer = true;
};

// Process ownership is explicit: only these two child processes are stopped.
class Session : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Starting, Connected, Stopping };
    explicit Session(QObject *parent = nullptr);
    State state() const { return m_state; }
    QString statusMessage() const;
    void start(const ConnectionOptions &options);
    void stop();

signals:
    void changed(Session::State state, const QString &message);
    void log(const QString &message);

private:
    void setState(State state, const char *id, const QString &detail = {});
    void fail(const char *id, const QString &detail = {});
    void launchServer(const QString &help);
    void launchBridge();
    void stopServer();
    void finishStop();
    void readBridgeEvents();
    ConnectionOptions m_options;
    State m_state = State::Idle;
    QProcess m_server, m_bridge;
    QTcpSocket m_probe;
    QTimer m_retry, m_deadline, m_killBridge, m_killServer;
    QTemporaryDir m_configDir;
    QByteArray m_events;
    QByteArray m_failureId, m_statusId;
    QString m_failureDetail, m_statusDetail;
    quint64 m_generation = 0;
};
