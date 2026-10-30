// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "device_info.h"
#include "session.h"
#include "connection_manager.h"
#include "usb_auto_connect.h"
#include <QMainWindow>
#include <QPointer>
#include <QVariantMap>

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
class QListWidget;
class QTabWidget;
class ScreenLayoutPreview;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
signals:
    void exitReady();
protected:
    virtual bool trayAvailable() const;
    virtual QString bridgeExecutable() const;
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
    QVariantMap deviceProfile() const;
    void applyDeviceProfile(const QVariantMap &profile);
    void rememberDevice();
    void updateEnabled();
    QSize virtualScreenSize() const;
    void updateVirtualSize();
    void addDevice();
    void removeDevice();
    void selectDevice(const QString &id);
    void storeDevice();
    void updateDeviceList();
    void updateLayoutPreview();
    void connectAll();
    ConnectionOptions connectionOptions(const QVariantMap &profile) const;
    QString layoutConfiguration() const;
    void persistDevices();
    void updateAutoConnect();
    void appendLog(const QString &text, const QString &id = {});
    void refreshLog();
    QWidget *pathField(QLineEdit *&field, const char *placeholderId);
    struct TextBinding {
        QPointer<QObject> object;
        QByteArray property;
        QByteArray id;
    };
    QList<TextBinding> m_textBindings;
    QComboBox *m_language;
    ConnectionManager m_connections;
    Session *m_session = nullptr;
    QString m_selectedId = "initial";
    QMap<QString, QVariantMap> m_records;
    QMap<QString, QVariantMap> m_pendingProfiles;
    bool m_loadingDevice = false;
    QListWidget *m_deviceSidebar;
    QTabWidget *m_tabs;
    QWidget *m_deviceBox, *m_sharedBox, *m_layoutBox;
    QLineEdit *m_alias;
    QComboBox *m_neighbor;
    QLabel *m_layoutPreview, *m_summary;
    ScreenLayoutPreview *m_layoutCanvas;
    QPushButton *m_connectAll, *m_disconnectAll, *m_addDevice, *m_removeDevice;
    UsbAutoConnect m_autoConnect;
    QMap<QString, QVariantMap> m_profiles;
    QVariantMap m_pendingProfile;
    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_trayDisconnect = nullptr;
    QWidget *m_form;
    QComboBox *m_devices, *m_direction, *m_mouseMode;
    QComboBox *m_transport;
    QLineEdit *m_adapter, *m_blePeer;
    QCheckBox *m_bleIsolate;
    QPushButton *m_stopBleService;
    bool m_stoppingBleService = false;
    QLineEdit *m_serial, *m_adb, *m_deskflow, *m_host, *m_computer, *m_phone;
    QSpinBox *m_width, *m_height, *m_port;
    QDoubleSpinBox *m_sensitivity;
    QCheckBox *m_manage;
    QCheckBox *m_autoConnectEnabled;
    QPushButton *m_refresh, *m_connect, *m_disconnect;
    QLabel *m_status, *m_deviceStatus;
    QLabel *m_virtualSize;
    QPlainTextEdit *m_log;
    QComboBox *m_logFilter;
    QList<QPair<QString, QString>> m_logHistory;
    QFormLayout *m_deviceForm, *m_programForm;
    QList<AndroidDevice> m_deviceList;
    bool m_scanning = false;
    bool m_uiReady = false;
    bool m_sessionStatus = false;
    bool m_closing = false;
    quint64 m_request = 0;
};
