// SPDX-License-Identifier: Apache-2.0
#include "device_info.h"
#include "process_utils.h"
#include "session.h"
#include <QSignalSpy>
#include <QTcpServer>
#include <QScopeGuard>
#include <QtTest>

static QString peerPath()
{
    return QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ;
}

class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void devices()
    {
        auto devices = parseDevices("* daemon started successfully *\nList of devices attached\n"
            "ABC device usb:1-2 product:p model:Pixel_8 transport_id:1\n"
            "DEF unauthorized usb:1-3\nGHI offline\n"
            "emulator-5554 device\n192.168.1.2:5555 device\n"
            "adb-foo._adb-tls-connect._tcp device\n");
        QCOMPARE(devices.size(), 3);
        QCOMPARE(devices[0].serial, "ABC");
        QCOMPARE(devices[0].model, "Pixel 8");
        QCOMPARE(devices[1].state, "unauthorized");
    }
    void dimensions()
    {
        QCOMPARE(parseScreenSize("Physical size: 1080x2400\r\nOverride size: 720x1600\r\n"), QSize(720, 1600));
        QCOMPARE(parseScreenSize("Override size: 720x1600\nPhysical size: 1080x2400"), QSize(720, 1600));
        QVERIFY(!parseScreenSize("error: device unauthorized").isValid());
        QVERIFY(!parseScreenSize("Physical size: 40000x0").isValid());
        QCOMPARE(parseRotation("SurfaceOrientation: 3\n"), 3);
        QCOMPARE(parseRotation("Viewport EXTERNAL: displayId=1, orientation=3\n"
                               "Viewport INTERNAL: displayId=0, uniqueId='local:1', orientation=1\n"), 1);
        QCOMPARE(parseRotation("unknown"), -1);
    }
    void configuration()
    {
        const auto config = serverConfiguration("pc", "android", "left");
        QVERIFY(config.contains("pc:\n    left = android"));
        QVERIFY(config.contains("android:\n    right = pc"));
        QVERIFY(serverConfiguration("pc", "PC", "left").isEmpty());
        QVERIFY(serverConfiguration("pc\nend", "android", "left").isEmpty());
        QVERIFY(serverConfiguration("pc", "android", "invalid").isEmpty());
        QVERIFY(!validScreenName("-c"));
    }
    void failedCommand()
    {
        bool finished = false;
        runCommand(this, "/nonexistent/deskflow-otg-command", {}, [&](bool ok, const QString &error) {
            QVERIFY(!ok);
            QVERIFY(!error.isEmpty());
            finished = true;
        });
        QTRY_VERIFY(finished);
    }
    void occupiedPort()
    {
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost));
        Session session;
        QSignalSpy changes(&session, &Session::changed);
        ConnectionOptions options;
        options.port = occupied.serverPort();
        options.serial = "ABC";
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        QVERIFY(changes.last()[1].toString().contains("端口"));
        QVERIFY(occupied.isListening());
    }
    void missingBridge()
    {
        Session session;
        ConnectionOptions options;
        options.manageServer = false;
        options.serial = "ABC";
        options.bridge = "/nonexistent/deskflow-otg";
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Idle);
    }
    void managedLifecycle_data()
    {
        QTest::addColumn<QByteArray>("style");
        QTest::newRow("modern") << QByteArray("modern");
        QTest::newRow("legacy-optin") << QByteArray("legacy-optin");
        QTest::newRow("legacy-disable") << QByteArray("legacy-disable");
        QTest::newRow("legacy-settings") << QByteArray("legacy-settings");
    }
    void managedLifecycle()
    {
        QFETCH(QByteArray, style);
        qputenv("OTG_TEST_HELP", style);
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_HELP"); });
        QTcpServer allocator;
        QVERIFY(allocator.listen(QHostAddress::LocalHost));
        ConnectionOptions options;
        options.port = allocator.serverPort();
        allocator.close();
        options.serial = "ABC";
        options.bridge = peerPath();
        options.deskflow = peerPath();
        Session session;
        QSignalSpy logs(&session, &Session::log);
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), Session::State::Idle, 6000);
        bool released = false;
        for (const auto &entry : logs)
            released |= entry[0].toString().contains("released");
        QVERIFY(released);
        QVERIFY(allocator.listen(QHostAddress::LocalHost, quint16(options.port)));
    }
    void externalLifecycle()
    {
        QTcpServer external;
        QVERIFY(external.listen(QHostAddress::LocalHost));
        ConnectionOptions options;
        options.manageServer = false;
        options.port = external.serverPort();
        options.serial = "ABC";
        options.bridge = peerPath();
        Session session;
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Connected);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        QVERIFY(external.isListening());
        // Cancellation while startup has not emitted connected.
        options.serial = "slow";
        session.start(options);
        QTest::qWait(100);
        QCOMPARE(session.state(), Session::State::Starting);
        session.stop();
        QTRY_COMPARE(session.state(), Session::State::Idle);
        // A process which starts successfully but rejects the device isn't connected.
        options.serial = "reject";
        QSignalSpy states(&session, &Session::changed);
        session.start(options);
        QTRY_COMPARE(session.state(), Session::State::Idle);
        for (const auto &entry : states)
            QVERIFY(qvariant_cast<Session::State>(entry[0]) != Session::State::Connected);
    }
};

QTEST_GUILESS_MAIN(GuiTest)
#include "test_gui.moc"
