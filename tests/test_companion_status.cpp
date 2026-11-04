// SPDX-License-Identifier: Apache-2.0
#include "companion_status_sync.h"
#include "session.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

class CompanionStatusTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_log;
    static QString peer()
    {
        return QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
    }
    QList<QStringList> commands() const
    {
        QFile file(m_log);
        if (!file.open(QIODevice::ReadOnly)) return {};
        QList<QStringList> result;
        for (const auto &line : file.readAll().split('\n')) {
            if (line.isEmpty()) continue;
            QStringList args;
            for (const auto &value : QJsonDocument::fromJson(line).array()) args << value.toString();
            result << args;
        }
        return result;
    }
    QStringList states() const
    {
        QStringList result;
        for (const auto &args : commands()) {
            if (args.contains("broadcast")) result << args.value(args.indexOf("state") + 1);
        }
        return result;
    }
private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_log = m_dir.filePath("commands.jsonl");
        QFile::remove(m_log);
        qputenv("OTG_TEST_COMPANION_LOG", m_log.toUtf8());
    }
    void cleanup()
    {
        qunsetenv("OTG_TEST_COMPANION_LOG");
        qunsetenv("OTG_TEST_COMPANION_DELAY");
        qunsetenv("OTG_TEST_COMPANION");
        qunsetenv("OTG_TEST_BLE_RECONNECT");
    }
    void lifecycle()
    {
        Session session;
        ConnectionOptions options;
        options.manageServer = false;
        options.serial = "USB123";
        options.adbSerial = "[::1]:5555";
        options.adb = peer();
        options.bridge = peer();
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QTRY_VERIFY(states().contains("connected"));
        const auto count = states().count("connected");
        QTRY_VERIFY_WITH_TIMEOUT(states().count("connected") > count, 7000);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QTRY_COMPARE(states().last(), QString("disconnected"));
        for (const auto &args : commands()) {
            QCOMPARE(args.first(), "-s");
            QCOMPARE(args.value(1), options.adbSerial);
            QVERIFY(!args.contains("connect"));
            QVERIFY(!args.contains("start"));
            QVERIFY(!args.contains("grant"));
            if (args.contains("broadcast")) {
                QVERIFY(args.contains("com.github.gh6497.deskflowotg/.ConnectionStatusReceiver"));
                QVERIFY(args.contains("current"));
                QVERIFY(args.contains("version"));
            }
        }
    }
    void usbReadyIsNotConnected()
    {
        Session session;
        ConnectionOptions options;
        options.manageServer = false;
        options.serial = "slow";
        options.adb = peer();
        options.bridge = peer();
        session.start(options);
        QTRY_VERIFY(states().contains("connecting"));
        QTest::qWait(250);
        QCOMPARE(session.state(), Session::State::Starting);
        QVERIFY(!states().contains("connected"));
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QTRY_VERIFY(states().contains("disconnected"));
    }
    void staleCallbacksAndTargetChange()
    {
        qputenv("OTG_TEST_COMPANION_DELAY", "250");
        CompanionStatusSync sync;
        sync.update(peer(), "OLD", "connected");
        QTRY_COMPARE(commands().size(), 1);
        sync.update(peer(), "NEW", "connecting");
        sync.update(peer(), "NEW", "disconnected");
        QTRY_VERIFY(states().contains("disconnected"));
        QCOMPARE(states(), QStringList{"disconnected"});
        QCOMPARE(commands().last().value(1), "NEW");
        QTest::qWait(350); // finish the last fake broadcast before destruction
    }
    void offlineDoesNotPublishOrBlockHid()
    {
        qputenv("OTG_TEST_COMPANION", "offline");
        Session session;
        ConnectionOptions options;
        options.manageServer = false;
        options.serial = "USB123";
        options.adb = peer();
        options.bridge = peer();
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        QTRY_VERIFY(!commands().isEmpty());
        QVERIFY(states().isEmpty());
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QTest::qWait(100);
        QVERIFY(states().isEmpty());
    }
    void noImplicitTarget()
    {
        CompanionStatusSync sync;
        sync.update(peer(), {}, "connected");
        sync.update(peer(), "-d", "connected");
        sync.update(peer(), "bad target", "connected");
        sync.update({}, "USB123", "connected");
        sync.update(peer(), "USB123", "invalid");
        QTest::qWait(100);
        QVERIFY(commands().isEmpty());
    }
    void independentDevices()
    {
        Session first, second;
        ConnectionOptions options;
        options.manageServer = false;
        options.bridge = peer();
        options.adb = peer();
        options.serial = "FIRST";
        first.start(options);
        options.serial = "SECOND";
        second.start(options);
        auto reports = [this](const QString &target, const QString &state) {
            for (const auto &args : commands()) {
                if (args.contains("broadcast") && args.value(1) == target &&
                    args.value(args.indexOf("state") + 1) == state) return true;
            }
            return false;
        };
        QTRY_VERIFY(reports("FIRST", "connected"));
        QTRY_VERIFY(reports("SECOND", "connected"));
        first.stop();
        QTRY_VERIFY(reports("FIRST", "disconnected"));
        QCOMPARE(second.state(), Session::State::Connected);
        QVERIFY(!reports("SECOND", "disconnected"));
        second.stop();
        QTRY_VERIFY(reports("SECOND", "disconnected"));
        QTRY_COMPARE(first.state(), Session::State::Idle);
        QTRY_COMPARE(second.state(), Session::State::Idle);
    }
    void bridgeCrash()
    {
        Session session;
        ConnectionOptions options;
        options.manageServer = false;
        options.serial = "crash";
        options.adb = peer();
        options.bridge = peer();
        session.start(options);
        QTRY_VERIFY(states().contains("connected"));
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QTRY_VERIFY(states().contains("disconnected"));
    }
    void bleReconnect()
    {
#if defined(Q_OS_LINUX) && defined(OTG_GUI_HAVE_BLE)
        qputenv("OTG_TEST_BLE_RECONNECT", "1");
        Session session;
        ConnectionOptions options;
        options.manageServer = false;
        options.transport = "ble";
        options.blePeer = "AA:BB:CC:DD:EE:FF";
        options.adbSerial = "phone._adb-tls-connect._tcp";
        options.adb = peer();
        options.bridge = peer();
        session.start(options);
        QTRY_VERIFY(states().count("connected") >= 2);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QTRY_VERIFY(states().contains("disconnected"));
        for (const auto &args : commands()) QCOMPARE(args.value(1), options.adbSerial);
        qunsetenv("OTG_TEST_BLE_RECONNECT");
        QFile::remove(m_log);
        // With no explicit ADB identity, even an active BLE session sends nothing.
        options.adbSerial.clear();
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(commands().isEmpty());
#else
        QSKIP("Linux fake BLE lifecycle requires BUILD_BLE");
#endif
    }
    void timeoutCoalescesToLatest()
    {
        qputenv("OTG_TEST_COMPANION_DELAY", "5000");
        CompanionStatusSync sync;
        sync.update(peer(), "USB123", "connected");
        QTRY_COMPARE(commands().size(), 1);
        sync.update(peer(), "USB123", "disconnected");
        qunsetenv("OTG_TEST_COMPANION_DELAY");
        QTRY_VERIFY_WITH_TIMEOUT(states().contains("disconnected"), 4000);
        QCOMPARE(states(), QStringList{"disconnected"});
    }
};

QTEST_GUILESS_MAIN(CompanionStatusTest)
#include "test_companion_status.moc"
