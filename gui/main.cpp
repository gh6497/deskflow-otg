// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include "build_version.h"
#include <QApplication>
#include <QStyleHints>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    // Version queries work without a graphical desktop or loading Qt platform plugins.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("deskflow-otg-gui %s (commit %s)\n", OTG_VERSION, OTG_COMMIT);
            return 0;
        }
    }
    QApplication app(argc, argv);
    // Keep the GUI tied to the desktop appearance. Qt updates the default
    // palette and emits colorSchemeChanged when the system theme changes;
    // do not install an application palette here, otherwise that mechanism
    // cannot update widgets that use the platform style.
    app.styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("deskflow-otg");
    QCoreApplication::setApplicationName("deskflow-otg-gui");
    QCoreApplication::setApplicationVersion(OTG_VERSION);
    MainWindow window;
    QObject::connect(&window, &MainWindow::exitReady, &app, &QCoreApplication::quit);
    window.show();
    return app.exec();
}
