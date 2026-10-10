// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "device_info.h"
#include "session.h"
#include <QMainWindow>
#include <QPointer>

class QCheckBox;
class QAction;
class QSystemTrayIcon;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
signals:
    void exitReady();
protected:
    virtual bool trayAvailable() const;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void setTranslatedProperty(QObject *object, const char *property, const char *id);
    template<class T> T *translated(T *object, const char *property, const char *id)
    {
        setTranslatedProperty(object, property, id);
        return object;
    }
    void addTranslatedRow(QFormLayout *form, const char *id, QWidget *field);
    void retranslateUi();
    void setupTray();
    void restoreWindow();
    void requestExit();
    void updateTray();
    void refreshDevices();
    void readDevice();
    void connectDevice();
    void saveSettings();
    void updateEnabled();
    QSize virtualScreenSize() const;
    void updateVirtualSize();
    void appendLog(const QString &text);
    QWidget *pathField(QLineEdit *&field, const char *placeholderId);
    struct TextBinding {
        QPointer<QObject> object;
        QByteArray property;
        QByteArray id;
    };
    QList<TextBinding> m_textBindings;
    QComboBox *m_language;
    Session m_session;
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_trayDisconnect = nullptr;
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
    bool m_uiReady = false;
    bool m_sessionStatus = false;
    bool m_closing = false;
    quint64 m_request = 0;
};
