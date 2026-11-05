// SPDX-License-Identifier: Apache-2.0
#include "companion_transfer.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <cmath>

namespace {
bool idValid(const QString &id) { return QRegularExpression("^[0-9a-f]{32}$").match(id).hasMatch(); }
bool entry(const QJsonObject &o, SharedFile &f)
{
    const double size = o.value("size").toDouble(-1);
    f.id = o.value("id").toString();
    f.name = CompanionTransfer::safeName(o.value("name").toString());
    f.sha256 = o.value("sha256").toString().toLatin1();
    if (!idValid(f.id) || !o.value("name").isString() || size < 0 ||
        !std::isfinite(size) || size > CompanionTransfer::MaxFile || std::floor(size) != size ||
        !QRegularExpression("^[0-9a-f]{64}$").match(QString::fromLatin1(f.sha256)).hasMatch()) return false;
    f.size = static_cast<qint64>(size);
    return true;
}
}

CompanionTransfer::CompanionTransfer(const QString &adb, const QString &target, QObject *parent)
    : QObject(parent), m_adb(adb), m_target(target)
{
    m_idle.setSingleShot(true);
    m_absolute.setSingleShot(true);
    m_receiptTimer.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, &CompanionTransfer::failCommand);
    connect(&m_absolute, &QTimer::timeout, this, &CompanionTransfer::failCommand);
    connect(&m_receiptTimer, &QTimer::timeout, this, &CompanionTransfer::receipt);
}

CompanionTransfer::~CompanionTransfer()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

QString CompanionTransfer::safeName(QString name)
{
    if (name.size() > 180) {
        name.truncate(180);
        if (name.back().isHighSurrogate()) name.chop(1);
    }
    name.replace(QRegularExpression("[\\x00-\\x1f\\x7f\\p{Cf}\\\\/:*?\"<>|]"), "_");
    name = name.trimmed();
    name.remove(QRegularExpression("[. ]+$"));
    if (name.isEmpty() || name == "." || name == "..") name = "file";
    if (QRegularExpression("^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\\..*)?$",
                           QRegularExpression::CaseInsensitiveOption).match(name).hasMatch()) name.prepend('_');
    while (name.toUtf8().size() > 180) {
        if (name.back().isLowSurrogate() && name.size() > 1) name.chop(2);
        else name.chop(1);
    }
    return name;
}

bool CompanionTransfer::parseManifest(const QByteArray &data, QList<SharedFile> &files)
{
    files.clear();
    QList<SharedFile> parsed;
    if (data.size() > 65536) return false;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(data, &error);
    const auto obj = doc.object();
    if (error.error != QJsonParseError::NoError || obj.value("version").toInt() != 1 ||
        !obj.value("files").isArray() || obj.value("files").toArray().size() > 32) return false;
    QSet<QString> ids;
    qint64 total = 0;
    for (const auto &v : obj.value("files").toArray()) {
        SharedFile f;
        if (!v.isObject() || !entry(v.toObject(), f) || ids.contains(f.id)) return false;
        total += f.size;
        if (total > 2 * MaxFile) return false;
        ids.insert(f.id); parsed.append(f);
    }
    files = parsed;
    return true;
}

QString CompanionTransfer::uri(const QString &path) const
{
    return "content://com.github.gh6497.deskflowotg.transfer/" + path;
}
QStringList CompanionTransfer::read(const QString &path) const
{
    return {"exec-out", "content", "read", "--user", m_user, "--uri", uri(path)};
}

bool CompanionTransfer::begin()
{
    if (m_busy) return false;
    if (m_adb.trimmed().isEmpty() || m_target.isEmpty() || m_target.startsWith('-') ||
        m_target.contains(QRegularExpression("[\\x00-\\x20\\x7f]"))) {
        finish(false, "otg.companion.target_required"); return false;
    }
    m_busy = true; m_cancelled = false; m_index = 0;
    m_receiptTimer.stop();
    return true;
}

void CompanionTransfer::online(Done done)
{
    command({"get-state"}, Stream::Capture, [this, done](bool ok) {
        if (!ok || m_capture.trimmed() != "device") { finish(false, "otg.transfer.failed"); return; }
        // Unlike `am`, Android's `content` CLI only accepts a numeric user ID.
        // Resolve it explicitly; never accidentally transfer to the system user.
        command({"shell", "am", "get-current-user"}, Stream::Capture, [this, done](bool current) {
            const auto user = QString::fromUtf8(m_capture).trimmed();
            bool valid = false;
            user.toInt(&valid);
            if (!current || !valid || !QRegularExpression("^[0-9]+$").match(user).hasMatch()) {
                finish(false, "otg.transfer.failed"); return;
            }
            m_user = user;
            done(true);
        }, 5000);
    }, 5000);
}

void CompanionTransfer::command(const QStringList &args, Stream stream, Done done, int timeout)
{
    if (m_cancelled) { finish(false, "otg.transfer.cancelled"); return; }
    m_capture.clear(); m_errors.clear(); m_failed = false; m_stream = stream;
    auto *process = new QProcess(this);
    m_process = process;
    process->setReadChannel(QProcess::StandardOutput);
    connect(process, &QProcess::readyReadStandardOutput, this, &CompanionTransfer::drain);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        const auto bytes = process->readAllStandardError();
        m_errors.append(bytes.left(qMax(0, 8192 - int(m_errors.size()))));
    });
    connect(process, &QProcess::started, this, [this, process] {
        if (m_stream != Stream::Upload) { process->closeWriteChannel(); return; }
        QByteArray header("DFOTGFT1", 8);
        const auto name = m_name.toUtf8();
        char length[4], size[8];
        qToBigEndian<quint32>(quint32(name.size()), length);
        qToBigEndian<quint64>(quint64(m_size), size);
        header.append(length, 4); header.append(name); header.append(size, 8);
        process->write(header);
        pump();
    });
    connect(process, &QProcess::bytesWritten, this, [this](qint64) {
        if (m_stream == Stream::Upload) { m_idle.start(30000); pump(); }
    });
    auto finishCommand = [this, process, done](bool ok) {
        if (m_process != process) return;
        drain();
        m_idle.stop(); m_absolute.stop();
        m_process = nullptr;
        process->deleteLater();
        if (m_cancelled) { finish(false, "otg.transfer.cancelled"); return; }
        done(ok && !m_failed);
    };
    connect(process, &QProcess::errorOccurred, this, [finishCommand](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finishCommand(false);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [finishCommand](int code, QProcess::ExitStatus status) {
        finishCommand(code == 0 && status == QProcess::NormalExit);
    });
    m_idle.start(timeout);
    m_absolute.start(stream == Stream::Capture ? timeout : 15 * 60 * 1000);
    // No host shell, and no sender-controlled names/paths in a remote command.
    process->start(m_adb, QStringList{"-s", m_target} + args);
}

void CompanionTransfer::drain()
{
    if (!m_process) return;
    while (m_process->bytesAvailable() > 0) {
        const auto bytes = m_process->read(65536);
        if (bytes.isEmpty()) break;
        m_idle.start(30000);
        if (m_stream == Stream::Download) {
            if (!m_output || m_bytes + bytes.size() > m_size || m_output->write(bytes) != bytes.size()) {
                failCommand(); return;
            }
            m_hash.addData(bytes); m_bytes += bytes.size();
            emit progress(m_name, m_bytes, m_size);
        } else {
            if (m_capture.size() + bytes.size() > 65536) { failCommand(); return; }
            m_capture.append(bytes);
        }
    }
}

void CompanionTransfer::pump()
{
    if (!m_process || m_stream != Stream::Upload || m_failed || m_cancelled || m_trailer) return;
    while (m_process->bytesToWrite() < 256 * 1024 && m_bytes < m_size) {
        const auto bytes = m_input.read(qMin<qint64>(65536, m_size - m_bytes));
        if (bytes.isEmpty()) { failCommand(); return; }
        m_hash.addData(bytes); m_bytes += bytes.size();
        if (m_process->write(bytes) != bytes.size()) { failCommand(); return; }
        emit progress(m_name, m_bytes, m_size);
    }
    if (m_bytes == m_size) {
        // Detect a source growing during upload instead of silently truncating it.
        if (!m_input.atEnd() || m_input.size() != m_size) { failCommand(); return; }
        m_trailer = true;
        m_process->write(m_hash.result());
        m_process->closeWriteChannel();
    }
}

void CompanionTransfer::failCommand()
{
    m_failed = true;
    if (m_process) m_process->kill();
}

void CompanionTransfer::finish(bool ok, const char *statusId, const QString &detail)
{
    m_busy = false;
    m_receiptTimer.stop();
    m_input.close();
    m_output.reset(); // Incomplete downloads are removed automatically.
    emit completed(ok, QString::fromLatin1(statusId), detail);
}

void CompanionTransfer::cancel()
{
    if (!m_busy) return;
    m_cancelled = true; m_receiptTimer.stop();
    if (m_process) m_process->kill();
    else finish(false, "otg.transfer.cancelled");
}

void CompanionTransfer::refresh()
{
    if (!begin()) return;
    m_files.clear();
    online([this](bool) {
        command(read("manifest"), Stream::Capture, [this](bool ok) {
            if (!ok || !parseManifest(m_capture, m_files)) { finish(false, "otg.transfer.failed"); return; }
            emit listed();
            finish(true, "otg.transfer.ready");
        });
    });
}

void CompanionTransfer::receive(const QStringList &ids, const QString &directory)
{
    if (!begin()) return;
    m_receiving.clear(); m_directory = directory;
    QSet<QString> selected;
    for (const auto &id : ids) {
        bool found = false;
        for (const auto &f : m_files) if (f.id == id && !selected.contains(id)) {
            selected.insert(id); m_receiving.append(f); found = true; break;
        }
        if (!found) { finish(false, "otg.transfer.failed"); return; }
    }
    if (m_receiving.isEmpty() || !QFileInfo(directory).isDir()) { finish(false, "otg.transfer.failed"); return; }
    online([this](bool) { nextReceive(); });
}

void CompanionTransfer::nextReceive()
{
    if (m_index >= m_receiving.size()) { finish(true, "otg.transfer.done"); return; }
    const auto f = m_receiving.at(m_index);
    m_name = f.name; m_size = f.size; m_bytes = 0; m_hash.reset();
    m_output = std::make_unique<QTemporaryFile>(QDir(m_directory).filePath(".deskflow-XXXXXX.part"));
    if (!m_output->open()) { finish(false, "otg.transfer.failed"); return; }
    emit progress(m_name, 0, m_size);
    command(read("out/" + f.id), Stream::Download, [this, f](bool ok) {
        if (!ok || m_bytes != f.size || m_hash.result().toHex() != f.sha256 || !m_output->flush()) {
            finish(false, "otg.transfer.failed"); return;
        }
        // Prefix avoids collisions on all platforms; QFile::rename never overwrites.
        const auto path = QDir(m_directory).filePath(f.id.left(8) + "-" + f.name);
        m_output->close();
        if (QFileInfo::exists(path) || !m_output->rename(path)) { finish(false, "otg.transfer.failed"); return; }
        m_output->setAutoRemove(false); m_output.reset();
        command({"shell", "content", "delete", "--user", m_user, "--uri", uri("out/" + f.id)},
                Stream::Capture, [this, f](bool ack) {
            if (!ack) { finish(false, "otg.transfer.ack_failed"); return; }
            // `content delete` may return exit 0 even on a provider error and
            // prints no success count. Re-read the manifest to confirm removal.
            command(read("manifest"), Stream::Capture, [this, f](bool readOk) {
                QList<SharedFile> pending;
                bool removed = readOk && parseManifest(m_capture, pending);
                for (const auto &p : pending) if (p.id == f.id) removed = false;
                if (!removed) { finish(false, "otg.transfer.ack_failed"); return; }
                ++m_index; nextReceive();
            });
        });
    });
}

void CompanionTransfer::send(const QStringList &paths)
{
    if (!begin()) return;
    if (paths.isEmpty() || paths.size() > 32) { finish(false, "otg.transfer.failed"); return; }
    qint64 total = 0;
    for (const auto &path : paths) {
        const QFileInfo f(path);
        if (!f.isFile() || !f.isReadable() || f.size() < 0 || f.size() > MaxFile) {
            finish(false, "otg.transfer.failed"); return;
        }
        total += f.size();
    }
    if (total > 2 * MaxFile) { finish(false, "otg.transfer.failed"); return; }
    m_paths = paths;
    online([this](bool) { nextSend(); });
}

void CompanionTransfer::nextSend()
{
    if (m_index >= m_paths.size()) { finish(true, "otg.transfer.done"); return; }
    m_input.close(); m_input.setFileName(m_paths.at(m_index));
    if (!m_input.open(QIODevice::ReadOnly) || m_input.size() > MaxFile) {
        finish(false, "otg.transfer.failed"); return;
    }
    m_name = safeName(QFileInfo(m_input).fileName()); m_size = m_input.size();
    m_uploadId = QUuid::createUuid().toString(QUuid::Id128);
    m_bytes = 0; m_hash.reset(); m_trailer = false;
    emit progress(m_name, 0, m_size);
    // The raw exec service cannot half-close stdin: exec-in can terminate the
    // remote writer before it drains the upload. Shell v2 forwards stdin EOF
    // independently and waits for the remote command. -T keeps binary data raw.
    command({"shell", "-T", "content", "write", "--user", m_user, "--uri", uri("in/" + m_uploadId)},
            Stream::Upload, [this](bool ok) {
        m_input.close();
        if (!ok || !m_trailer) { finish(false, "otg.transfer.failed"); return; }
        m_receiptAttempts = 0; receipt();
    });
}

void CompanionTransfer::receipt()
{
    command(read("receipt/" + m_uploadId), Stream::Capture, [this](bool ok) {
        const auto doc = QJsonDocument::fromJson(m_capture);
        const auto obj = doc.object();
        SharedFile f;
        if (!ok || obj.value("version").toInt() != 1) { finish(false, "otg.transfer.failed"); return; }
        if (obj.value("pending").toBool() && ++m_receiptAttempts < 20) {
            m_receiptTimer.start(250); return;
        }
        if (!entry(obj.value("file").toObject(), f) || f.id != m_uploadId || f.size != m_size ||
            f.sha256 != m_hash.result().toHex()) { finish(false, "otg.transfer.failed"); return; }
        ++m_index; nextSend();
    }, 3000);
}
