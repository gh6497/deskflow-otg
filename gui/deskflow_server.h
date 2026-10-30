// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QProcess>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

struct ServerOptions {
    QString program;
    QString computer = "computer";
    QString configuration;
    int port = 24800;
};

// Owns only the server it creates; never attaches to or stops an external server.
class DeskflowServer : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Starting, Running, Stopping };
    explicit DeskflowServer(QObject *parent = nullptr);
    ~DeskflowServer() override;
    State state() const { return m_state; }
    void start(const ServerOptions &options);
    void stop();
signals:
    void ready();
    void stopped();
    void changed();
    void failed(const QString &id, const QString &detail);
    void log(const QString &message);
private:
    void launch(const QString &help);
    void fail(const char *id, const QString &detail = {});
    void finish();
    void setState(State state);
    ServerOptions m_options;
    State m_state = State::Idle;
    QProcess m_process;
    QTcpSocket m_probe;
    QTemporaryDir m_directory;
    QTimer m_retry, m_deadline, m_kill;
    quint64 m_generation = 0;
};
