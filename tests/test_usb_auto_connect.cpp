// SPDX-License-Identifier: Apache-2.0
#include "usb_auto_connect.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

static QByteArray snapshot(std::initializer_list<QPair<QString, QString>> devices)
{
    QJsonArray array;
    for (const auto &device : devices)
        array.append(QJsonObject{{"id", device.first}, {"serial", device.second}});
    return QJsonDocument(QJsonObject{{"version", 1}, {"devices", array}}).toJson();
}

static QString peerPath()
{
    return QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ;
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

static int scanCount(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
}

static QProcess *watcher(UsbAutoConnect &monitor)
{
    for (auto *process : monitor.findChildren<QProcess *>()) {
        if (process->arguments().contains("--usb-watch") && process->state() == QProcess::Running)
            return process;
    }
    return nullptr;
}

class UsbAutoConnectTest : public QObject {
    Q_OBJECT
private slots:
    void hotplugNotifications()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto devices = dir.filePath("devices.json");
        const auto log = dir.filePath("scans.log");
        QVERIFY(writeFile(devices, snapshot({})));
        qputenv("OTG_TEST_USB_WATCH", "events");
        qputenv("OTG_TEST_USB_SNAPSHOT", devices.toUtf8());
        qputenv("OTG_TEST_USB_SCAN_LOG", log.toUtf8());
        const auto cleanup = qScopeGuard([] {
            qunsetenv("OTG_TEST_USB_WATCH");
            qunsetenv("OTG_TEST_USB_SNAPSHOT");
            qunsetenv("OTG_TEST_USB_SCAN_LOG");
            qunsetenv("OTG_TEST_USB_SCAN_DELAY");
        });
        UsbAutoConnect monitor;
        monitor.setProgram(peerPath());
        monitor.setKnownSerials({"phone"});
        monitor.setReady(true);
        QSignalSpy requests(&monitor, &UsbAutoConnect::connectRequested);
        QSignalSpy modes(&monitor, &UsbAutoConnect::detectionModeChanged);
        monitor.setEnabled(true);
        QTRY_VERIFY(monitor.usesHotplug());
        QTRY_COMPARE(scanCount(log), 3); // Initial snapshot plus bounded permission retries.
        QTest::qWait(2200);
        QCOMPARE(scanCount(log), 3); // No periodic USB scans in event mode.
        QPointer<QProcess> process = watcher(monitor);
        QVERIFY(process);
        const auto phone = snapshot({{"1:2", "phone"}});
        QVERIFY(writeFile(devices, phone));
        process->write("{\"event\":\"usb-added\",\"id\":\"1:2\"}\n");
        QTRY_COMPARE(requests.count(), 1);
        // A real departure re-arms the same address, even without an intervening
        // empty snapshot (rapid removal/reinsertion between scans).
        process->write("{\"event\":\"usb-removed\",\"id\":\"1:2\"}\n"
                       "{\"event\":\"usb-added\",\"id\":\"1:2\"}\n");
        QTRY_COMPARE(requests.count(), 2);
        // A departure while a scan is in flight must discard its stale results.
        qputenv("OTG_TEST_USB_SCAN_DELAY", "500");
        const int count = scanCount(log);
        monitor.poll();
        QTRY_VERIFY(scanCount(log) > count);
        QVERIFY(writeFile(devices, snapshot({})));
        process->write("{\"event\":\"usb-removed\",\"id\":\"1:2\"}\n");
        QTest::qWait(1300);
        QCOMPARE(requests.count(), 2);
        monitor.setEnabled(false);
        QTRY_VERIFY(!process || process->state() == QProcess::NotRunning);
        QCOMPARE(modes.count(), 1); // Intentional shutdown must not trigger fallback.
        monitor.setEnabled(true);
        QTRY_VERIFY(monitor.usesHotplug());
        QCOMPARE(modes.count(), 2);
        QVERIFY(watcher(monitor));
        monitor.setEnabled(false);
    }

    void pollingFallback_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("unsupported") << QByteArray("unsupported");
        QTest::newRow("registration-failed") << QByteArray("registration-failure");
        QTest::newRow("invalid-output") << QByteArray("malformed");
        QTest::newRow("startup-timeout") << QByteArray("timeout");
        QTest::newRow("listener-exited") << QByteArray("exit-after-ready");
    }

    void pollingFallback()
    {
        QFETCH(QByteArray, mode);
        qputenv("OTG_TEST_USB_WATCH", mode);
        const auto cleanup = qScopeGuard([] { qunsetenv("OTG_TEST_USB_WATCH"); });
        UsbAutoConnect monitor;
        monitor.setProgram(peerPath());
        monitor.setKnownSerials({"TEST123"});
        monitor.setReady(true);
        QSignalSpy requests(&monitor, &UsbAutoConnect::connectRequested);
        QSignalSpy modes(&monitor, &UsbAutoConnect::detectionModeChanged);
        monitor.setEnabled(true);
        QTRY_VERIFY(!modes.isEmpty() && !modes.last().at(0).toBool());
        QVERIFY(!monitor.usesHotplug());
        QTRY_COMPARE(requests.count(), 1);
        QVERIFY(monitor.acceptSnapshot(snapshot({})));
        QTRY_COMPARE(requests.count(), 2); // Periodic fallback continues after startup.
        monitor.setEnabled(false);
    }

    void missingWatcherFallsBack()
    {
        UsbAutoConnect monitor;
        monitor.setProgram("/nonexistent/otg-test-bridge");
        monitor.setKnownSerials({"phone"});
        monitor.setReady(true);
        QSignalSpy modes(&monitor, &UsbAutoConnect::detectionModeChanged);
        QSignalSpy failures(&monitor, &UsbAutoConnect::scanFailed);
        monitor.setEnabled(true);
        QTRY_COMPARE(modes.count(), 1);
        QCOMPARE(modes.at(0).at(0).toBool(), false);
        QTRY_COMPARE(failures.count(), 1);
        monitor.setEnabled(false);
    }

    void attachmentLifecycle()
    {
        UsbAutoConnect monitor;
        monitor.setKnownSerials({"phone"});
        monitor.setEnabled(true);
        monitor.setReady(true);
        QSignalSpy requests(&monitor, &UsbAutoConnect::connectRequested);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", "unknown"}})));
        QCOMPARE(requests.count(), 0);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", "phone"}})));
        QCOMPARE(requests.count(), 1);
        QCOMPARE(requests.at(0).at(0).toString(), "phone");
        // Success, manual disconnect, or connection failure all have the same
        // policy: do not retry this physical attachment.
        monitor.setReady(false);
        monitor.setReady(true);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", "phone"}})));
        QCOMPARE(requests.count(), 1);
        // Descriptor/permission failures and invalid snapshots are not unplug.
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", ""}})));
        QVERIFY(!monitor.acceptSnapshot("invalid"));
        QVERIFY(!monitor.acceptSnapshot("{\"version\":1,\"devices\":[{}]}"));
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", "phone"}})));
        QCOMPARE(requests.count(), 1);
        // A new USB address detects reinsertion even between polling intervals.
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:3", "phone"}})));
        QCOMPARE(requests.count(), 2);
        QVERIFY(monitor.acceptSnapshot(snapshot({})));
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:3", "phone"}})));
        QCOMPARE(requests.count(), 3);
    }

    void disabledBusyAndManual()
    {
        UsbAutoConnect monitor;
        monitor.setKnownSerials({"phone"});
        QSignalSpy requests(&monitor, &UsbAutoConnect::connectRequested);
        const auto phone = snapshot({{"1:2", "phone"}});
        monitor.setReady(true);
        QVERIFY(monitor.acceptSnapshot(phone));
        QCOMPARE(requests.count(), 0);
        monitor.setEnabled(true);
        monitor.setReady(false);
        QVERIFY(monitor.acceptSnapshot(phone));
        QCOMPARE(requests.count(), 0);
        monitor.suppressSerial("phone");
        QVERIFY(monitor.acceptSnapshot(phone));
        monitor.setReady(true);
        QVERIFY(monitor.acceptSnapshot(phone));
        QCOMPARE(requests.count(), 0);
        monitor.setEnabled(false);
        monitor.setEnabled(true);
        QVERIFY(monitor.acceptSnapshot(phone));
        QCOMPARE(requests.count(), 0);
        QVERIFY(monitor.acceptSnapshot(snapshot({})));
        QVERIFY(monitor.acceptSnapshot(phone));
        QCOMPARE(requests.count(), 1);
    }

    void ambiguityAndDeferredConnection()
    {
        UsbAutoConnect monitor;
        monitor.setKnownSerials({"one", "two"});
        monitor.setEnabled(true);
        monitor.setReady(true);
        QSignalSpy requests(&monitor, &UsbAutoConnect::connectRequested);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", "one"}, {"1:3", "two"}})));
        QCOMPARE(requests.count(), 0);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", "one"}, {"1:3", "one"}})));
        QCOMPARE(requests.count(), 0);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", "one"}})));
        QCOMPARE(requests.count(), 1);
        monitor.setReady(false);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:3", "two"}})));
        QCOMPARE(requests.count(), 1);
        monitor.setReady(true);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:3", "two"}})));
        QCOMPARE(requests.count(), 2);
        QCOMPARE(requests.at(1).at(0).toString(), "two");
    }

    void bridgeAttachmentBeforeFirstScan()
    {
        UsbAutoConnect monitor;
        monitor.setKnownSerials({"phone"});
        monitor.setEnabled(true);
        monitor.setReady(false);
        QSignalSpy requests(&monitor, &UsbAutoConnect::connectRequested);
        monitor.suppressSerial("phone");
        // On Windows, the scanner may not be able to open a device that the
        // bridge currently owns. The bridge supplies its actual bus/address.
        monitor.suppressAttachment("1:2", "phone");
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", ""}})));
        monitor.setReady(true);
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:2", "phone"}})));
        QCOMPARE(requests.count(), 0);
        QVERIFY(monitor.acceptSnapshot(snapshot({})));
        QVERIFY(monitor.acceptSnapshot(snapshot({{"1:3", "phone"}})));
        QCOMPARE(requests.count(), 1);
    }

    void subprocessProbe()
    {
        UsbAutoConnect monitor;
        monitor.setProgram(QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                           ".exe"
#endif
        );
        monitor.setKnownSerials({"TEST123"});
        monitor.setReady(true);
        QSignalSpy requests(&monitor, &UsbAutoConnect::connectRequested);
        QSignalSpy failures(&monitor, &UsbAutoConnect::scanFailed);
        monitor.setEnabled(true);
        QTRY_COMPARE(requests.count(), 1);
        QCOMPARE(requests.at(0).at(0).toString(), "TEST123");
        QCOMPARE(failures.count(), 0);
    }
};

QTEST_GUILESS_MAIN(UsbAutoConnectTest)
#include "test_usb_auto_connect.moc"
