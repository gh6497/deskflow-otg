// SPDX-License-Identifier: Apache-2.0
// Binary fake ADB. Never accesses devices, the system ADB server, or the network.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <cstdio>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

static QByteArray load(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
static bool save(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
static void output(const QByteArray &bytes)
{
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const auto args = app.arguments().mid(1);
    const QDir dir(qEnvironmentVariable("OTG_TEST_TRANSFER_DIR"));
    if (!dir.exists() || args.value(0) != "-s" || args.value(1) != "[::1]:5555") return 99;
    QFile log(dir.filePath("commands"));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return 98;
    log.write(QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact) + '\n'); log.close();
    const auto mode = qEnvironmentVariable("OTG_TEST_TRANSFER_MODE");
    if (args.contains("get-state")) {
        if (mode == "hang-online") return app.exec();
        output(mode == "offline" ? "unauthorized\n" : "device\n"); return 0;
    }
    if (args.contains("get-current-user")) { output("10\n"); return 0; }
    if (args.value(args.indexOf("--user") + 1) != "10") return 97;
    const auto uri = args.value(args.indexOf("--uri") + 1);
    const QString prefix = "content://com.github.gh6497.deskflowotg.transfer/";
    if (!uri.startsWith(prefix)) return 96;
    const auto path = uri.mid(prefix.size());
    if (path == "manifest") {
        if (mode == "missing") { output("Error: provider not found"); return 0; }
        if (mode == "huge") { output(QByteArray(70000, 'x')); return 0; }
        output(load(dir.filePath("manifest.json"))); return 0;
    }
    if (path.startsWith("out/")) {
        if (args.contains("delete")) {
            if (mode != "ack-error") {
                auto manifest = QJsonDocument::fromJson(load(dir.filePath("manifest.json"))).object();
                QJsonArray pending;
                for (const auto &v : manifest.value("files").toArray())
                    if (v.toObject().value("id").toString() != path.mid(4)) pending.append(v);
                manifest.insert("files", pending);
                save(dir.filePath("manifest.json"), QJsonDocument(manifest).toJson(QJsonDocument::Compact));
            }
            return 0; // Actual Android content delete prints no success count.
        }
        if (mode == "hold") return app.exec();
        auto data = load(dir.filePath(path.mid(4) + ".data"));
        if (mode == "corrupt" && !data.isEmpty()) data[0] = char(data.at(0) ^ 1);
        if (mode == "truncated") data.chop(1);
        if (mode == "extra") data.append('x');
        output(data); return 0;
    }
    if (path.startsWith("in/")) {
        // Raw exec-in has no independent stdin EOF. Reproduce an exit-0 upload
        // that never commits, instead of treating it like an ordinary pipe.
        if (args.value(2) == "exec-in") return 0;
        const bool encoded = args.mid(2, 7) == QStringList{
            "shell", "-T", "base64", "-d", "|", "content", "write"};
        if (!encoded && args.mid(2, 4) != QStringList{"shell", "-T", "content", "write"}) return 89;
        if (mode == "hold") return app.exec();
        if (mode == "upload-rejected") {
            std::fputs("Error while accessing provider:com.github.gh6497.deskflowotg.transfer\n"
                       "java.io.IOException: Another upload is active\n", stderr);
            return 0;
        }
        QFile input;
        if (!input.open(stdin, QIODevice::ReadOnly)) return 95;
        auto data = input.readAll();
        if (mode == "text-stdin") {
            // Windows adb shell uses CRT text input even with -T. The old
            // binary fake hid Ctrl-Z EOF and CRLF translation by using _O_BINARY.
            save(dir.filePath(path.mid(3) + ".stdin"), data);
            const auto eof = data.indexOf('\x1a');
            if (eof >= 0) data.truncate(eof);
            data.replace("\r\n", "\n");
        }
        if (encoded) {
            const auto decoded = QByteArray::fromBase64Encoding(data, QByteArray::AbortOnBase64DecodingErrors);
            // Enforce a single canonical stream: padding inside separately
            // encoded chunks must not silently discard the rest of the file.
            if (!decoded || decoded.decoded.toBase64() != data) return 88;
            data = decoded.decoded;
        }
        if (data.size() < 52 || data.left(8) != "DFOTGFT1") return 94;
        const auto n = qFromBigEndian<quint32>(data.constData() + 8);
        if (n < 1 || n > 1024 || quint64(data.size()) < 52 + n) return 93;
        const auto size = qFromBigEndian<quint64>(data.constData() + 12 + n);
        if (size != quint64(data.size()) - 52 - n) return 92;
        const auto body = data.mid(20 + n, size);
        if (QCryptographicHash::hash(body, QCryptographicHash::Sha256) != data.right(32)) return 91;
        const auto id = path.mid(3);
        const QJsonObject receipt{{"version", 1}, {"file", QJsonObject{
            {"id", id}, {"name", QString::fromUtf8(data.mid(12, n))}, {"size", qint64(size)},
            {"sha256", QString::fromLatin1(data.right(32).toHex())}}}};
        save(dir.filePath(id + ".wire"), data);
        save(dir.filePath(id + ".receipt"), QJsonDocument(receipt).toJson(QJsonDocument::Compact));
        return 0;
    }
    if (path.startsWith("receipt/")) {
        if (mode == "bad-receipt") { output("{\"version\":1,\"file\":{}}"); return 0; }
        const auto receipt = load(dir.filePath(path.mid(8) + ".receipt"));
        output(receipt.isEmpty() ? QByteArray("{\"version\":1,\"pending\":true}") : receipt); return 0;
    }
    return 90;
}
