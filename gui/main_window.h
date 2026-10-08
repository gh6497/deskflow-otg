// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "device_info.h"
#include "session.h"
#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class MainWindow : public QMainWindow {
public:
    MainWindow();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void refreshDevices();
    void readDevice();
    void connectDevice();
    void saveSettings();
    void updateEnabled();
    QSize virtualScreenSize() const;
    void updateVirtualSize();
    void appendLog(const QString &text);
    QWidget *pathField(QLineEdit *&field, const QString &placeholder);
    Session m_session;
    QWidget *m_form;
    QComboBox *m_devices, *m_direction, *m_mouseMode;
    QLineEdit *m_serial, *m_adb, *m_deskflow, *m_host, *m_computer, *m_phone;
    QSpinBox *m_width, *m_height, *m_port;
    QDoubleSpinBox *m_sensitivity;
    QCheckBox *m_manage;
    QPushButton *m_refresh, *m_connect, *m_disconnect;
    QLabel *m_status, *m_deviceStatus;
    QLabel *m_virtualSize;
    QPlainTextEdit *m_log;
    QList<AndroidDevice> m_deviceList;
    bool m_scanning = false;
    bool m_closing = false;
    quint64 m_request = 0;
};
