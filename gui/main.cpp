// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include <QApplication>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("deskflow-otg");
    QCoreApplication::setApplicationName("deskflow-otg-gui");
    MainWindow window;
    QObject::connect(&window, &MainWindow::exitReady, &app, &QCoreApplication::quit);
    window.show();
    return app.exec();
}
