// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <QCryptographicHash>
#include <QTemporaryFile>
#include <functional>
#include <memory>

struct SharedFile {
    QString id, name;
    qint64 size = 0;
    QByteArray sha256;
};

// One explicit, pinned ADB target, one command in flight, bounded binary streams.
// Independent of the C HID bridge and the Deskflow text clipboard.
class CompanionTransfer : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 MaxFile = 1024LL * 1024 * 1024;
    CompanionTransfer(const QString &adb, const QString &target, QObject *parent = nullptr);
    ~CompanionTransfer() override;
    bool busy() const { return m_busy; }
    const QList<SharedFile> &files() const { return m_files; }
    void refresh();
    void receive(const QStringList &ids, const QString &directory);
    void send(const QStringList &paths);
    void cancel();
    static QString safeName(QString name);
    static bool parseManifest(const QByteArray &data, QList<SharedFile> &files);
signals:
    void listed();
    void progress(const QString &name, qint64 bytes, qint64 total);
    void completed(bool ok, const QString &statusId, const QString &detail);
private:
    using Done = std::function<void(bool)>;
    enum class Stream { Capture, Download, Upload };
    bool begin();
    void online(Done done);
    void command(const QStringList &args, Stream stream, Done done, int timeout = 30000);
    void drain();
    bool writeUpload(const QByteArray &bytes, bool final = false);
    void pump();
    void failCommand();
    void finish(bool ok, const char *statusId, const QString &detail = {});
    void nextReceive();
    void nextSend();
    void receipt();
    QString uri(const QString &path) const;
    QStringList read(const QString &path) const;
    QString m_adb, m_target, m_user;
    QProcess *m_process = nullptr;
    QTimer m_idle, m_absolute, m_receiptTimer;
    QByteArray m_capture, m_errors, m_uploadRemainder;
    Stream m_stream = Stream::Capture;
    bool m_busy = false, m_cancelled = false, m_failed = false, m_trailer = false;
    QList<SharedFile> m_files, m_receiving;
    QStringList m_paths;
    QString m_directory, m_name, m_uploadId;
    qint64 m_size = 0, m_bytes = 0;
    int m_index = 0, m_receiptAttempts = 0;
    QFile m_input;
    std::unique_ptr<QTemporaryFile> m_output;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
};
