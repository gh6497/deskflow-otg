// SPDX-License-Identifier: Apache-2.0
#include "companion_transfer.h"
#include "transfer_dialog.h"
#include "i18n.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QListWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QtTest>

// Windows CI can spend several seconds starting the fake ADB process while
// the test suite is under load. Keep the asynchronous assertions deterministic
// without changing the production command timeouts.
#define OTG_QTRY_TIMEOUT_MS 15000
#define OTG_QTRY_COMPARE(actual, expected) \
    QTRY_COMPARE_WITH_TIMEOUT(actual, expected, OTG_QTRY_TIMEOUT_MS)
#define OTG_QTRY_VERIFY(condition) \
    QTRY_VERIFY_WITH_TIMEOUT(condition, OTG_QTRY_TIMEOUT_MS)

class TransferTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    const QString id = QString(32, 'a');
    QByteArray content;
    static QString peer()
    {
        return QCoreApplication::applicationDirPath() + "/test_transfer_peer"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
    }
    void write(const QString &name, const QByteArray &bytes)
    {
        QFile f(m_dir.filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size());
    }
    QList<QStringList> commands()
    {
        QFile f(m_dir.filePath("commands"));
        if (!f.open(QIODevice::ReadOnly)) return {};
        QList<QStringList> result;
        for (const auto &line : f.readAll().split('\n')) {
            if (line.isEmpty()) continue;
            QStringList args;
            for (const auto &v : QJsonDocument::fromJson(line).array()) args << v.toString();
            result << args;
        }
        return result;
    }
    void load(CompanionTransfer &transfer)
    {
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.refresh(); OTG_QTRY_COMPARE(spy.size(), 1); QVERIFY(spy.first().first().toBool());
        QCOMPARE(transfer.files().size(), 1);
    }
    QString receiveDir()
    {
        const auto path = m_dir.filePath("received"); QDir().mkpath(path); return path;
    }
    void clearDir(const QString &path)
    {
        const QDir dir(path);
        for (const auto &name : dir.entryList(QDir::Files | QDir::Hidden)) QVERIFY(QFile::remove(dir.filePath(name)));
    }
private slots:
    void initTestCase()
    {
        QVERIFY(LanguageManager::instance().setLanguage("zh_CN"));
    }
    void init()
    {
        QVERIFY(m_dir.isValid());
        clearDir(m_dir.path()); clearDir(receiveDir());
        qputenv("OTG_TEST_TRANSFER_DIR", m_dir.path().toUtf8());
        qunsetenv("OTG_TEST_TRANSFER_MODE");
        content.clear();
        for (int i = 0; i < 256 * 40; ++i) content.append(char(i % 256));
        write(id + ".data", content);
        const QJsonObject file{{"id", id}, {"name", "../../图片;$(noop).png"}, {"size", content.size()},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex())}};
        write("manifest.json", QJsonDocument(QJsonObject{{"version", 1}, {"files", QJsonArray{file}}}).toJson());
    }
    void cleanup()
    {
        qunsetenv("OTG_TEST_TRANSFER_DIR"); qunsetenv("OTG_TEST_TRANSFER_MODE");
    }
    void receiveBinary()
    {
        CompanionTransfer transfer(peer(), "[::1]:5555"); load(transfer);
        QCOMPARE(commands().size(), 3); // Refresh never downloads or acknowledges files.
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.receive({id}, receiveDir()); OTG_QTRY_COMPARE(spy.size(), 1);
        QVERIFY(spy.first().first().toBool());
        const auto names = QDir(receiveDir()).entryList(QDir::Files);
        QCOMPARE(names.size(), 1); QVERIFY(!names.first().contains('/'));
        QFile file(QDir(receiveDir()).filePath(names.first())); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), content);
        for (const auto &args : commands()) {
            QCOMPARE(args.value(0), "-s"); QCOMPARE(args.value(1), "[::1]:5555");
            QVERIFY(!args.contains("connect")); QVERIFY(!args.contains("pair")); QVERIFY(!args.contains("start"));
            QVERIFY(!args.contains("grant")); QVERIFY(!args.contains("kill-server"));
            QVERIFY(!args.contains("../../图片;$(noop).png"));
            if (args.contains("content")) QCOMPARE(args.value(args.indexOf("--user") + 1), "10");
        }
        QSignalSpy refreshed(&transfer, &CompanionTransfer::completed);
        transfer.refresh(); OTG_QTRY_COMPARE(refreshed.size(), 1); QVERIFY(transfer.files().isEmpty());
    }
    void sendBinaryAndEmpty()
    {
        write("你好 '$; image.png", content); write("empty", {});
        CompanionTransfer transfer(peer(), "[::1]:5555");
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.send({m_dir.filePath("你好 '$; image.png"), m_dir.filePath("empty")});
        OTG_QTRY_COMPARE(spy.size(), 1); QVERIFY(spy.first().first().toBool());
        const auto sent = QDir(m_dir.path()).entryList({"*.wire"}, QDir::Files);
        QCOMPARE(sent.size(), 2);
        // The peer verified magic, name length, BE64 size, NUL/CR/LF bytes and SHA-256.
        for (const auto &args : commands()) {
            QVERIFY(!args.contains(m_dir.filePath("你好 '$; image.png")));
            QVERIFY(!args.contains("你好 '$; image.png"));
            if (args.contains("write"))
                QCOMPARE(args.mid(2, 4), (QStringList{"shell", "-T", "content", "write"}));
        }
    }
    void sendBeyondPipeCapacity()
    {
        const QByteArray large = content.repeated(128);
        write("large-binary", large);
        CompanionTransfer transfer(peer(), "[::1]:5555");
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.send({m_dir.filePath("large-binary")});
        OTG_QTRY_COMPARE(spy.size(), 1); QVERIFY(spy.first().first().toBool());
        const auto sent = QDir(m_dir.path()).entryList({"*.wire"}, QDir::Files);
        QCOMPARE(sent.size(), 1);
        QFile file(m_dir.filePath(sent.first())); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto wire = file.readAll();
        QVERIFY(wire.endsWith(QCryptographicHash::hash(large, QCryptographicHash::Sha256)));
        QCOMPARE(wire.mid(20 + QByteArray("large-binary").size(), large.size()), large);
    }
    void progressWaitsForReceipt()
    {
        TransferDialog dialog(peer(), "[::1]:5555");
        auto *bar = dialog.findChild<QProgressBar *>(); QVERIFY(bar);
        auto *transfer = dialog.findChild<CompanionTransfer *>(); QVERIFY(transfer);
        QSignalSpy initial(transfer, &CompanionTransfer::completed);
        OTG_QTRY_COMPARE(initial.size(), 1);
        transfer->progress("file", 100, 100);
        QCOMPARE(bar->value(), 99);
        transfer->completed(false, "otg.transfer.failed", {});
        QCOMPARE(bar->value(), 99);
        transfer->completed(true, "otg.transfer.done", {});
        QCOMPARE(bar->value(), 100);
        transfer->progress("empty", 0, 0);
        QCOMPARE(bar->value(), 99);
    }
    void rejectedDownloads_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("hash") << QByteArray("corrupt");
        QTest::newRow("short") << QByteArray("truncated");
        QTest::newRow("long") << QByteArray("extra");
    }
    void rejectedDownloads()
    {
        QFETCH(QByteArray, mode);
        CompanionTransfer transfer(peer(), "[::1]:5555"); load(transfer);
        qputenv("OTG_TEST_TRANSFER_MODE", mode);
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.receive({id}, receiveDir()); OTG_QTRY_COMPARE(spy.size(), 1);
        QVERIFY(!spy.first().first().toBool());
        QVERIFY(QDir(receiveDir()).entryList(QDir::Files | QDir::Hidden).isEmpty());
        for (const auto &args : commands()) QVERIFY(!args.contains("delete"));
    }
    void doNotOverwrite()
    {
        CompanionTransfer transfer(peer(), "[::1]:5555"); load(transfer);
        const auto name = id.left(8) + "-" + transfer.files().first().name;
        write("received/" + name, "existing");
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.receive({id}, receiveDir()); OTG_QTRY_COMPARE(spy.size(), 1);
        QVERIFY(!spy.first().first().toBool());
        QFile f(QDir(receiveDir()).filePath(name)); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), "existing");
        QCOMPARE(QDir(receiveDir()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }
    void acknowledgementFailure()
    {
        CompanionTransfer transfer(peer(), "[::1]:5555"); load(transfer);
        qputenv("OTG_TEST_TRANSFER_MODE", "ack-error");
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.receive({id}, receiveDir()); OTG_QTRY_COMPARE(spy.size(), 1);
        QCOMPARE(spy.first().at(1).toString(), "otg.transfer.ack_failed");
        QCOMPARE(QDir(receiveDir()).entryList(QDir::Files).size(), 1);
    }
    void cancelDownload()
    {
        CompanionTransfer transfer(peer(), "[::1]:5555"); load(transfer);
        qputenv("OTG_TEST_TRANSFER_MODE", "hold");
        QSignalSpy progress(&transfer, &CompanionTransfer::progress);
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.receive({id}, receiveDir()); OTG_QTRY_VERIFY(!progress.isEmpty());
        transfer.cancel(); OTG_QTRY_COMPARE(spy.size(), 1);
        QCOMPARE(spy.first().at(1).toString(), "otg.transfer.cancelled");
        QVERIFY(QDir(receiveDir()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    }
    void boundedUploadAndCancel_data()
    {
        QTest::addColumn<bool>("fillPipe");
        QTest::newRow("immediate") << false;
        QTest::newRow("pipe-buffered") << true;
    }
    void boundedUploadAndCancel()
    {
        QFETCH(bool, fillPipe);
        constexpr qint64 fileSize = 4 * 1024 * 1024;
        write("large", QByteArray(fileSize, 'x'));
        qputenv("OTG_TEST_TRANSFER_MODE", "hold");
        CompanionTransfer transfer(peer(), "[::1]:5555");
        QSignalSpy progress(&transfer, &CompanionTransfer::progress);
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.send({m_dir.filePath("large")}); OTG_QTRY_VERIFY(progress.size() > 2);
        // Exercise both cancellation timings. This wait deliberately lets pipe
        // writes complete; no assertion depends on completing them by a deadline.
        if (fillPipe) QTest::qWait(200);
        // Progress counts bytes read from the file, including data already in
        // the OS pipe (Qt uses a 1 MiB pipe on Windows). Only the application
        // queue has a portable bound: the 256 KiB watermark plus one chunk.
        const auto queuedBytes = [&transfer]() -> qint64 {
            for (auto *process : transfer.findChildren<QProcess *>())
                if (process->state() == QProcess::Running) return process->bytesToWrite();
            return -1;
        };
        OTG_QTRY_VERIFY(queuedBytes() > 0);
        QVERIFY(queuedBytes() <= 256 * 1024 + 65536);
        QVERIFY(progress.last().at(1).toLongLong() < fileSize);
        QVERIFY(transfer.busy());
        QVERIFY(spy.isEmpty());
        transfer.cancel(); OTG_QTRY_COMPARE(spy.size(), 1); QVERIFY(!spy.first().first().toBool());
        QCOMPARE(spy.first().at(1).toString(), "otg.transfer.cancelled");
        QVERIFY(!transfer.busy());
    }
    void failClosed_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("offline") << QByteArray("offline");
        QTest::newRow("old-apk") << QByteArray("missing");
        QTest::newRow("oversized-manifest") << QByteArray("huge");
    }
    void failClosed()
    {
        QFETCH(QByteArray, mode); qputenv("OTG_TEST_TRANSFER_MODE", mode);
        CompanionTransfer transfer(peer(), "[::1]:5555");
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.refresh(); OTG_QTRY_COMPARE(spy.size(), 1);
        QVERIFY(!spy.first().first().toBool()); QVERIFY(transfer.files().isEmpty());
    }
    void invalidTarget()
    {
        for (const auto &target : QStringList{"", "-d", "bad\nserial"}) {
            CompanionTransfer transfer(peer(), target);
            QSignalSpy spy(&transfer, &CompanionTransfer::completed);
            transfer.refresh(); QCOMPARE(spy.size(), 1); QVERIFY(!spy.first().first().toBool());
        }
        QVERIFY(commands().isEmpty());
    }
    void commandTimeout()
    {
        qputenv("OTG_TEST_TRANSFER_MODE", "hang-online");
        CompanionTransfer transfer(peer(), "[::1]:5555");
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.refresh(); QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 7000);
        QVERIFY(!spy.first().first().toBool()); QVERIFY(!transfer.busy());
    }
    void dialogLifecycle()
    {
        TransferDialog dialog(peer(), "[::1]:5555"); dialog.show();
        auto *list = dialog.findChild<QListWidget *>("sharedFiles"); QVERIFY(list);
        OTG_QTRY_COMPARE(list->count(), 1);
        auto *send = dialog.findChild<QPushButton *>("transferSend"); QVERIFY(send);
        OTG_QTRY_VERIFY(send->isEnabled());
        const auto screenshot = qEnvironmentVariable("OTG_TEST_TRANSFER_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(dialog.grab().save(screenshot));
        dialog.reject(); QVERIFY(!dialog.isVisible());
    }
    void badReceipt()
    {
        write("file", content);
        qputenv("OTG_TEST_TRANSFER_MODE", "bad-receipt");
        CompanionTransfer transfer(peer(), "[::1]:5555");
        QSignalSpy spy(&transfer, &CompanionTransfer::completed);
        transfer.send({m_dir.filePath("file")}); OTG_QTRY_COMPARE(spy.size(), 1);
        QVERIFY(!spy.first().first().toBool());
    }
    void untrustedMetadata()
    {
        QList<SharedFile> files;
        QVERIFY(!CompanionTransfer::parseManifest("{}", files));
        QVERIFY(!CompanionTransfer::parseManifest("{\"version\":2,\"files\":[]}", files));
        QFile f(m_dir.filePath("manifest.json")); QVERIFY(f.open(QIODevice::ReadOnly));
        auto obj = QJsonDocument::fromJson(f.readAll()).object();
        auto file = obj["files"].toArray().first().toObject();
        file["size"] = double(CompanionTransfer::MaxFile) + 1;
        obj["files"] = QJsonArray{file};
        QVERIFY(!CompanionTransfer::parseManifest(QJsonDocument(obj).toJson(), files));
        QCOMPARE(CompanionTransfer::safeName("CON.txt"), "_CON.txt");
        QCOMPARE(CompanionTransfer::safeName("../a\\b\n.txt"), ".._a_b_.txt");
        QCOMPARE(CompanionTransfer::safeName(".."), "file");
    }
};
QTEST_MAIN(TransferTest)
#include "test_companion_transfer.moc"
