// SPDX-License-Identifier: Apache-2.0
#include "device_info.h"
#include "process_utils.h"
#include "session.h"
#include "i18n.h"
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QScopeGuard>
#include <QXmlStreamReader>
#include <QtTest>

static QString peerPath()
{
    return QCoreApplication::applicationDirPath() + "/test_peer"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ;
}

struct Catalog {
    QString language, error;
    QMap<QString, QString> sources, texts;
};

static Catalog readCatalog(const QString &path)
{
    Catalog catalog;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        catalog.error = file.errorString();
        return catalog;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == QLatin1String("TS"))
            catalog.language = xml.attributes().value("language").toString();
        if (xml.name() != QLatin1String("message"))
            continue;
        const QString id = xml.attributes().value("id").toString();
        if (id.isEmpty() || catalog.texts.contains(id)) {
            xml.raiseError("Missing or duplicate ID: " + id);
            break;
        }
        QString source, translation;
        while (xml.readNextStartElement()) {
            if (xml.name() == QLatin1String("source")) {
                source = xml.readElementText();
            } else if (xml.name() == QLatin1String("translation")) {
                if (!xml.attributes().value("type").isEmpty()) {
                    xml.raiseError("Unfinished or obsolete translation: " + id);
                    break;
                }
                translation = xml.readElementText();
            } else {
                xml.skipCurrentElement();
            }
        }
        if (source.isEmpty() || translation.isEmpty()) {
            xml.raiseError("Empty source or translation: " + id);
            break;
        }
        catalog.sources.insert(id, source);
        catalog.texts.insert(id, translation);
    }
    if (xml.hasError())
        catalog.error = xml.errorString();
    return catalog;
}

static QStringList placeholders(const QString &text)
{
    static const QRegularExpression pattern("%L?(?:[1-9][0-9]*|n)");
    QStringList result;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext())
        result.append(matches.next().captured());
    result.sort();
    return result;
}

class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(LanguageManager::instance().setLanguage("zh_CN"));
    }
    void translations()
    {
        auto &languages = LanguageManager::instance();
        QVERIFY(languages.setLanguage("en"));
        QCOMPARE(qtTrId("otg.action.connect"), "Connect");
        QVERIFY(!languages.setLanguage("unsupported"));
        QCOMPARE(languages.language(), "en");
        Session session;
        ConnectionOptions options;
        session.start(options);
        QCOMPARE(session.state(), Session::State::Idle);
        QCOMPARE(session.statusMessage(), "Invalid device serial number, screen name, host or dimensions.");
        QVERIFY(languages.setLanguage("zh_CN"));
        QCOMPARE(session.statusMessage(), "设备序列号、屏幕名称、主机或尺寸无效。");
        QCOMPARE(qtTrId("otg.action.connect"), "连接");
    }
    void catalogs()
    {
        const auto english = readCatalog(":/i18n-test/gui/translations/deskflow_otg_en.xml");
        const auto chinese = readCatalog(":/i18n-test/gui/translations/deskflow_otg_zh_CN.xml");
        QVERIFY2(english.error.isEmpty(), qPrintable(english.error));
        QVERIFY2(chinese.error.isEmpty(), qPrintable(chinese.error));
        QCOMPARE(english.language, "en");
        QCOMPARE(chinese.language, "zh_CN");
        QVERIFY(!english.texts.isEmpty());
        QCOMPARE(english.texts.keys(), chinese.texts.keys());
        QCOMPARE(english.sources, chinese.sources);
        QCOMPARE(english.sources, english.texts);
        for (const auto &id : english.texts.keys())
            QCOMPARE(placeholders(english.texts[id]), placeholders(chinese.texts[id]));

        // Include indirect ID references in property bindings, states and arrays.
        QSet<QString> references;
        const QRegularExpression pattern("\"(otg\\.[a-z0-9_.]+)\"");
        for (const char *name : {"main_window.cpp", "session.cpp", "process_utils.cpp"}) {
            QFile source(QString(":/i18n-test/gui/") + name + ".txt");
            QVERIFY(source.open(QIODevice::ReadOnly));
            auto matches = pattern.globalMatch(QString::fromUtf8(source.readAll()));
            while (matches.hasNext())
                references.insert(matches.next().captured(1));
        }
        const auto ids = english.texts.keys();
        QCOMPARE(references, QSet<QString>(ids.begin(), ids.end()));

        // Also verify actual -idbased QM compilation and resource loading.
        auto &languages = LanguageManager::instance();
        const auto restore = qScopeGuard([&languages] { languages.setLanguage("zh_CN"); });
        QVERIFY(languages.setLanguage("en"));
        for (auto it = english.texts.cbegin(); it != english.texts.cend(); ++it)
            QCOMPARE(qtTrId(it.key().toUtf8().constData()), it.value());
        QVERIFY(languages.setLanguage("zh_CN"));
        for (auto it = chinese.texts.cbegin(); it != chinese.texts.cend(); ++it)
            QCOMPARE(qtTrId(it.key().toUtf8().constData()), it.value());
    }
    void englishFallback()
    {
        class IncompleteTranslator : public QTranslator {
        public:
            bool isEmpty() const override { return false; }
            QString translate(const char *, const char *id, const char *, int) const override
            {
                return QByteArray(id) == "otg.action.connect" ? QStringLiteral("连接") : QString();
            }
        } partial;
        auto &languages = LanguageManager::instance();
        const auto restore = qScopeGuard([&languages] { languages.setLanguage("zh_CN"); });
        QVERIFY(languages.setLanguage("en"));
        QVERIFY(QCoreApplication::installTranslator(&partial));
        QCOMPARE(qtTrId("otg.action.connect"), "连接");
        QCOMPARE(qtTrId("otg.action.refresh"), "Refresh devices");
        QCoreApplication::removeTranslator(&partial);
    }
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
        const QString detail = session.statusMessage().mid(QString("桥接启动失败：").size());
        QVERIFY(!detail.isEmpty());
        QVERIFY(LanguageManager::instance().setLanguage("en"));
        QCOMPARE(session.statusMessage(), "Failed to start bridge: " + detail);
        QVERIFY(LanguageManager::instance().setLanguage("zh_CN"));
        QCOMPARE(session.statusMessage(), "桥接启动失败：" + detail);
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
        QVERIFY(LanguageManager::instance().setLanguage("en"));
        QCOMPARE(session.statusMessage(), "Connected — move the mouse toward the phone to control it");
        QVERIFY(LanguageManager::instance().setLanguage("zh_CN"));
        QCOMPARE(session.statusMessage(), "已连接 — 将鼠标移到手机一侧即可控制");
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
