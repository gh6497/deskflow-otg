// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "session.h"
#include <QMap>

class ConnectionManager : public QObject {
    Q_OBJECT
public:
    explicit ConnectionManager(QObject *parent = nullptr);
    ~ConnectionManager() override;
    Session *addDevice(const QString &id);
    bool removeDevice(const QString &id);
    Session *session(const QString &id) const;
    Session::State state(const QString &id) const;
    QString status(const QString &id) const;
    bool hasFailure(const QString &id) const;
    bool busy() const;
    bool settled() const;
    bool hasBleProvider() const;
    DeskflowServer::State serverState() const { return m_server.state(); }
    bool start(const QString &id, const ConnectionOptions &options, const QString &layout);
    void stop(const QString &id);
    void stopAll();
    void shutdown();
signals:
    void changed(const QString &id);
    void log(const QString &id, const QString &message);
    void usbAttached(const QString &id, const QString &serial);
private:
    struct Entry {
        Session *session = nullptr;
        ConnectionOptions options;
        bool pending = false;
        QString errorId, errorDetail;
    };
    void launchPending();
    void update(const QString &id);
    bool reject(const QString &id, const char *message);
    QMap<QString, Entry> m_devices;
    DeskflowServer m_server;
    ConnectionOptions m_shared;
    QString m_layout;
    bool m_shutdown = false;
};
