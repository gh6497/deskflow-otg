// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include "i18n.h"
#include "build_version.h"
#include "system_theme.h"
#include "tls_certificate.h"
#include "transfer_dialog.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QScopeGuard>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QSystemTrayIcon>
#include <QPalette>
#include <QStyleHints>
#include <QListWidget>
#include <QTabWidget>
#include <QWheelEvent>
#include <QTcpServer>
#include <QtTest>

class TrayTestWindow : public MainWindow {
public:
    bool available = true;
protected:
    bool trayAvailable() const override { return available; }
};

class AutoConnectTestWindow : public TrayTestWindow {
protected:
    QString bridgeExecutable() const override
    {
        return QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
            ".exe"
#endif
            ;
    }
};

class WindowTest : public QObject {
    Q_OBJECT
private slots:
    void rightButtonSettings()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("language", "zh_CN");
        settings.setValue("manageServer", false);
        {
            AutoConnectTestWindow window;
            QCoreApplication::processEvents();
            auto *right = window.findChild<QComboBox *>("rightButtonAction");
            auto *middle = window.findChild<QComboBox *>("middleButtonAction");
            auto *list = window.findChild<QListWidget *>("deviceSidebar");
            auto *language = window.findChild<QComboBox *>("languageSelector");
            QVERIFY(right && middle && list && language);
            QCOMPARE(right->currentData().toString(), "normal");
            right->setCurrentIndex(right->findData("back"));
            QCOMPARE(middle->currentData().toString(), "normal");
            QCOMPARE(right->currentText(), "安卓返回键（Win+Backspace）");
            language->setCurrentIndex(language->findData("en"));
            QCOMPARE(right->currentText(), "Android Back (Win+Backspace)");
            QCOMPARE(right->currentData().toString(), "back");
            window.findChild<QPushButton *>("addDeviceButton")->click();
            QCOMPARE(list->count(), 2);
            right->setCurrentIndex(right->findData("normal"));
            middle->setCurrentIndex(middle->findData("home"));
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole) == "initial") list->setCurrentRow(i);
            QCOMPARE(right->currentData().toString(), "back");
            QCOMPARE(middle->currentData().toString(), "normal");
            window.findChild<QAction *>("trayQuit")->trigger();
        }
        AutoConnectTestWindow restored;
        auto *right = restored.findChild<QComboBox *>("rightButtonAction");
        auto *middle = restored.findChild<QComboBox *>("middleButtonAction");
        auto *list = restored.findChild<QListWidget *>("deviceSidebar");
        QCOMPARE(right->currentData().toString(), "back");
        QCOMPARE(middle->currentData().toString(), "normal");
        list->setCurrentRow(1);
        QCOMPARE(right->currentData().toString(), "normal");
        QCOMPARE(middle->currentData().toString(), "home");
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void fileSharingWindow()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", ""); settings.setValue("manageServer", false);
        qputenv("OTG_TEST_TRANSFER_DIR", dir.path().toUtf8());
        const auto cleanup = qScopeGuard([] {
            qunsetenv("OTG_TEST_TRANSFER_DIR"); qunsetenv("OTG_TEST_TRANSFER_MODE");
        });
        QFile manifest(dir.filePath("manifest.json")); QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.write("{\"version\":1,\"files\":[]}"); manifest.close();
        AutoConnectTestWindow window; window.show();
        QTest::qWait(20); // Initial no-ADB device discovery.
        auto *button = window.findChild<QPushButton *>("transferFiles"); QVERIFY(button);
        QVERIFY(!button->isEnabled());
        auto *adb = window.findChild<QLineEdit *>("adbExecutable");
        auto *target = window.findChild<QLineEdit *>("adbSerial");
        adb->setText(QCoreApplication::applicationDirPath() + "/test_transfer_peer"
#ifdef Q_OS_WIN
                     ".exe"
#endif
                     );
        target->setText("[::1]:5555");
        QVERIFY(button->isEnabled()); button->click();
        auto *dialog = window.findChild<TransferDialog *>("transferDialog"); QVERIFY(dialog);
        QPointer<CompanionTransfer> engine = dialog->findChild<CompanionTransfer *>(); QVERIFY(engine);
        QTRY_VERIFY(!engine->busy());
        // Changing selection/identity in the main form never retargets an open transfer window.
        target->setText("OTHER-PHONE");
        dialog->findChild<QPushButton *>("transferRefresh")->click();
        QTRY_VERIFY(!engine->busy());
        QFile log(dir.filePath("commands")); QVERIFY(log.open(QIODevice::ReadOnly));
        for (const auto &line : log.readAll().trimmed().split('\n'))
            QCOMPARE(QJsonDocument::fromJson(line).array()[1].toString(), "[::1]:5555");
        qputenv("OTG_TEST_TRANSFER_MODE", "hang-online");
        dialog->findChild<QPushButton *>("transferRefresh")->click();
        QVERIFY(engine->busy());
        QSignalSpy exit(&window, &MainWindow::exitReady);
        window.findChild<QAction *>("trayQuit")->trigger();
        QCOMPARE(exit.size(), 0); // Exit waits for cancellation of the ADB worker.
        QTRY_COMPARE(exit.size(), 1);
        QVERIFY(!engine || !engine->busy());
    }

    void numericFieldsIgnoreMouseWheel()
    {
        MainWindow window;
        auto *width = window.findChild<QSpinBox *>("screenWidth");
        auto *height = window.findChild<QSpinBox *>("screenHeight");
        auto *sensitivity = window.findChild<QDoubleSpinBox *>("sensitivityMultiplier");
        auto *port = window.findChild<QSpinBox *>("serverPort");
        QVERIFY(width && height && sensitivity && port);

        width->setValue(1080);
        height->setValue(2340);
        sensitivity->setValue(2.0);
        port->setValue(24800);

        const auto sendWheel = [](QWidget *widget) {
            const QPointF position(widget->rect().center());
            const QPointF globalPosition(widget->mapToGlobal(widget->rect().center()));
            QWheelEvent event(position, globalPosition, QPoint(), QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(widget, &event);
        };
        sendWheel(width);
        sendWheel(height);
        sendWheel(sensitivity);
        sendWheel(port);

        QCOMPARE(width->value(), 1080);
        QCOMPARE(height->value(), 2340);
        QCOMPARE(sensitivity->value(), 2.0);
        QCOMPARE(port->value(), 24800);
    }
    void companionApkDefaults()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        const auto bundled = QCoreApplication::applicationDirPath() + "/deskflow-otg-companion.apk";
        const bool existed = QFile::exists(bundled);
        if (!existed) {
            QFile file(bundled);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("fake APK");
        }
        const auto cleanup = qScopeGuard([&] { if (!existed) QFile::remove(bundled); });
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        const auto custom = dir.filePath("custom.apk");
        QFile file(custom);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("custom fake APK"); file.close();
        for (const auto &saved : QStringList{QString(), dir.filePath("old-install/companion.apk"), custom}) {
            settings.setValue("companionApk", saved);
            AutoConnectTestWindow window;
            QCOMPARE(window.findChild<QLineEdit *>("companionApk")->text(), saved == custom ? custom : bundled);
            QVERIFY(!window.findChild<CompanionApp *>()->busy()); // No automatic install.
            window.findChild<QAction *>("trayQuit")->trigger();
            QCOMPARE(settings.value("companionApk").toString(), saved == custom ? custom : QString());
        }
        settings.remove("companionApk"); // Fresh install also defaults to the bundle.
        AutoConnectTestWindow window;
        QCOMPARE(window.findChild<QLineEdit *>("companionApk")->text(), bundled);
        window.findChild<QAction *>("trayQuit")->trigger();
        if (!existed) {
            QVERIFY(QFile::remove(bundled));
            QVERIFY(CompanionApp::preferredApk("").isEmpty());
            QCOMPARE(CompanionApp::preferredApk(custom), custom);
        }
    }
    void companionSetup()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("language", "zh_CN");
        settings.setValue("manageServer", false);
        const auto logPath = dir.filePath("adb.jsonl");
        qputenv("OTG_TEST_COMPANION_LOG", logPath.toUtf8());
        qputenv("OTG_TEST_COMPANION_DELAY", "80");
        const auto cleanup = qScopeGuard([] {
            qunsetenv("OTG_TEST_COMPANION_LOG"); qunsetenv("OTG_TEST_COMPANION_DELAY");
        });
        QString apk;
        {
            AutoConnectTestWindow window;
            window.show();
            QTest::qWait(20); // Let startup's no-ADB refresh finish.
            auto *install = window.findChild<QPushButton *>("companionInstall");
            auto *open = window.findChild<QPushButton *>("companionOpen");
            auto *check = window.findChild<QPushButton *>("companionCheck");
            auto *status = window.findChild<QLabel *>("companionStatus");
            auto *serial = window.findChild<QLineEdit *>("usbSerial");
            auto *target = window.findChild<QLineEdit *>("adbSerial");
            auto *adb = window.findChild<QLineEdit *>("adbExecutable");
            auto *sidebar = window.findChild<QListWidget *>("deviceSidebar");
            auto *connectButton = window.findChild<QPushButton *>("connectButton");
            auto *app = window.findChild<CompanionApp *>();
            QVERIFY(install && open && check && status && serial && target && adb && sidebar && app);
            QVERIFY(!install->isEnabled());
            adb->setText(QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                         ".exe"
#endif
                         );
            serial->setText("USB-ONLY");
            target->setText("192.0.2.10:5555");
            QVERIFY(install->isEnabled());
            QVERIFY(!QFile::exists(logPath)); // No installation/query on launch or editing.
            apk = dir.filePath(QString::fromUtf8("手机 app.apk"));
            QFile file(apk); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fake APK"); file.close();
            window.findChild<QLineEdit *>("companionApk")->setText(apk);
            install->click();
            QVERIFY(app->busy());
            QVERIFY(!sidebar->isEnabled());
            QVERIFY(!connectButton->isEnabled());
            QCOMPARE(status->text(), qtTrId("otg.companion.working"));
            QTRY_VERIFY(!app->busy());
            QCOMPARE(status->text(), qtTrId("otg.companion.installed"));
            QVERIFY(sidebar->isEnabled()); QVERIFY(connectButton->isEnabled());
            auto *language = window.findChild<QComboBox *>("languageSelector");
            language->setCurrentIndex(language->findData("en"));
            QCOMPARE(install->text(), "Install / update APK");
            QCOMPARE(status->text(), qtTrId("otg.companion.installed"));
            language->setCurrentIndex(language->findData("zh_CN"));
            check->click(); QTRY_VERIFY(!app->busy());
            QCOMPARE(status->text(), qtTrId("otg.companion.access_required"));
            auto *transport = window.findChild<QComboBox *>("transportSelector");
            if (transport->findData("ble") >= 0) {
                transport->setCurrentIndex(transport->findData("ble"));
                QVERIFY(!install->isVisible());
                transport->setCurrentIndex(transport->findData("usb"));
            }
            target->clear(); // Manual USB serial fallback, never an unqualified adb command.
            open->click(); QTRY_VERIFY(!app->busy());
            QCOMPARE(status->text(), qtTrId("otg.companion.opened"));
            const auto capture = qEnvironmentVariable("OTG_TEST_COMPANION_SCREENSHOT");
            if (!capture.isEmpty()) QVERIFY(window.grab().save(capture));
            check->click();
            QVERIFY(app->busy());
            QSignalSpy exit(&window, &MainWindow::exitReady);
            window.findChild<QAction *>("trayQuit")->trigger();
            QCOMPARE(exit.size(), 0); // Quit waits for the bounded setup command.
            QTRY_COMPARE(exit.size(), 1);
            QFile log(logPath); QVERIFY(log.open(QIODevice::ReadOnly));
            const auto lines = log.readAll().trimmed().split('\n');
            QCOMPARE(lines.size(), 11); // install(2), check(3), open(3), check(3)
            for (int i = 0; i < lines.size(); ++i) {
                const auto args = QJsonDocument::fromJson(lines[i]).array();
                QCOMPARE(args[0].toString(), "-s");
                QCOMPARE(args[1].toString(), i < 5 ? "192.0.2.10:5555" : "USB-ONLY");
            }
        }
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QLineEdit *>("companionApk")->text(), apk);
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void generateClientCertificate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        settings.setValue("language", "zh_CN");
        const auto path = dir.filePath(QString::fromUtf8("客户端/client.pem"));
        {
            AutoConnectTestWindow window;
            auto *tls = window.findChild<QCheckBox *>("tlsEnabled");
            auto *certificate = window.findChild<QLineEdit *>("tlsCertificate");
            auto *fingerprint = window.findChild<QLineEdit *>("tlsFingerprint");
            auto *database = window.findChild<QLineEdit *>("tlsFingerprintFile");
            auto *generate = window.findChild<QPushButton *>("tlsRegenerateCertificate");
            auto *browse = window.findChild<QPushButton *>("tlsBrowseCertificate");
            QVERIFY(tls && certificate && fingerprint && database && generate && browse);
            QVERIFY(certificate->text().isEmpty()); // Client authentication remains optional.
            QVERIFY(!generate->isEnabled());
            QVERIFY(!browse->isEnabled());
            tls->setChecked(true);
            QVERIFY(generate->isEnabled()); // External-server mode is supported.
            QVERIFY(browse->isEnabled());
            QCOMPARE(generate->text(), QString::fromUtf8("生成客户端证书"));
            QVERIFY(!window.findChild<QLineEdit *>("tlsServerCertificate"));
            QVERIFY(!window.findChild<QCheckBox *>("tlsRequireClientCertificate"));
            certificate->setText(path);
            database->setText("trusted-servers");
            QString message;
            auto answerDialog = [&](QMessageBox::StandardButton answer) {
                QTimer::singleShot(0, &window, [&, answer] {
                    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                    QVERIFY(box);
                    message = box->text();
                    box->done(answer);
                });
            };
            answerDialog(QMessageBox::Ok);
            generate->click();
            QVERIFY(QFile::exists(path));
            const auto clientFingerprint = TlsCertificate::fingerprint(path);
            QVERIFY(!clientFingerprint.isEmpty());
            QVERIFY(message.contains(clientFingerprint));
            QVERIFY(fingerprint->text().isEmpty());
            QCOMPARE(database->text(), "trusted-servers");
            QCOMPARE(certificate->text(), path);
            QCOMPARE(settings.value("tlsCertificate").toString(), path);

            QFile pem(path);
            QVERIFY(pem.open(QIODevice::ReadOnly));
            const auto original = pem.readAll();
            pem.close();
            answerDialog(QMessageBox::No);
            generate->click();
            QVERIFY(pem.open(QIODevice::ReadOnly));
            QCOMPARE(pem.readAll(), original); // Declining overwrite preserves the identity.
            pem.close();
            QVERIFY(message.contains(path));

            database->clear();
            fingerprint->setText(QString(64, 'a'));
            certificate->setText(dir.filePath("second.pem"));
            answerDialog(QMessageBox::Ok);
            generate->click();
            QCOMPARE(fingerprint->text(), QString(64, 'a'));
            QVERIFY(database->text().isEmpty());

            certificate->setText(path + "/impossible.pem"); // Parent is a file, save must fail.
            answerDialog(QMessageBox::Ok);
            generate->click();
            QVERIFY(message.contains(qtTrId("otg.tls.generate_failed").section("%1", 0, 0)));
            QCOMPARE(TlsCertificate::fingerprint(path), clientFingerprint);
            QCOMPARE(fingerprint->text(), QString(64, 'a'));
            certificate->setText(path);
            auto *language = window.findChild<QComboBox *>("languageSelector");
            language->setCurrentIndex(language->findData("en"));
            QCOMPARE(generate->text(), "Generate client certificate");
            QCOMPARE(certificate->text(), path);
            language->setCurrentIndex(language->findData("zh_CN"));

            // Exercise connection gating with a fake bridge, never USB hardware.
            window.findChild<QLineEdit *>("usbSerial")->setText("TLS-client");
            auto *manager = window.findChild<ConnectionManager *>();
            window.findChild<QPushButton *>("connectButton")->click();
            QTRY_COMPARE(manager->state("initial"), Session::State::Connected);
            QVERIFY(!generate->isEnabled());
            QVERIFY(!browse->isEnabled());
            manager->stopAll();
            QTRY_VERIFY(!manager->busy());
            QVERIFY(generate->isEnabled());

            const auto capture = qEnvironmentVariable("OTG_TEST_TLS_SCREENSHOT");
            if (!capture.isEmpty()) {
                window.findChild<QTabWidget *>("deviceTabs")->setCurrentIndex(2);
                window.show();
                QTest::qWait(50);
                QVERIFY(window.grab().save(capture));
            }
            window.findChild<QAction *>("trayQuit")->trigger();
        }
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QLineEdit *>("tlsCertificate")->text(), path);
        QCOMPARE(restored.findChild<QLineEdit *>("tlsFingerprint")->text(), QString(64, 'a'));
        QVERIFY(restored.findChild<QPushButton *>("tlsRegenerateCertificate")->isEnabled());
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void displayUpdatesArePerDevice()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings().setValue("adb", "");
        {
            AutoConnectTestWindow window;
            auto *manager = window.findChild<ConnectionManager *>();
            auto *list = window.findChild<QListWidget *>("deviceSidebar");
            auto *width = window.findChild<QSpinBox *>("screenWidth");
            auto *height = window.findChild<QSpinBox *>("screenHeight");
            auto *sensitivity = window.findChild<QDoubleSpinBox *>("sensitivityMultiplier");
            QVERIFY(manager && list && width && height && sensitivity);
            width->setValue(1080); height->setValue(1920); sensitivity->setValue(2.0);
            window.findChild<QPushButton *>("addDeviceButton")->click();
            const auto second = list->currentItem()->data(Qt::UserRole).toString();
            width->setValue(720); height->setValue(1600); sensitivity->setValue(1.5);
            manager->session("initial")->displaySizeChanged(QSize(1920, 1080));
            QCOMPARE(width->value(), 720); QCOMPARE(height->value(), 1600);
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole) == "initial") list->setCurrentRow(i);
            QCOMPARE(width->value(), 1920); QCOMPARE(height->value(), 1080);
            QCOMPARE(sensitivity->value(), 2.0);
            QVERIFY(window.findChild<QLabel *>("virtualScreenSize")->text().startsWith("960 × 540"));
            manager->session(second)->displaySizeChanged(QSize(1600, 720));
            QCOMPARE(width->value(), 1920);
            window.findChild<QAction *>("trayQuit")->trigger();
        }
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1920);
        QCOMPARE(restored.findChild<QSpinBox *>("screenHeight")->value(), 1080);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("sensitivityMultiplier")->value(), 2.0);
        auto *list = restored.findChild<QListWidget *>("deviceSidebar");
        list->setCurrentRow(1);
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1600);
        QCOMPARE(restored.findChild<QSpinBox *>("screenHeight")->value(), 720);
        QCOMPARE(restored.findChild<QDoubleSpinBox *>("sensitivityMultiplier")->value(), 1.5);
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void deviceFeatureProfiles_data()
    {
        QTest::addColumn<QString>("objectName");
        QTest::newRow("clipboard") << QString("clipboardEnabled");
        QTest::newRow("audio") << QString("audioEnabled");
    }
    void deviceFeatureProfiles()
    {
        QFETCH(QString, objectName);
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings().setValue("adb", "");
        {
            AutoConnectTestWindow window;
            auto *feature = window.findChild<QCheckBox *>(objectName);
            auto *list = window.findChild<QListWidget *>("deviceSidebar");
            QVERIFY(feature && list);
            QVERIFY(!feature->isChecked());
            feature->setChecked(true);
            window.findChild<QLineEdit *>("usbSerial")->setText("A");
            window.findChild<QPushButton *>("addDeviceButton")->click();
            QVERIFY(!feature->isChecked());
            window.findChild<QLineEdit *>("usbSerial")->setText("B");
            list->setCurrentRow(0);
            QVERIFY(feature->isChecked());
            list->setCurrentRow(1);
            QVERIFY(!feature->isChecked());
        }
        AutoConnectTestWindow restored;
        auto *list = restored.findChild<QListWidget *>("deviceSidebar");
        list->setCurrentRow(0);
        QVERIFY(restored.findChild<QCheckBox *>(objectName)->isChecked());
        list->setCurrentRow(1);
        QVERIFY(!restored.findChild<QCheckBox *>(objectName)->isChecked());
    }
    void audioPlacementAndAvailability()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings().setValue("adb", "");
        QSettings().setValue("language", "zh_CN");
        AutoConnectTestWindow window;
        window.show();
        auto *audio = window.findChild<QCheckBox *>("audioEnabled");
        auto *clipboard = window.findChild<QCheckBox *>("clipboardEnabled");
        auto *autoRotate = window.findChild<QCheckBox *>("autoRotate");
        auto *tabs = window.findChild<QTabWidget *>("deviceTabs");
        auto *transport = window.findChild<QComboBox *>("transportSelector");
        QVERIFY(audio && clipboard && autoRotate && tabs && transport);
        QVERIFY(autoRotate->isVisible());
        QVERIFY(!autoRotate->isChecked());
        QVERIFY(!autoRotate->isEnabled());
#ifdef OTG_GUI_HAVE_AUDIO
        audio->setChecked(true);
        QVERIFY(autoRotate->isEnabled());
        audio->setChecked(false);
        QVERIFY(!autoRotate->isEnabled());
#endif
#ifdef OTG_GUI_HAVE_CLIPBOARD
        clipboard->setChecked(true);
        QVERIFY(autoRotate->isEnabled());
        clipboard->setChecked(false);
        QVERIFY(!autoRotate->isEnabled());
#endif
        QCOMPARE(audio->text(), "转发音频（需要 ADB）");
        auto *language = window.findChild<QComboBox *>("languageSelector");
        QVERIFY(language);
        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(audio->text(), "Forward audio (requires ADB)");
        language->setCurrentIndex(language->findData("zh_CN"));
        QCOMPARE(audio->text(), "转发音频（需要 ADB）");
        QVERIFY(tabs->widget(0)->isAncestorOf(audio));
        QCOMPARE(audio->parentWidget(), clipboard->parentWidget());
        QVERIFY(!tabs->widget(2)->isAncestorOf(audio));
#ifdef OTG_GUI_HAVE_AUDIO
        QVERIFY(!audio->isHidden());
#else
        QVERIFY(audio->isHidden());
#endif
        const int ble = transport->findData("ble");
        if (ble >= 0) {
            transport->setCurrentIndex(ble);
            QVERIFY(audio->isHidden());
            QVERIFY(!audio->isEnabled());
            transport->setCurrentIndex(0);
#ifdef OTG_GUI_HAVE_AUDIO
            QVERIFY(!audio->isHidden());
            QVERIFY(audio->isEnabled());
#else
            QVERIFY(audio->isHidden());
#endif
        }
        window.findChild<QAction *>("trayQuit")->trigger();
    }
    void multipleDevices()
    {
        QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("deskflow", QCoreApplication::applicationDirPath() + "/test_peer");
        settings.setValue("manageServer", true);
        settings.setValue("language", "zh_CN");
        QTcpServer allocator;
        QVERIFY(allocator.listen(QHostAddress::LocalHost));
        const auto port = allocator.serverPort(); allocator.close();
        settings.setValue("port", port);
        {
            AutoConnectTestWindow window;
            window.show();
            QCoreApplication::processEvents();
            auto *manager = window.findChild<ConnectionManager *>();
            auto *list = window.findChild<QListWidget *>("deviceSidebar");
            auto *tabs = window.findChild<QTabWidget *>("deviceTabs");
            auto *serial = window.findChild<QLineEdit *>("usbSerial");
            auto *alias = window.findChild<QLineEdit *>("deviceAlias");
            auto *width = window.findChild<QSpinBox *>("screenWidth");
            auto *audio = window.findChild<QCheckBox *>("audioEnabled");
            auto *connectButton = window.findChild<QPushButton *>("connectButton");
            QVERIFY(manager && list && tabs && serial && alias && width && audio);
            QCOMPARE(list->count(), 1);
            serial->setText("A"); alias->setText("Redmi K20 Pro"); width->setValue(1234);
            window.findChild<QPushButton *>("addDeviceButton")->click();
            QCOMPARE(list->count(), 2);
            const auto secondId = list->currentItem()->data(Qt::UserRole).toString();
            QVERIFY(secondId != "initial");
            serial->setText("B"); alias->setText("Pixel 8"); width->setValue(1080);
            connectButton->click();
            QTRY_COMPARE(manager->state(secondId), Session::State::Connected);
            QVERIFY(list->isEnabled());
            QVERIFY(!tabs->widget(2)->isEnabled());
            QVERIFY(!audio->isEnabled());
            QVERIFY(!window.findChild<QPushButton *>("addDeviceButton")->isEnabled());
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole) == "initial") list->setCurrentRow(i);
            QCOMPARE(serial->text(), "A");
            QCOMPARE(width->value(), 1234);
            QVERIFY(serial->isEnabled());
            QVERIFY(audio->isEnabled()); // Another device's connection must not lock this option.
            connectButton->click();
            QTRY_COMPARE(manager->state("initial"), Session::State::Connected);
            QVERIFY(!audio->isEnabled());
            QCOMPARE(manager->state(secondId), Session::State::Connected);
            QVERIFY(window.findChild<QLabel *>("connectionSummary")->text().contains("2 / 2"));
            auto *logFilter = window.findChild<QComboBox *>("logFilter");
            auto *log = window.findChild<QPlainTextEdit *>("runtimeLog");
            manager->session("initial")->log("ONLY_A");
            manager->session(secondId)->log("ONLY_B");
            logFilter->setCurrentIndex(logFilter->findData(secondId));
            QVERIFY(log->toPlainText().contains("ONLY_B"));
            QVERIFY(!log->toPlainText().contains("ONLY_A"));
            logFilter->setCurrentIndex(logFilter->findData("*"));
            QVERIFY(log->toPlainText().contains("ONLY_A"));
            const auto screenshot = qEnvironmentVariable("OTG_TEST_MULTI_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                QVERIFY(window.grab().save(screenshot));
                tabs->setCurrentIndex(1);
                QCoreApplication::processEvents();
                QVERIFY(window.grab().save(screenshot + ".layout.png"));
                tabs->setCurrentIndex(0);
            }
            window.findChild<QPushButton *>("disconnectAllButton")->click();
            QTRY_VERIFY(!manager->busy());
            QTRY_COMPARE(manager->serverState(), DeskflowServer::State::Idle);
            // Save with the second record selected: restoring must not merge or lose IDs.
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole) == secondId) list->setCurrentRow(i);
            window.findChild<QPushButton *>("connectAllButton")->click();
            QTRY_COMPARE(manager->state("initial"), Session::State::Connected);
            QTRY_COMPARE(manager->state(secondId), Session::State::Connected);
            window.findChild<QAction *>("trayQuit")->trigger();
            QTRY_VERIFY(manager->settled());
        }
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QListWidget *>("deviceSidebar")->count(), 2);
        QCOMPARE(restored.findChild<QLineEdit *>("usbSerial")->text(), "A");
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1234);
        auto *list = restored.findChild<QListWidget *>("deviceSidebar");
        list->setCurrentRow(1);
        restored.findChild<QPushButton *>("removeDeviceButton")->click();
        QCOMPARE(list->count(), 1);
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void systemThemePalette()
    {
        auto *hints = QGuiApplication::styleHints();
        const auto originalScheme = hints->colorScheme();
        const auto originalPalette = QGuiApplication::palette();
        const auto restore = qScopeGuard([&] {
            QGuiApplication::setPalette(originalPalette);
            hints->setColorScheme(originalScheme);
        });

        hints->setColorScheme(Qt::ColorScheme::Dark);
        if (hints->colorScheme() != Qt::ColorScheme::Dark)
            QSKIP("the offscreen platform does not support overriding the color scheme");
        syncSystemThemePalette(*qobject_cast<QApplication *>(QCoreApplication::instance()));
        QVERIFY(QGuiApplication::palette().color(QPalette::Window).lightness() < 128);

        hints->setColorScheme(Qt::ColorScheme::Light);
        syncSystemThemePalette(*qobject_cast<QApplication *>(QCoreApplication::instance()));
        QVERIFY(QGuiApplication::palette().color(QPalette::Window).lightness() >= 128);
    }
    void multipleAutoConnectWithoutAdb()
    {
        QTemporaryDir dir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        QVariantMap a{{"serial", "A"}, {"phone", "a"}, {"neighbor", "computer"}, {"direction", "right"},
                      {"autoConnect", true}, {"originalWidth", 1234}, {"originalHeight", 2340}};
        auto b = a; b["serial"] = "B"; b["phone"] = "b"; b["neighbor"] = "a"; b["originalWidth"] = 1080;
        settings.beginWriteArray("devices", 2);
        settings.setArrayIndex(0); settings.setValue("id", "phone-a"); settings.setValue("profile", a);
        settings.setArrayIndex(1); settings.setValue("id", "phone-b"); settings.setValue("profile", b);
        settings.endArray();
        settings.beginWriteArray("rememberedDevices", 2);
        settings.setArrayIndex(0); settings.setValue("profile", a);
        settings.setArrayIndex(1); settings.setValue("profile", b);
        settings.endArray();
        const auto path = dir.filePath("usb.json");
        QFile snapshot(path);
        QVERIFY(snapshot.open(QIODevice::WriteOnly));
        const QByteArray devices = "{\"version\":1,\"devices\":[{\"id\":\"1:2\",\"serial\":\"A\"},{\"id\":\"1:3\",\"serial\":\"B\"}]}";
        snapshot.write(devices); snapshot.close();
        qputenv("OTG_TEST_USB_SNAPSHOT", path.toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("OTG_TEST_USB_SNAPSHOT"); });
        AutoConnectTestWindow window;
        auto *manager = window.findChild<ConnectionManager *>();
        auto *monitor = window.findChild<UsbAutoConnect *>();
        QTRY_COMPARE(manager->state("phone-a"), Session::State::Connected);
        QTRY_COMPARE(manager->state("phone-b"), Session::State::Connected);
        QCOMPARE(window.findChild<QLineEdit *>("usbSerial")->text(), "A");
        manager->stop("phone-a");
        QTRY_COMPARE(manager->state("phone-a"), Session::State::Idle);
        QCOMPARE(manager->state("phone-b"), Session::State::Connected);
        QVERIFY(monitor->acceptSnapshot(devices));
        QCOMPARE(manager->state("phone-a"), Session::State::Idle);
        window.findChild<QAction *>("trayQuit")->trigger();
        QTRY_VERIFY(manager->settled());
    }
    void transportAvailability()
    {
        QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings().setValue("adb", "");
        AutoConnectTestWindow window;
        auto *transport = window.findChild<QComboBox *>("transportSelector");
        QVERIFY(transport);
        QCOMPARE(transport->itemData(0).toString(), "usb");
#if (defined(Q_OS_LINUX) && defined(OTG_GUI_HAVE_BLE)) || (defined(Q_OS_WIN) && defined(OTG_GUI_HAVE_BLE_WIN))
        QCOMPARE(transport->count(), 2);
        QCOMPARE(transport->itemData(1).toString(), "ble");
#else
        QCOMPARE(transport->count(), 1);
#endif
        window.findChild<QAction *>("trayQuit")->trigger();
    }
    void bleSettingsAndNoAdb()
    {
#if !defined(OTG_GUI_HAVE_BLE) && !defined(OTG_GUI_HAVE_BLE_WIN)
        QSKIP("BLE-enabled GUI only");
#endif
        QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings settings;
        settings.setValue("adb", QCoreApplication::applicationDirPath() + "/test_peer");
        settings.setValue("transport", "ble");
        settings.setValue("bleAdapter", "hci1");
        settings.setValue("blePeer", "AA:BB:CC:DD:EE:FF");
#ifdef Q_OS_WIN
        settings.setValue("bleIsolate", true); // Stale Linux settings must never leak into Windows args.
        qputenv("OTG_TEST_BLE_WINDOWS", "1");
        const auto restoreEnv = qScopeGuard([] { qunsetenv("OTG_TEST_BLE_WINDOWS"); });
#endif
        settings.setValue("mouseMode", "relative");
        settings.setValue("manageServer", false);
        settings.setValue("language", "zh_CN");
        AutoConnectTestWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto *transport = window.findChild<QComboBox *>("transportSelector");
        QCOMPARE(transport->currentData().toString(), "ble");
        auto *serial = window.findChild<QLineEdit *>("usbSerial");
        QVERIFY(serial->text().isEmpty());
        QVERIFY(!serial->isEnabled());
        auto *mode = window.findChild<QComboBox *>("mouseMode");
        QCOMPARE(mode->currentData().toString(), "absolute");
        QVERIFY(!mode->isEnabled());
        QCOMPARE(window.findChild<QLineEdit *>("bleAdapter")->text(), "hci1");
#ifdef Q_OS_WIN
        QVERIFY(!window.findChild<QLineEdit *>("bleAdapter")->isVisible());
        QVERIFY(!window.findChild<QCheckBox *>("bleIsolate")->isVisible());
        QVERIFY(!window.findChild<QPushButton *>("stopBleService")->isVisible());
        QVERIFY(window.findChild<QLineEdit *>("blePeer")->toolTip().contains("Windows"));
#endif
        auto *session = window.findChild<Session *>();
        window.findChild<QPushButton *>("connectButton")->click();
        QTRY_COMPARE(session->state(), Session::State::Connected);
        QVERIFY(serial->text().isEmpty()); // No startup ADB scan in BLE mode.
        session->stop();
        QTRY_COMPARE(session->state(), Session::State::Idle);
        const auto screenshot = qEnvironmentVariable("OTG_TEST_BLE_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
        window.findChild<QAction *>("trayQuit")->trigger();
        QTRY_VERIFY(!session->hasBleProvider());
        QCOMPARE(settings.value("transport").toString(), "ble");
        QCOMPARE(settings.value("blePeer").toString(), "AA:BB:CC:DD:EE:FF");
        AutoConnectTestWindow restored;
        QCOMPARE(restored.findChild<QLineEdit *>("blePeer")->text(), "AA:BB:CC:DD:EE:FF");
        restored.findChild<QComboBox *>("transportSelector")->setCurrentIndex(0);
        QVERIFY(restored.findChild<QLineEdit *>("usbSerial")->isEnabled());
        restored.findChild<QAction *>("trayQuit")->trigger();
    }
    void clearLogs()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        settings.setValue("language", "zh_CN");
        AutoConnectTestWindow window;
        QCoreApplication::processEvents();
        auto *clear = window.findChild<QPushButton *>("clearLogButton");
        auto *log = window.findChild<QPlainTextEdit *>("runtimeLog");
        auto *session = window.findChild<Session *>();
        auto *language = window.findChild<QComboBox *>("languageSelector");
        QVERIFY(clear && log && session && language);
        QCOMPARE(clear->text(), "清除日志");
        clear->click(); // Clearing an empty log is harmless.
        QVERIFY(log->toPlainText().isEmpty());
        window.findChild<QLineEdit *>("usbSerial")->setText("TEST123");
        window.findChild<QPushButton *>("connectButton")->click();
        QTRY_COMPARE(session->state(), Session::State::Connected);
        session->log("first line\nsecond line");
        QVERIFY(log->toPlainText().contains("first line\nsecond line"));
        QVERIFY(clear->isEnabled());
        clear->click();
        QVERIFY(log->toPlainText().isEmpty());
        QCOMPARE(session->state(), Session::State::Connected);
        session->log("new log after clearing");
        QVERIFY(log->toPlainText().contains("new log after clearing"));
        QVERIFY(!log->toPlainText().contains("first line"));
        const QString text = log->toPlainText();
        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(clear->text(), "Clear logs");
        QCOMPARE(log->toPlainText(), text);
        clear->click();
        QVERIFY(log->toPlainText().isEmpty());
        window.findChild<QAction *>("trayQuit")->trigger();
        QTRY_COMPARE(session->state(), Session::State::Idle);
    }
    void failedConnectionNotRemembered()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("usb-failure-test");
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        AutoConnectTestWindow window;
        QCoreApplication::processEvents();
        window.findChild<QLineEdit *>("usbSerial")->setText("reject");
        auto *session = window.findChild<Session *>();
        window.findChild<QPushButton *>("connectButton")->click();
        QTRY_COMPARE(session->state(), Session::State::Idle);
        QCOMPARE(settings.beginReadArray("rememberedDevices"), 0);
        settings.endArray();
        window.findChild<QAction *>("trayQuit")->trigger();
    }
    void autoConnectWithoutAdb()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("usb-auto-connect-test");
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("manageServer", false);
        settings.setValue("language", "en");
        const QString snapshotFile = settingsDir.filePath("usb.json");
        auto writeSnapshot = [&](const QByteArray &data) {
            QFile file(snapshotFile);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            return file.write(data) == data.size();
        };
        const QByteArray phone = "{\"version\":1,\"devices\":[{\"id\":\"1:2\",\"serial\":\"TEST123\"}]}";
        QVERIFY(writeSnapshot(phone));
        qputenv("OTG_TEST_USB_SNAPSHOT", snapshotFile.toUtf8());
        qputenv("OTG_TEST_RIGHT_ACTION", "back");
        const auto restore = qScopeGuard([] {
            qunsetenv("OTG_TEST_USB_SNAPSHOT"); qunsetenv("OTG_TEST_RIGHT_ACTION");
        });
        {
            AutoConnectTestWindow window;
            auto *enabled = window.findChild<QCheckBox *>("autoConnectEnabled");
            auto *session = window.findChild<Session *>();
            auto *monitor = window.findChild<UsbAutoConnect *>();
            auto *serial = window.findChild<QLineEdit *>("usbSerial");
            auto *adbSerial = window.findChild<QLineEdit *>("adbSerial");
            auto *width = window.findChild<QSpinBox *>("screenWidth");
            auto *multiplier = window.findChild<QDoubleSpinBox *>("sensitivityMultiplier");
            auto *button = window.findChild<QPushButton *>("connectButton");
            QVERIFY(enabled && session && monitor && serial && adbSerial && width && multiplier && button);
            QVERIFY(!enabled->isChecked());
            // First connect manually with no ADB, then remember only on success.
            QCoreApplication::processEvents();
            serial->setText("TEST123");
            adbSerial->setText("192.168.1.20:5555");
            width->setValue(1234);
            multiplier->setValue(2.0);
            window.findChild<QComboBox *>("rightButtonAction")->setCurrentIndex(1);
            enabled->setChecked(true);
            button->click();
            QTRY_COMPARE(session->state(), Session::State::Connected);
            QCOMPARE(settings.beginReadArray("rememberedDevices"), 1);
            settings.setArrayIndex(0);
            QCOMPARE(settings.value("profile").toMap().value("originalWidth").toInt(), 1234);
            QCOMPARE(settings.value("profile").toMap().value("adbSerial").toString(),
                     QStringLiteral("192.168.1.20:5555"));
            QCOMPARE(settings.value("profile").toMap().value("rightButtonAction").toString(), "back");
            settings.endArray();
            QVERIFY(monitor->acceptSnapshot(phone));
            window.findChild<QAction *>("trayDisconnect")->trigger();
            QTRY_COMPARE(session->state(), Session::State::Idle);
            QVERIFY(monitor->acceptSnapshot(phone));
            QCOMPARE(session->state(), Session::State::Idle);
            // Reinsert restores the phone's settings, not unsaved form edits.
            width->setValue(999);
            multiplier->setValue(1.0);
            window.findChild<QComboBox *>("rightButtonAction")->setCurrentIndex(0);
            QVERIFY(writeSnapshot("{\"version\":1,\"devices\":[]}"));
            QVERIFY(monitor->acceptSnapshot("{\"version\":1,\"devices\":[]}"));
            window.show();
            window.close(); // Tray/background auto-connect remains active.
            QVERIFY(!window.isVisible());
            QVERIFY(writeSnapshot(phone));
            monitor->poll();
            QTRY_COMPARE(session->state(), Session::State::Connected);
            QCOMPARE(width->value(), 1234);
            QCOMPARE(multiplier->value(), 2.0);
            QCOMPARE(window.findChild<QComboBox *>("rightButtonAction")->currentData().toString(), "back");
            window.findChild<QAction *>("trayQuit")->trigger();
            QTRY_COMPARE(session->state(), Session::State::Idle);
        }
        // Startup with a remembered phone connects without invoking ADB.
        AutoConnectTestWindow restored;
        auto *session = restored.findChild<Session *>();
        QVERIFY(restored.findChild<QCheckBox *>("autoConnectEnabled")->isChecked());
        QTRY_COMPARE(session->state(), Session::State::Connected);
        QCOMPARE(restored.findChild<QSpinBox *>("screenWidth")->value(), 1234);
        restored.findChild<QAction *>("trayQuit")->trigger();
        QTRY_COMPARE(session->state(), Session::State::Idle);
    }
    void aboutVersion()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings().setValue("adb", "");
        QSettings().setValue("language", "en");
        MainWindow window;
        auto *about = window.findChild<QPushButton *>("aboutButton");
        QVERIFY(about);
        bool inspected = false;
        QString aboutText;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QMessageBox *>();
            if (dialog) {
                aboutText = dialog->text();
                inspected = true;
                dialog->accept();
            }
        });
        about->click();
        // QMessageBox::about() is non-blocking on macOS; let the timer run there too.
        QTRY_VERIFY(inspected);
        QVERIFY(aboutText.contains(OTG_VERSION));
        QVERIFY(aboutText.contains(OTG_COMMIT));
        QVERIFY(aboutText.contains("\nVersion:"));
    }
    void trayLifecycle_data()
    {
        QTest::addColumn<bool>("available");
        QTest::addColumn<bool>("connected");
        QTest::newRow("tray-idle") << true << false;
        QTest::newRow("tray-connected") << true << true;
        QTest::newRow("no-tray-idle") << false << false;
        QTest::newRow("no-tray-connected") << false << true;
    }
    void trayLifecycle()
    {
        QFETCH(bool, available);
        QFETCH(bool, connected);
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("tray-test");
        QSettings settings;
        settings.setValue("adb", "");
        settings.setValue("language", "en");
        TrayTestWindow window;
        window.available = available;
        window.show();
        auto *tray = window.findChild<QSystemTrayIcon *>("systemTray");
        auto *show = window.findChild<QAction *>("trayShow");
        auto *disconnect = window.findChild<QAction *>("trayDisconnect");
        auto *quit = window.findChild<QAction *>("trayQuit");
        auto *session = window.findChild<Session *>();
        QVERIFY(tray && show && disconnect && quit && session);
        QVERIFY(!tray->icon().isNull());
        QVERIFY(!tray->icon().pixmap(32, 32).isNull());
        QCOMPARE(tray->icon().cacheKey(), window.windowIcon().cacheKey());
        QVERIFY(!disconnect->isEnabled());
        QCOMPARE(show->text(), "Show window");
        auto *language = window.findChild<QComboBox *>("languageSelector");
        language->setCurrentIndex(language->findData("zh_CN"));
        QCOMPARE(show->text(), "显示窗口");
        QCOMPARE(quit->text(), "退出");
        language->setCurrentIndex(language->findData("en"));
        if (connected) {
            ConnectionOptions options;
            options.bridge = QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                ".exe"
#endif
                ;
            options.manageServer = false;
            options.serial = "TEST123";
            session->start(options);
            QTRY_COMPARE(session->state(), Session::State::Connected);
            QVERIFY(disconnect->isEnabled());
            QVERIFY(tray->toolTip().contains(session->statusMessage()));
        }
        QSignalSpy exited(&window, &MainWindow::exitReady);
        window.close();
        if (available) {
            QVERIFY(!window.isVisible());
            QCOMPARE(exited.count(), 0);
            QCOMPARE(session->state(), connected ? Session::State::Connected : Session::State::Idle);
            show->trigger();
            QVERIFY(window.isVisible());
            window.showMinimized();
            emit tray->activated(QSystemTrayIcon::Trigger);
            QVERIFY(window.isVisible());
            QVERIFY(!window.isMinimized());
            if (connected) {
                window.close();
                disconnect->trigger();
                QTRY_COMPARE(session->state(), Session::State::Idle);
                QVERIFY(!disconnect->isEnabled());
                QCOMPARE(exited.count(), 0);
                // Quit while connected must wait for graceful bridge shutdown.
                ConnectionOptions options;
                options.bridge = QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
                    ".exe"
#endif
                    ;
                options.manageServer = false;
                options.serial = "TEST123";
                session->start(options);
                QTRY_COMPARE(session->state(), Session::State::Connected);
                quit->trigger();
                QCOMPARE(exited.count(), 0);
            } else {
                quit->trigger();
            }
        }
        QTRY_COMPARE(exited.count(), 1);
        QCOMPARE(session->state(), Session::State::Idle);
        QVERIFY(!window.isVisible());
        QVERIFY(!tray->isVisible());
    }
    void defaultLanguage_data()
    {
        QTest::addColumn<QString>("savedLanguage");
        QTest::newRow("first-launch") << QString();
        QTest::newRow("invalid-setting") << QString("unsupported");
    }
    void defaultLanguage()
    {
        QFETCH(QString, savedLanguage);
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("default-language-test");
        QSettings settings;
        settings.setValue("adb", "");
        if (!savedLanguage.isEmpty())
            settings.setValue("language", savedLanguage);
        MainWindow window;
        QCOMPARE(window.findChild<QComboBox *>("languageSelector")->currentData().toString(),
                 LanguageManager::systemLanguage());
        window.close();
    }
    void languageSwitching()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("otg-test");
        QCoreApplication::setApplicationName("language-test");
        QSettings settings;
        settings.setValue("language", "en");
        settings.setValue("adb", "");
        MainWindow window;
        window.show();
        auto *language = window.findChild<QComboBox *>("languageSelector");
        auto *button = window.findChild<QPushButton *>("connectButton");
        auto *width = window.findChild<QSpinBox *>("screenWidth");
        auto *mode = window.findChild<QComboBox *>("mouseMode");
        QVERIFY(language && button && width && mode);
        QCOMPARE(language->currentData().toString(), "en");
        QCOMPARE(button->text(), "Connect");
        QTRY_VERIFY(window.findChildren<QLabel *>().size() > 0);
        width->setValue(1234);
        mode->setCurrentIndex(mode->findData("relative"));
        language->setCurrentIndex(language->findData("zh_CN"));
        QCOMPARE(button->text(), "连接");
        QCOMPARE(mode->currentText(), "相对坐标（兼容模式）");
        QCOMPARE(width->value(), 1234);
        QCOMPARE(mode->currentData().toString(), "relative");
        QCOMPARE(settings.value("language").toString(), "zh_CN");
        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(button->text(), "Connect");
        QCOMPARE(mode->currentText(), "Relative (compatibility mode)");
        QTRY_VERIFY([&window] {
            for (auto *label : window.findChildren<QLabel *>()) {
                if (label->text().startsWith("ADB not found."))
                    return true;
            }
            return false;
        }());
        QCOMPARE(width->value(), 1234);
        window.close();
        MainWindow restored;
        QCOMPARE(restored.findChild<QComboBox *>("languageSelector")->currentData().toString(), "en");
        QCOMPARE(restored.findChild<QPushButton *>("connectButton")->text(), "Connect");
        restored.close();
    }
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
        settings.setValue("language", "zh_CN");
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
        auto *language = window.findChild<QComboBox *>("languageSelector");
        QVERIFY(language);
        language->setCurrentIndex(language->findData("en"));
        QCOMPARE(connectButton->text(), "Connect");
        QCOMPARE(serial->text(), "TEST123");
        QCOMPARE(width->value(), 1600);
        QTRY_VERIFY([&window] {
            for (auto *label : window.findChildren<QLabel *>()) {
                if (label->isVisible() && !label->wordWrap() && label->width() < label->minimumSizeHint().width())
                    return false;
            }
            return true;
        }());
        const QString englishCapture = qEnvironmentVariable("OTG_TEST_SCREENSHOT_EN");
        if (!englishCapture.isEmpty())
            QVERIFY(window.grab().save(englishCapture));
        language->setCurrentIndex(language->findData("zh_CN"));
        QCOMPARE(connectButton->text(), "连接");
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
