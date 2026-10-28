// Mock ADB, Deskflow core and bridge for process lifecycle regression tests.
// SPDX-License-Identifier: Apache-2.0
#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#include <cstdio>
#include <thread>

static void output(const char *text)
{
    std::fputs(text, stdout);
    std::fflush(stdout);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (qEnvironmentVariableIsSet("OTG_TEST_BLE_WINDOWS") &&
        (args.contains("--ble-status") || args.contains("--ble-service"))) return 10;
    if (args.contains("--ble-status")) {
        const auto mode = qEnvironmentVariable("OTG_TEST_BLE_STATUS");
        if (mode == "busy")
            output("{\"version\":2,\"available\":true,\"idle\":false,\"ready\":true}\n");
        else if (mode == "incompatible")
            output("{\"version\":1,\"available\":true,\"idle\":true}\n");
        else
            output("{\"version\":2,\"available\":true,\"idle\":true,\"ready\":false}\n");
        return 0;
    }
    if (args.contains("--ble-service")) return 2; // Never start a real daemon in tests.
    if (args.contains("--usb-watch")) {
        const auto mode = qEnvironmentVariable("OTG_TEST_USB_WATCH");
        if (mode == "registration-failure")
            return 1;
        if (mode == "timeout")
            return app.exec();
        if (mode == "malformed") {
            output("invalid JSON\n");
            return app.exec();
        }
        if (mode != "events" && mode != "exit-after-ready")
            return 3; // Default mock backend does not support hotplug.
        output("{\"event\":\"watch-");
        QTimer::singleShot(20, &app, [] { output("ready\",\"version\":1}\n"); });
        if (mode == "exit-after-ready") {
            QTimer::singleShot(100, &app, &QCoreApplication::quit);
        } else {
            // Tests inject notifications through stdin; unlike the real watcher,
            // this mock deliberately supports arbitrary/malformed event lines.
            std::thread([&app] {
                char buffer[4096];
                while (std::fgets(buffer, sizeof(buffer), stdin)) {
                    const QByteArray line(buffer);
                    QMetaObject::invokeMethod(&app, [line] { output(line.constData()); }, Qt::QueuedConnection);
                }
                QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
            }).detach();
        }
        return app.exec();
    }
    if (args.contains("--usb-scan")) {
        const auto logPath = qEnvironmentVariable("OTG_TEST_USB_SCAN_LOG");
        if (!logPath.isEmpty()) {
            QFile log(logPath);
            if (log.open(QIODevice::WriteOnly | QIODevice::Append))
                log.write("scan\n");
        }
        const auto path = qEnvironmentVariable("OTG_TEST_USB_SNAPSHOT");
        if (!path.isEmpty()) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return 7;
            const auto data = file.readAll();
            const int delay = qEnvironmentVariableIntValue("OTG_TEST_USB_SCAN_DELAY");
            if (delay > 0) {
                QTimer::singleShot(delay, &app, [data, &app] {
                    output(data.constData());
                    app.quit();
                });
                return app.exec();
            }
            output(data.constData());
        } else {
            output("{\"version\":1,\"devices\":[{\"id\":\"1:2\",\"serial\":\"TEST123\"}]}\n");
        }
        return 0;
    }
    if (args.contains("devices")) {
        output("List of devices attached\nTEST123 device usb:1-2 model:Pixel_Test transport_id:1\n");
        return 0;
    }
    if (args.contains("get-devpath")) {
        output(qEnvironmentVariableIsSet("OTG_TEST_NO_DEVPATH") ? "unknown\n" : "usb:1-2\n");
        return 0;
    }
    if (args.contains("wm")) {
        output("Physical size: 1080x2400\nOverride size: 720x1600\n");
        return 0;
    }
    if (args.contains("dumpsys")) {
        output("SurfaceOrientation: 1\n");
        return 0;
    }
    if (args.contains("--help")) {
        const auto style = qEnvironmentVariable("OTG_TEST_HELP");
        if (style.startsWith("legacy")) {
            output("deskflow-server test\n--no-daemon --name --config --address\n");
            output(style == "legacy-optin" ? "--enable-crypto\n" :
                   style == "legacy-settings" ? "--settings <file>\n" : "--disable-crypto\n");
        } else {
            output("deskflow-core test\nUsage: coremode [options]\n--settings <file>\n");
        }
        return 0;
    }
    QTcpServer server;
    if (args.value(1) == "server" || args.contains("--no-daemon")) {
        QString layoutPath;
        int port = 0;
        if (args.contains("--settings")) {
            QSettings settings(args.value(args.indexOf("--settings") + 1), QSettings::IniFormat);
            if (settings.value("security/tlsEnabled", true).toBool())
                return 2;
            layoutPath = settings.value("server/externalConfigFile").toString();
            port = settings.value("core/port").toInt();
        } else {
            if (args.contains("--enable-crypto"))
                return 2;
            layoutPath = args.value(args.indexOf("--config") + 1);
            port = args.value(args.indexOf("--address") + 1).section(':', -1).toInt();
        }
        QFile layout(layoutPath);
        if (!layout.open(QIODevice::ReadOnly) || !layout.readAll().contains("section: links"))
            return 3;
        if (!port || !server.listen(QHostAddress::LocalHost, quint16(port)))
            return 4;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
            while (server.hasPendingConnections())
                server.nextPendingConnection()->deleteLater();
        });
    } else if (args.contains("--gui")) {
        const bool ble = args.value(args.indexOf("--transport") + 1) == "ble";
        if (ble && (!args.contains("--ble-peer") || args.contains("--serial") ||
                    args.value(args.indexOf("--mouse-mode") + 1) != "absolute")) return 9;
        if (ble && qEnvironmentVariableIsSet("OTG_TEST_BLE_WINDOWS") &&
            (args.contains("--ble-session") || args.contains("--adapter") ||
             args.contains("--ble-isolate") || args.contains("--background"))) return 10;
        if (args.contains("--gui-ble-control")) {
            auto generation = std::make_shared<int>(0);
            auto connectSession = [&app, generation] {
                const int current = ++*generation;
                if (qEnvironmentVariableIsSet("OTG_TEST_BLE_WAIT")) return;
                QTimer::singleShot(100, &app, [generation, current] {
                    if (*generation == current) output("{\"event\":\"connected\"}\n");
                });
                if (qEnvironmentVariableIsSet("OTG_TEST_BLE_RECONNECT")) {
                    QTimer::singleShot(250, &app, [generation, current] {
                        if (*generation == current) output("{\"event\":\"disconnected\"}\n");
                    });
                    QTimer::singleShot(600, &app, [generation, current] {
                        if (*generation == current) output("{\"event\":\"connected\"}\n");
                    });
                }
            };
            connectSession();
            std::thread([&app, generation, connectSession] {
                char bytes[1600];
                while (std::fgets(bytes, sizeof(bytes), stdin)) {
                    const QByteArray line(bytes);
                    QMetaObject::invokeMethod(&app, [&app, generation, connectSession, line] {
                        if (line == "pause\n") {
                            ++*generation;
                            output("{\"event\":\"ble-idle\"}\n");
                        } else if (line.startsWith("resume\t")) connectSession();
                        else app.quit();
                    }, Qt::QueuedConnection);
                }
                QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
            }).detach();
            return app.exec();
        }
        const auto serial = args.value(args.indexOf("--serial") + 1);
        if (serial == "reject")
            return 5;
        if (!ble) output("{\"event\":\"usb-ready\",\"usb-id\":\"1:2\"}\n");
        if (ble && qEnvironmentVariableIsSet("OTG_TEST_BLE_RECONNECT") &&
            !qEnvironmentVariableIsSet("OTG_TEST_BLE_WAIT")) {
            QTimer::singleShot(250, &app, [] { output("{\"event\":\"disconnected\"}\n"); });
            QTimer::singleShot(600, &app, [] { output("{\"event\":\"connected\"}\n"); });
        }
        if (serial != "slow" && !(ble && qEnvironmentVariableIsSet("OTG_TEST_BLE_WAIT"))) {
            // Split across reads to exercise the line protocol framing.
            output("{\"event\":");
            QTimer::singleShot(100, &app, [] { output("\"connected\"}\n"); });
        }
        std::thread([&app] {
            (void)std::getchar();
            std::fputs("released\n", stderr);
            std::fflush(stderr);
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
        }).detach();
    } else {
        return 6;
    }
    return app.exec();
}
