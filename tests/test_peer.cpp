// Mock ADB, Deskflow core and bridge for process lifecycle regression tests.
// SPDX-License-Identifier: Apache-2.0
#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
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
        const auto serial = args.value(args.indexOf("--serial") + 1);
        if (serial == "reject")
            return 5;
        output("{\"event\":\"usb-ready\"}\n");
        if (serial != "slow") {
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
