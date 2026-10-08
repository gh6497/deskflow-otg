// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QScopeGuard>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

class WindowTest : public QObject {
    Q_OBJECT
private slots:
    void discoverAndRender_data()
    {
        QTest::addColumn<bool>("noDevpath");
        QTest::newRow("usb-path") << false;
        QTest::newRow("windows-native-usb") << true;
    }
    void discoverAndRender()
    {
        QFETCH(bool, noDevpath);
        if (noDevpath)
            qputenv("OTG_TEST_NO_DEVPATH", "1");
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_NO_DEVPATH"); });
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("window-test");
        const QString peer = QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
        QSettings settings;
        settings.setValue("adb", peer);
        settings.setValue("deskflow", peer);
        MainWindow window;
        window.show();
        auto *width = window.findChild<QSpinBox *>("screenWidth");
        auto *height = window.findChild<QSpinBox *>("screenHeight");
        auto *serial = window.findChild<QLineEdit *>("usbSerial");
        auto *connectButton = window.findChild<QPushButton *>("connectButton");
        QVERIFY(width && height && serial && connectButton);
        QTRY_COMPARE(width->value(), 1600);
        QCOMPARE(height->value(), 720);
        QCOMPARE(serial->text(), "TEST123");
        QVERIFY(connectButton->isEnabled());
        auto *multiplier = window.findChild<QDoubleSpinBox *>("sensitivityMultiplier");
        auto *virtualSize = window.findChild<QLabel *>("virtualScreenSize");
        auto *swap = window.findChild<QPushButton *>("swapDimensions");
        auto *mode = window.findChild<QComboBox *>("mouseMode");
        QVERIFY(multiplier && virtualSize && swap && mode);
        width->setValue(1080);
        height->setValue(2340);
        multiplier->setValue(1.5);
        QVERIFY(virtualSize->text().startsWith("720 × 1560"));
        multiplier->setValue(2.0);
        QVERIFY(virtualSize->text().startsWith("540 × 1170"));
        multiplier->setValue(1.5);
        QVERIFY(virtualSize->text().startsWith("720 × 1560"));
        QCOMPARE(width->value(), 1080);
        QCOMPARE(height->value(), 2340);
        swap->click();
        QVERIFY(virtualSize->text().startsWith("1560 × 720"));
        swap->click();
        mode->setCurrentIndex(mode->findData("relative"));
        QVERIFY(!multiplier->isEnabled());
        QVERIFY(virtualSize->text().startsWith("1080 × 2340"));
        mode->setCurrentIndex(mode->findData("absolute"));
        QVERIFY(multiplier->isEnabled());
        QVERIFY(virtualSize->text().startsWith("720 × 1560"));
        width->setValue(1);
        height->setValue(3);
        multiplier->setValue(10.0);
        QVERIFY(virtualSize->text().startsWith("1 × 1"));
        width->setValue(1080);
        height->setValue(2340);
        multiplier->setValue(1.0);
        QVERIFY(virtualSize->text().startsWith("1080 × 2340"));
        multiplier->setValue(1.5);
        const QString capture = qEnvironmentVariable("OTG_TEST_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        window.close();
        QCOMPARE(settings.value("originalWidth").toInt(), 1080);
        QCOMPARE(settings.value("originalHeight").toInt(), 2340);
        QCOMPARE(settings.value("sensitivityMultiplier").toDouble(), 1.5);
        QCOMPARE(settings.value("width").toInt(), 720);
        QCOMPARE(settings.value("height").toInt(), 1560);
        MainWindow restored;
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1080);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("sensitivityMultiplier")->value(), 1.5);
        QVERIFY(restored.findChild<QLabel *>("virtualScreenSize")->text().startsWith("720 × 1560"));
        // ADB refresh replaces the baseline, retaining the chosen multiplier.
        QTRY_COMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1600);
        QTRY_VERIFY(restored.findChild<QPushButton *>("connectButton")->isEnabled());
        QVERIFY(restored.findChild<QLabel *>("virtualScreenSize")->text().startsWith("1067 × 480"));
        restored.close();
    }
};

QTEST_MAIN(WindowTest)
#include "test_window.moc"
