// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include <QApplication>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("deskflow-otg");
    QCoreApplication::setApplicationName("deskflow-otg-gui");
    MainWindow window;
    window.show();
    return app.exec();
}
