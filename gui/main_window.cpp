// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include "process_utils.h"
#include "i18n.h"
#include "build_version.h"
#include "screen_layout_preview.h"

#include <QCheckBox>
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <QListWidget>
#include <QTabWidget>
#include <QUuid>
#include <algorithm>

void MainWindow::setTranslatedProperty(QObject *object, const char *property, const char *id)
{
    for (auto &binding : m_textBindings) {
        if (binding.object == object && binding.property == property) {
            binding.id = id;
            object->setProperty(property, qtTrId(id));
            return;
        }
    }
    m_textBindings.append({object, property, id});
    object->setProperty(property, qtTrId(id));
}

void MainWindow::addTranslatedRow(QFormLayout *form, const char *id, QWidget *field)
{
    form->addRow(translated(new QLabel, "text", id), field);
}

void MainWindow::retranslateUi()
{
    for (const auto &binding : m_textBindings) {
        if (binding.object)
            binding.object->setProperty(binding.property.constData(), qtTrId(binding.id.constData()));
    }
    QSignalBlocker devicesBlocker(m_devices);
    m_devices->setItemText(0, qtTrId("otg.device.manual"));
    for (int i = 0; i < m_deviceList.size(); ++i) {
        const auto &device = m_deviceList[i];
        const char *stateId = device.state == "device" ? "otg.device.online" :
                              device.state == "unauthorized" ? "otg.device.unauthorized" :
                              device.state == "offline" ? "otg.device.offline" : "otg.device.access_denied";
        m_devices->setItemText(i + 1, QString("%1 · %2 [%3]").arg(device.model, device.serial, qtTrId(stateId)));
    }
    const char *directions[] = {"otg.position.right", "otg.position.left", "otg.position.up", "otg.position.down"};
    for (int i = 0; i < 4; ++i)
        m_direction->setItemText(i, qtTrId(directions[i]));
    m_mouseMode->setItemText(0, qtTrId("otg.mouse.absolute"));
    m_mouseMode->setItemText(1, qtTrId("otg.mouse.relative"));
    m_transport->setItemText(0, qtTrId("otg.transport.usb"));
    if (m_transport->count() > 1)
#ifdef Q_OS_WIN
        m_transport->setItemText(1, qtTrId("otg.transport.ble_windows"));
#else
        m_transport->setItemText(1, qtTrId("otg.transport.ble"));
#endif
    QSignalBlocker languageBlocker(m_language);
    m_language->setItemText(0, qtTrId("otg.language.chinese"));
    m_language->setItemText(1, qtTrId("otg.language.english"));
    if (m_sessionStatus)
        m_status->setText(m_connections.status(m_selectedId));
    if (m_tabs) {
        m_tabs->setTabText(0, qtTrId("otg.multi.device_tab"));
        m_tabs->setTabText(1, qtTrId("otg.multi.layout_tab"));
        m_tabs->setTabText(2, qtTrId("otg.multi.server_tab"));
        updateDeviceList();
        updateLayoutPreview();
    }
    updateVirtualSize();
    updateTray();
}

bool MainWindow::trayAvailable() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

QString MainWindow::bridgeExecutable() const
{
    return bridgePath();
}

void MainWindow::setupTray()
{
    const QIcon icon(":/icons/deskflow-otg.svg");
    setWindowIcon(icon);
    m_tray = new QSystemTrayIcon(icon, this);
    m_tray->setObjectName("systemTray");
    auto *menu = new QMenu(this);
    auto *restore = translated(menu->addAction(QString()), "text", "otg.tray.show");
    restore->setObjectName("trayShow");
    connect(restore, &QAction::triggered, this, &MainWindow::restoreWindow);
    m_trayDisconnect = translated(menu->addAction(QString()), "text", "otg.action.disconnect");
    m_trayDisconnect->setObjectName("trayDisconnect");
    setTranslatedProperty(m_trayDisconnect, "text", "otg.multi.disconnect_all");
    connect(m_trayDisconnect, &QAction::triggered, &m_connections, &ConnectionManager::stopAll);
    menu->addSeparator();
    auto *quit = translated(menu->addAction(QString()), "text", "otg.tray.quit");
    quit->setObjectName("trayQuit");
    connect(quit, &QAction::triggered, this, &MainWindow::requestExit);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            restoreWindow();
    });
    m_tray->show();
}

void MainWindow::restoreWindow()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

void MainWindow::requestExit()
{
    m_closing = true;
    close();
}

void MainWindow::updateTray()
{
    if (!m_tray)
        return;
    m_tray->setToolTip(QString("Deskflow OTG · %1\n%2").arg(m_summary->text(), m_connections.status(m_selectedId)));
    m_trayDisconnect->setEnabled(m_connections.busy());
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_uiReady)
        retranslateUi();
}

QWidget *MainWindow::pathField(QLineEdit *&field, const char *placeholderId)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    field = new QLineEdit;
    setTranslatedProperty(field, "placeholderText", placeholderId);
    auto *browse = translated(new QPushButton, "text", "otg.action.browse");
    layout->addWidget(field, 1);
    layout->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this, field] {
        const auto path = QFileDialog::getOpenFileName(this, qtTrId("otg.dialog.select_executable"), field->text());
        if (!path.isEmpty())
            field->setText(path);
    });
    return row;
}

MainWindow::MainWindow() : m_connections(this), m_autoConnect(this)
{
    QSettings settings;
    if (settings.beginReadArray("devices") > 0) {
        settings.setArrayIndex(0);
        m_selectedId = settings.value("id", "initial").toString();
        if (m_selectedId.isEmpty()) m_selectedId = "initial";
    }
    settings.endArray();
    m_session = m_connections.addDevice(m_selectedId);
    m_autoConnect.setMultipleDevices(true);
    auto &languages = LanguageManager::instance();
    const QString savedLanguage = settings.value("language", LanguageManager::systemLanguage()).toString();
    if (!languages.setLanguage(savedLanguage))
        languages.setLanguage(LanguageManager::systemLanguage());
    setTranslatedProperty(this, "windowTitle", "otg.window.title");
    resize(1000, 820);
    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(20, 16, 20, 16);
    auto *heading = new QLabel("Deskflow OTG");
    QFont titleFont = heading->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    heading->setFont(titleFont);
    auto *headingRow = new QHBoxLayout;
    headingRow->addWidget(heading, 1);
    headingRow->addWidget(translated(new QLabel, "text", "otg.label.language"));
    m_language = new QComboBox;
    m_language->setObjectName("languageSelector");
    m_language->addItem(qtTrId("otg.language.chinese"), "zh_CN");
    m_language->addItem(qtTrId("otg.language.english"), "en");
    m_language->setCurrentIndex(m_language->findData(languages.language()));
    headingRow->addWidget(m_language);
    auto *about = translated(new QPushButton, "text", "otg.action.about");
    about->setObjectName("aboutButton");
    connect(about, &QPushButton::clicked, this, [this] {
        QMessageBox::about(this, qtTrId("otg.about.title"),
            qtTrId("otg.about.text").arg(OTG_VERSION, OTG_COMMIT));
    });
    headingRow->addWidget(about);
    root->addLayout(headingRow);
    auto *intro = translated(new QLabel, "text", "otg.window.intro");
    root->addWidget(intro);

    m_summary = new QLabel;
    m_summary->setObjectName("connectionSummary");
    root->addWidget(m_summary);
    auto *splitter = new QSplitter;
    auto *sidebar = new QWidget;
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 8, 0);
    m_deviceSidebar = new QListWidget;
    m_deviceSidebar->setObjectName("deviceSidebar");
    m_deviceSidebar->setMinimumWidth(180);
    sidebarLayout->addWidget(m_deviceSidebar, 1);
    m_addDevice = translated(new QPushButton, "text", "otg.multi.add_device");
    m_addDevice->setObjectName("addDeviceButton");
    sidebarLayout->addWidget(m_addDevice);
    m_removeDevice = translated(new QPushButton, "text", "otg.multi.remove_device");
    m_removeDevice->setObjectName("removeDeviceButton");
    sidebarLayout->addWidget(m_removeDevice);
    m_connectAll = translated(new QPushButton, "text", "otg.multi.connect_all");
    m_connectAll->setObjectName("connectAllButton");
    m_disconnectAll = translated(new QPushButton, "text", "otg.multi.disconnect_all");
    m_disconnectAll->setObjectName("disconnectAllButton");
    sidebarLayout->addWidget(m_connectAll);
    sidebarLayout->addWidget(m_disconnectAll);
    splitter->addWidget(sidebar);
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget;
    m_tabs->setObjectName("deviceTabs");
    rightLayout->addWidget(m_tabs, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 3);

    m_form = new QWidget;
    auto *formLayout = new QVBoxLayout(m_form);
    formLayout->setContentsMargins(0, 0, 0, 0);
    auto *deviceBox = translated(new QGroupBox, "title", "otg.section.device");
    m_deviceBox = deviceBox;
    auto *deviceForm = new QFormLayout(deviceBox);
    m_deviceForm = deviceForm;
    m_alias = new QLineEdit;
    m_alias->setObjectName("deviceAlias");
    addTranslatedRow(deviceForm, "otg.multi.alias", m_alias);
    m_transport = new QComboBox;
    m_transport->setObjectName("transportSelector");
    m_transport->addItem(qtTrId("otg.transport.usb"), "usb");
#if defined(Q_OS_LINUX) && defined(OTG_GUI_HAVE_BLE)
    m_transport->addItem(qtTrId("otg.transport.ble"), "ble");
#elif defined(Q_OS_WIN) && defined(OTG_GUI_HAVE_BLE_WIN)
    m_transport->addItem(qtTrId("otg.transport.ble_windows"), "ble");
#endif
    addTranslatedRow(deviceForm, "otg.label.transport", m_transport);
    m_adapter = new QLineEdit;
    m_adapter->setObjectName("bleAdapter");
    addTranslatedRow(deviceForm, "otg.ble.adapter", m_adapter);
    m_blePeer = new QLineEdit;
    m_blePeer->setObjectName("blePeer");
    m_blePeer->setPlaceholderText("AA:BB:CC:DD:EE:FF");
    setTranslatedProperty(m_blePeer, "toolTip", "otg.ble.peer_hint");
#ifdef Q_OS_WIN
    setTranslatedProperty(m_blePeer, "toolTip", "otg.ble.windows_peer_hint");
#endif
    addTranslatedRow(deviceForm, "otg.ble.peer", m_blePeer);
    m_bleIsolate = translated(new QCheckBox, "text", "otg.ble.isolate");
    m_bleIsolate->setObjectName("bleIsolate");
    deviceForm->addRow(m_bleIsolate);
    m_stopBleService = translated(new QPushButton, "text", "otg.ble.stop_service");
    m_stopBleService->setObjectName("stopBleService");
    deviceForm->addRow(m_stopBleService);
    connect(m_stopBleService, &QPushButton::clicked, this, [this] {
        if (QMessageBox::warning(this, qtTrId("otg.ble.stop_service"), qtTrId("otg.ble.stop_confirm"),
                                 QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok)
            return;
#ifdef Q_OS_WIN
        m_session->stopBleProvider();
        return;
#endif
        m_stoppingBleService = true;
        updateEnabled();
        runCommand(this, bridgeExecutable(), {"--ble-stop-service", "--adapter", m_adapter->text().trimmed()},
                   [this](bool ok, const QString &output) {
            appendLog(qtTrId(ok ? "otg.ble.stopped" : "otg.ble.stop_failed") + output);
            m_stoppingBleService = false;
            updateEnabled();
        });
    });
    auto *deviceRow = new QWidget;
    auto *deviceLayout = new QHBoxLayout(deviceRow);
    deviceLayout->setContentsMargins(0, 0, 0, 0);
    m_devices = new QComboBox;
    m_devices->addItem(qtTrId("otg.device.manual"), -1);
    m_refresh = translated(new QPushButton, "text", "otg.action.refresh");
    deviceLayout->addWidget(m_devices, 1);
    deviceLayout->addWidget(m_refresh);
    addTranslatedRow(deviceForm, "otg.label.device", deviceRow);
    m_serial = new QLineEdit;
    m_serial->setObjectName("usbSerial");
    addTranslatedRow(deviceForm, "otg.label.usb_serial", m_serial);
    m_deviceStatus = translated(new QLabel, "text", "otg.device.debugging_required");
    m_deviceStatus->setWordWrap(true);
    deviceForm->addRow(m_deviceStatus);
    m_autoConnectEnabled = translated(new QCheckBox, "text", "otg.auto_connect.enabled");
    m_autoConnectEnabled->setObjectName("autoConnectEnabled");
    setTranslatedProperty(m_autoConnectEnabled, "toolTip", "otg.auto_connect.tooltip");
    deviceForm->addRow(m_autoConnectEnabled);
    auto *sizeRow = new QWidget;
    auto *sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    m_width = new QSpinBox;
    m_height = new QSpinBox;
    m_width->setObjectName("screenWidth");
    m_height->setObjectName("screenHeight");
    m_width->setRange(1, 32767);
    m_height->setRange(1, 32767);
    sizeLayout->addWidget(m_width);
    sizeLayout->addWidget(new QLabel("×"));
    sizeLayout->addWidget(m_height);
    auto *swap = translated(new QPushButton, "text", "otg.action.swap_dimensions");
    swap->setObjectName("swapDimensions");
    sizeLayout->addWidget(swap);
    connect(swap, &QPushButton::clicked, this, [this] {
        const int width = m_width->value();
        m_width->setValue(m_height->value());
        m_height->setValue(width);
    });
    addTranslatedRow(deviceForm, "otg.label.original_size", sizeRow);
    m_sensitivity = new QDoubleSpinBox;
    m_sensitivity->setObjectName("sensitivityMultiplier");
    m_sensitivity->setRange(1.0, 10.0);
    m_sensitivity->setDecimals(2);
    m_sensitivity->setSingleStep(0.25);
    m_sensitivity->setSuffix(" ×");
    setTranslatedProperty(m_sensitivity, "toolTip", "otg.sensitivity.tooltip");
    addTranslatedRow(deviceForm, "otg.label.sensitivity", m_sensitivity);
    m_virtualSize = new QLabel;
    m_virtualSize->setObjectName("virtualScreenSize");
    m_virtualSize->setWordWrap(true);
    addTranslatedRow(deviceForm, "otg.label.virtual_size", m_virtualSize);
    formLayout->addWidget(deviceBox);

    auto *serverBox = translated(new QGroupBox, "title", "otg.section.connection");
    auto *serverForm = new QFormLayout(serverBox);
    m_manage = translated(new QCheckBox, "text", "otg.server.manage");
    serverForm->addRow(m_manage);
    m_direction = new QComboBox;
    m_direction->addItem(qtTrId("otg.position.right"), "right");
    m_direction->addItem(qtTrId("otg.position.left"), "left");
    m_direction->addItem(qtTrId("otg.position.up"), "up");
    m_direction->addItem(qtTrId("otg.position.down"), "down");
    m_layoutBox = new QWidget;
    auto *layoutForm = new QFormLayout(m_layoutBox);
    m_layoutCanvas = new ScreenLayoutPreview;
    m_layoutCanvas->setObjectName("screenLayoutCanvas");
    layoutForm->addRow(m_layoutCanvas);
    m_neighbor = new QComboBox;
    m_neighbor->setObjectName("screenNeighbor");
    addTranslatedRow(layoutForm, "otg.multi.neighbor", m_neighbor);
    addTranslatedRow(layoutForm, "otg.label.position", m_direction);
    m_layoutPreview = new QLabel;
    m_layoutPreview->setObjectName("layoutPreview");
    m_layoutPreview->setWordWrap(true);
    m_layoutPreview->setTextFormat(Qt::PlainText);
    layoutForm->addRow(m_layoutPreview);
    auto *layoutHint = translated(new QLabel, "text", "otg.multi.layout_hint");
    layoutHint->setWordWrap(true);
    layoutForm->addRow(layoutHint);
    m_computer = new QLineEdit;
    m_phone = new QLineEdit;
    m_host = new QLineEdit;
    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    addTranslatedRow(serverForm, "otg.label.computer_name", m_computer);
    m_phone->setObjectName("screenName");
    addTranslatedRow(deviceForm, "otg.label.phone_name", m_phone);
    addTranslatedRow(serverForm, "otg.label.server_address", m_host);
    addTranslatedRow(serverForm, "otg.label.server_port", m_port);
    m_sharedBox = new QWidget;
    auto *sharedLayout = new QVBoxLayout(m_sharedBox);
    sharedLayout->addWidget(serverBox);

    auto *advancedBox = translated(new QGroupBox, "title", "otg.section.programs");
    auto *advancedForm = new QFormLayout(advancedBox);
    m_programForm = advancedForm;
    advancedForm->addRow("ADB", pathField(m_adb, "otg.path.adb_hint"));
    advancedForm->addRow("Deskflow", pathField(m_deskflow, "otg.path.deskflow_hint"));
    m_mouseMode = new QComboBox;
    m_mouseMode->setObjectName("mouseMode");
    m_mouseMode->addItem(qtTrId("otg.mouse.absolute"), "absolute");
    m_mouseMode->addItem(qtTrId("otg.mouse.relative"), "relative");
    addTranslatedRow(deviceForm, "otg.label.mouse_mode", m_mouseMode);
    sharedLayout->addWidget(advancedBox);
    sharedLayout->addStretch();
    formLayout->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_form);
    m_tabs->addTab(scroll, QString());
    m_tabs->addTab(m_layoutBox, QString());
    m_tabs->addTab(m_sharedBox, QString());

    auto *buttons = new QHBoxLayout;
    m_status = translated(new QLabel, "text", "otg.status.not_connected");
    m_status->setWordWrap(true);
    m_connect = translated(new QPushButton, "text", "otg.action.connect");
    m_connect->setObjectName("connectButton");
    m_connect->setDefault(true);
    m_disconnect = translated(new QPushButton, "text", "otg.action.disconnect");
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_connect);
    buttons->addWidget(m_disconnect);
    rightLayout->addLayout(buttons);
    auto *logHeader = new QHBoxLayout;
    logHeader->addWidget(translated(new QLabel, "text", "otg.log.title"), 1);
    m_logFilter = new QComboBox;
    m_logFilter->setObjectName("logFilter");
    logHeader->addWidget(m_logFilter);
    auto *clearLog = translated(new QPushButton, "text", "otg.action.clear_log");
    clearLog->setObjectName("clearLogButton");
    logHeader->addWidget(clearLog);
    root->addLayout(logHeader);
    m_log = new QPlainTextEdit;
    m_log->setObjectName("runtimeLog");
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1200);
    m_log->setMinimumHeight(100);
    setTranslatedProperty(m_log, "placeholderText", "otg.log.placeholder");
    root->addWidget(m_log, 1);
    connect(clearLog, &QPushButton::clicked, this, [this] { m_logHistory.clear(); m_log->clear(); });
    connect(m_logFilter, &QComboBox::currentIndexChanged, this, &MainWindow::refreshLog);
    setCentralWidget(central);

    m_adb->setText(settings.value("adb", findAdb()).toString());
    m_deskflow->setText(settings.value("deskflow", findDeskflow()).toString());
    m_serial->setText(settings.value("serial").toString());
    m_transport->setCurrentIndex(qMax(0, m_transport->findData(settings.value("transport", "usb"))));
    m_adapter->setText(settings.value("bleAdapter", "hci0").toString());
    m_blePeer->setText(settings.value("blePeer").toString());
    m_bleIsolate->setChecked(settings.value("bleIsolate", false).toBool());
    m_width->setValue(settings.value("originalWidth", settings.value("width", 1080)).toInt());
    m_height->setValue(settings.value("originalHeight", settings.value("height", 1920)).toInt());
    m_sensitivity->setValue(settings.value("sensitivityMultiplier", 1.0).toDouble());
    m_computer->setText(settings.value("computer", "computer").toString());
    m_phone->setText(settings.value("phone", "android").toString());
    m_host->setText(settings.value("host", "127.0.0.1").toString());
    m_port->setValue(settings.value("port", 24800).toInt());
    m_manage->setChecked(settings.value("manageServer", true).toBool());
    m_direction->setCurrentIndex(qMax(0, m_direction->findData(settings.value("direction", "right"))));
    m_mouseMode->setCurrentIndex(qMax(0, m_mouseMode->findData(settings.value("mouseMode", "absolute"))));
    if (m_transport->currentData() == "ble") m_mouseMode->setCurrentIndex(0);
    const int profileCount = settings.beginReadArray("rememberedDevices");
    for (int i = 0; i < profileCount; ++i) {
        settings.setArrayIndex(i);
        const auto profile = settings.value("profile").toMap();
        const auto serial = profile.value("serial").toString();
        if (!serial.isEmpty())
            m_profiles.insert(serial, profile);
    }
    settings.endArray();
    m_autoConnect.setKnownSerials(m_profiles.keys());
    m_autoConnectEnabled->setChecked(settings.value("autoConnect", false).toBool());
    m_alias->setText(m_phone->text());
    m_neighbor->addItem(m_computer->text(), m_computer->text());
    m_records.insert(m_selectedId, deviceProfile());
    const int deviceCount = settings.beginReadArray("devices");
    for (int i = 0; i < deviceCount; ++i) {
        settings.setArrayIndex(i);
        const auto id = settings.value("id").toString();
        const auto profile = settings.value("profile").toMap();
        if (!id.isEmpty() && !profile.isEmpty()) {
            if (i == 0) {
                // Reuse the initial Session so existing single-device settings migrate naturally.
                m_records[m_selectedId] = profile;
            } else {
                m_records[id] = profile;
                m_connections.addDevice(id);
            }
        }
    }
    settings.endArray();
    if (deviceCount > 0) applyDeviceProfile(m_records.value(m_selectedId));
    if (m_transport->currentData() == "ble")
#ifdef Q_OS_WIN
        setTranslatedProperty(m_deviceStatus, "text", "otg.ble.windows_setup_hint");
#else
        setTranslatedProperty(m_deviceStatus, "text", "otg.ble.setup_hint");
#endif
    connect(m_autoConnectEnabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (m_loadingDevice) return;
        QSettings().setValue("autoConnect", enabled);
        if (!m_loadingDevice) {
            storeDevice();
            persistDevices();
        }
        updateAutoConnect();
    });
    connect(&m_autoConnect, &UsbAutoConnect::connectRequested, this, [this](const QString &serial) {
        if (m_closing || !m_profiles.contains(serial))
            return;
        storeDevice();
        for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
            if (it->value("transport", "usb") != "usb" || it->value("serial").toString() != serial ||
                !it->value("autoConnect", false).toBool() || m_connections.state(it.key()) != Session::State::Idle) continue;
            auto profile = m_profiles.value(serial);
            // Global server fields come from the shared form, not an old remembered profile.
            profile["phone"] = it->value("phone");
            profile["neighbor"] = it->value("neighbor");
            profile["direction"] = it->value("direction", "right");
            profile["alias"] = it->value("alias");
            profile["autoConnect"] = true;
            profile["transport"] = "usb";
            m_records[it.key()] = profile; // Show the exact parameters used, even for a background device.
            m_pendingProfiles[it.key()] = profile;
            appendLog(qtTrId("otg.auto_connect.connecting").arg(serial));
            m_connections.start(it.key(), connectionOptions(profile), layoutConfiguration());
            if (it.key() == m_selectedId) applyDeviceProfile(profile);
            break;
        }
    });
    connect(&m_autoConnect, &UsbAutoConnect::scanFailed, this, [this] {
        appendLog(qtTrId("otg.auto_connect.scan_failed"));
    });
    connect(&m_autoConnect, &UsbAutoConnect::detectionModeChanged, this, [this](bool hotplug) {
        appendLog(hotplug ? qtTrId("otg.auto_connect.hotplug") : qtTrId("otg.auto_connect.polling"));
    });
    connect(m_width, &QSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_height, &QSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_sensitivity, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_mouseMode, &QComboBox::currentIndexChanged, this, &MainWindow::updateVirtualSize);
    updateVirtualSize();
    connect(m_manage, &QCheckBox::toggled, this, &MainWindow::updateEnabled);
    connect(m_refresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(m_devices, &QComboBox::currentIndexChanged, this, &MainWindow::readDevice);
    connect(m_serial, &QLineEdit::textChanged, this, &MainWindow::updateEnabled);
    connect(m_blePeer, &QLineEdit::textChanged, this, &MainWindow::updateEnabled);
    connect(m_transport, &QComboBox::currentIndexChanged, this, [this] {
        if (m_transport->currentData() == "ble") m_mouseMode->setCurrentIndex(0);
        setTranslatedProperty(m_deviceStatus, "text", m_transport->currentData() == "ble" ?
#ifdef Q_OS_WIN
                              "otg.ble.windows_setup_hint" : "otg.device.manual_hint");
#else
                              "otg.ble.setup_hint" : "otg.device.manual_hint");
#endif
        updateEnabled();
    });
    connect(m_connect, &QPushButton::clicked, this, &MainWindow::connectDevice);
    connect(m_disconnect, &QPushButton::clicked, this, [this] { m_connections.stop(m_selectedId); });
    connect(&m_connections, &ConnectionManager::log, this, [this](const QString &id, const QString &message) {
        const auto name = m_records.value(id).value("phone", id).toString();
        appendLog(id.isEmpty() ? message : "[" + name + "] " + message, id);
    });
    connect(&m_connections, &ConnectionManager::usbAttached, &m_autoConnect, &UsbAutoConnect::suppressAttachment);
    connect(&m_connections, &ConnectionManager::changed, this, [this](const QString &id) {
        m_sessionStatus = true;
        m_status->setText(m_connections.status(m_selectedId));
        if (!id.isEmpty() && m_connections.state(id) == Session::State::Connected && m_pendingProfiles.contains(id)) {
            const auto profile = m_pendingProfiles.take(id);
            if (profile.value("transport", "usb") == "usb") {
                m_pendingProfile = profile;
                rememberDevice();
            }
        }
        updateEnabled();
        updateDeviceList();
        if (m_closing && m_connections.settled())
            QTimer::singleShot(0, this, &QWidget::close);
    });
    connect(m_addDevice, &QPushButton::clicked, this, &MainWindow::addDevice);
    connect(m_removeDevice, &QPushButton::clicked, this, &MainWindow::removeDevice);
    connect(m_deviceSidebar, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current) {
        if (current) selectDevice(current->data(Qt::UserRole).toString());
    });
    connect(m_connectAll, &QPushButton::clicked, this, &MainWindow::connectAll);
    connect(m_disconnectAll, &QPushButton::clicked, &m_connections, &ConnectionManager::stopAll);
    connect(m_neighbor, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_loadingDevice) { storeDevice(); updateLayoutPreview(); }
    });
    connect(m_direction, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_loadingDevice) { storeDevice(); updateLayoutPreview(); }
    });
    connect(m_phone, &QLineEdit::editingFinished, this, [this] { storeDevice(); updateDeviceList(); updateLayoutPreview(); });
    connect(m_computer, &QLineEdit::editingFinished, this, &MainWindow::updateLayoutPreview);
    connect(m_language, &QComboBox::currentIndexChanged, this, [this] {
        const QString language = m_language->currentData().toString();
        if (LanguageManager::instance().setLanguage(language)) {
            QSettings().setValue("language", language);
            retranslateUi();
        }
    });
    m_uiReady = true;
    setupTray();
    retranslateUi();
    updateEnabled();
    QTimer::singleShot(0, this, [this] {
        m_autoConnect.setProgram(bridgeExecutable());
        updateAutoConnect();
        // A remembered phone uses its saved dimensions, not ADB. In particular,
        // do not start ADB or prompt for USB debugging during auto-connect.
        if (m_transport->currentData() == "usb" && (!m_autoConnectEnabled->isChecked() || m_profiles.isEmpty()))
            refreshDevices();
    });
}

void MainWindow::appendLog(const QString &text, const QString &id)
{
    if (text.isEmpty()) return;
    const auto line = QTime::currentTime().toString("HH:mm:ss ") + text;
    m_logHistory.append({id, line});
    if (m_logHistory.size() > 1200) m_logHistory.removeFirst();
    const auto filter = m_logFilter->currentData().toString();
    if (filter == "*" || filter == id) m_log->appendPlainText(line);
}

void MainWindow::refreshLog()
{
    const auto filter = m_logFilter->currentData().toString();
    QStringList lines;
    for (const auto &entry : m_logHistory)
        if (filter == "*" || filter == entry.first) lines.append(entry.second);
    m_log->setPlainText(lines.join('\n'));
}

void MainWindow::storeDevice()
{
    if (m_loadingDevice || !m_uiReady) return;
    const auto oldName = m_records.value(m_selectedId).value("phone").toString();
    m_records[m_selectedId] = deviceProfile();
    const auto name = m_phone->text().trimmed();
    if (!oldName.isEmpty() && oldName != name) {
        for (auto &record : m_records)
            if (record.value("neighbor").toString() == oldName) record["neighbor"] = name;
    }
}

void MainWindow::persistDevices()
{
    QSettings settings;
    settings.beginWriteArray("devices", m_records.size());
    // Keep the stable initial ID first; selection must never change record identity.
    QStringList ids = m_records.keys();
    if (ids.removeAll("initial")) ids.prepend("initial");
    int index = 0;
    for (const auto &id : ids) {
        settings.setArrayIndex(index++);
        settings.setValue("id", id);
        settings.setValue("profile", m_records.value(id));
    }
    settings.endArray();
}

void MainWindow::updateAutoConnect()
{
    QStringList known;
    QMap<QString, int> counts;
    for (const auto &record : m_records)
        if (record.value("transport", "usb") == "usb") ++counts[record.value("serial").toString()];
    for (const auto &record : m_records) {
        const auto serial = record.value("serial").toString();
        if (record.value("transport", "usb") == "usb" && counts.value(serial) == 1 &&
            record.value("autoConnect", false).toBool() && m_profiles.contains(serial))
            known.append(serial);
    }
    m_autoConnect.setKnownSerials(known);
    m_autoConnect.setEnabled(!known.isEmpty() && !m_closing);
}

void MainWindow::addDevice()
{
    if (m_connections.busy() || m_closing) return;
    storeDevice();
    auto profile = deviceProfile();
    QString name;
    int number = m_records.size() + 1;
    do {
        name = "android-" + QString::number(number++);
    } while (std::any_of(m_records.cbegin(), m_records.cend(), [&](const QVariantMap &record) {
        return record.value("phone").toString().compare(name, Qt::CaseInsensitive) == 0;
    }));
    profile["phone"] = name;
    profile["alias"] = name;
    profile["serial"] = "";
    profile["transport"] = "usb";
    profile["blePeer"] = "";
    profile["bleIsolate"] = false;
    profile["autoConnect"] = false;
    profile["neighbor"] = m_phone->text().trimmed();
    profile["direction"] = "right";
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_records[id] = profile;
    m_connections.addDevice(id);
    selectDevice(id);
    persistDevices();
}

void MainWindow::removeDevice()
{
    if (m_closing || m_connections.busy() || m_records.size() <= 1) return;
    storeDevice();
    const auto name = m_records.value(m_selectedId).value("phone").toString();
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        if (it.key() != m_selectedId && it->value("neighbor").toString() == name) {
            QMessageBox::warning(this, qtTrId("otg.multi.remove_device"), qtTrId("otg.multi.remove_dependency"));
            return;
        }
    }
    if (!m_connections.removeDevice(m_selectedId)) return;
    m_records.remove(m_selectedId);
    m_pendingProfiles.remove(m_selectedId);
    m_selectedId.clear();
    m_loadingDevice = true;
    selectDevice(m_records.firstKey());
    m_loadingDevice = false;
    persistDevices();
    updateAutoConnect();
}

void MainWindow::selectDevice(const QString &id)
{
    if (id == m_selectedId || !m_records.contains(id) || m_closing) return;
    storeDevice();
    m_selectedId = id;
    m_session = m_connections.session(id);
    applyDeviceProfile(m_records.value(id));
    m_sessionStatus = true;
    m_status->setText(m_connections.status(id));
    updateDeviceList();
    updateLayoutPreview();
    updateEnabled();
}

void MainWindow::updateDeviceList()
{
    QSignalBlocker blocker(m_deviceSidebar);
    m_deviceSidebar->clear();
    int connected = 0;
    const char *states[] = {"otg.status.not_connected", "otg.status.connecting", "otg.multi.connected", "otg.status.disconnecting"};
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        const auto state = m_connections.state(it.key());
        connected += state == Session::State::Connected;
        const auto alias = it->value("alias").toString();
        const auto name = it->value("phone").toString();
        const auto transport = it->value("transport", "usb").toString().toUpper();
        auto *item = new QListWidgetItem(QString("%1\n%2 · %3\n%4")
            .arg(alias.isEmpty() ? name : alias, transport, name,
                 qtTrId(m_connections.hasFailure(it.key()) ? "otg.multi.failed" : states[int(state)])), m_deviceSidebar);
        item->setData(Qt::UserRole, it.key());
        item->setToolTip(m_connections.status(it.key()));
        if (it.key() == "initial") {
            m_deviceSidebar->takeItem(m_deviceSidebar->row(item));
            m_deviceSidebar->insertItem(0, item);
        }
        if (it.key() == m_selectedId) m_deviceSidebar->setCurrentItem(item);
    }
    m_summary->setText(qtTrId("otg.multi.summary").arg(connected).arg(m_records.size()).arg(
        m_manage->isChecked() ? qtTrId(m_connections.serverState() == DeskflowServer::State::Running ?
                                    "otg.multi.server_running" : "otg.multi.server_stopped") : qtTrId("otg.multi.external_server")));
    const auto filter = m_logFilter->currentData();
    QSignalBlocker logBlocker(m_logFilter);
    m_logFilter->clear();
    m_logFilter->addItem(qtTrId("otg.multi.all_logs"), "*");
    m_logFilter->addItem(qtTrId("otg.multi.service_logs"), "");
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
        m_logFilter->addItem(it->value("phone").toString(), it.key());
    m_logFilter->setCurrentIndex(qMax(0, m_logFilter->findData(filter)));
    updateTray();
}

void MainWindow::updateLayoutPreview()
{
    const auto selectedNeighbor = m_records.value(m_selectedId).value("neighbor", m_computer->text()).toString();
    QSignalBlocker blocker(m_neighbor);
    m_neighbor->clear();
    m_neighbor->addItem(m_computer->text(), m_computer->text());
    QStringList lines;
    QList<ScreenLink> screens;
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        const auto name = it->value("phone").toString();
        if (it.key() != m_selectedId) m_neighbor->addItem(name, name);
        const auto direction = it->value("direction", "right").toString();
        const auto neighbor = it->value("neighbor", m_computer->text()).toString();
        screens.append({name, neighbor, direction});
        const auto index = m_direction->findData(direction);
        lines.append(qtTrId("otg.multi.layout_edge").arg(name, neighbor, index < 0 ? direction : m_direction->itemText(index)));
    }
    if (m_neighbor->findData(selectedNeighbor) < 0) m_neighbor->addItem(selectedNeighbor, selectedNeighbor);
    m_neighbor->setCurrentIndex(m_neighbor->findData(selectedNeighbor));
    if (layoutConfiguration().isEmpty()) lines.append(qtTrId("otg.multi.invalid_layout"));
    m_layoutPreview->setText(lines.join("\n\n"));
    m_layoutCanvas->setScreens(m_computer->text(), screens, m_phone->text());
}

QString MainWindow::layoutConfiguration() const
{
    QList<ScreenLink> screens;
    for (const auto &record : m_records)
        screens.append({record.value("phone").toString(), record.value("neighbor", m_computer->text()).toString(),
                        record.value("direction", "right").toString()});
    return serverConfiguration(m_computer->text().trimmed(), screens);
}

ConnectionOptions MainWindow::connectionOptions(const QVariantMap &profile) const
{
    ConnectionOptions options;
    options.bridge = bridgeExecutable();
    options.deskflow = m_deskflow->text().trimmed();
    options.computer = m_computer->text().trimmed();
    options.host = m_host->text().trimmed();
    options.port = m_port->value();
    options.manageServer = m_manage->isChecked();
    options.phone = profile.value("phone", "android").toString();
    options.serial = profile.value("serial").toString();
    options.transport = profile.value("transport", "usb").toString();
    options.mouseMode = options.transport == "ble" ? "absolute" : profile.value("mouseMode", "absolute").toString();
    options.blePeer = profile.value("blePeer").toString();
#ifdef Q_OS_LINUX
    options.adapter = profile.value("bleAdapter", "hci0").toString();
    options.bleIsolate = profile.value("bleIsolate", false).toBool();
#else
    options.adapter.clear();
#endif
    const double multiplier = options.mouseMode == "absolute" ? profile.value("sensitivityMultiplier", 1.0).toDouble() : 1.0;
    options.width = qMax(1, qRound(profile.value("originalWidth", 1080).toInt() / qMax(1.0, multiplier)));
    options.height = qMax(1, qRound(profile.value("originalHeight", 1920).toInt() / qMax(1.0, multiplier)));
    return options;
}

void MainWindow::connectAll()
{
    if (m_closing || m_scanning) return;
    saveSettings();
    const auto selected = m_selectedId;
    const auto ids = m_records.keys();
    for (const auto &id : ids) {
        if (m_connections.state(id) != Session::State::Idle) continue;
        selectDevice(id);
        connectDevice(); // Includes the existing per-adapter isolation confirmation.
    }
    selectDevice(selected);
}

void MainWindow::updateEnabled()
{
    const bool idle = m_connections.state(m_selectedId) == Session::State::Idle;
    const bool sharedIdle = !m_connections.busy() && m_connections.serverState() == DeskflowServer::State::Idle;
    const bool ble = m_transport->currentData() == "ble";
#ifdef Q_OS_LINUX
    const bool linuxBle = ble;
#else
    const bool linuxBle = false;
#endif
    m_deviceForm->setRowVisible(m_adapter, linuxBle);
    m_deviceForm->setRowVisible(m_blePeer, ble);
    m_deviceForm->setRowVisible(m_bleIsolate, linuxBle);
    m_deviceForm->setRowVisible(m_stopBleService, linuxBle || (ble && m_session->hasBleProvider()));
    m_deviceForm->setRowVisible(m_devices->parentWidget(), !ble);
    m_deviceForm->setRowVisible(m_serial, !ble);
    m_deviceForm->setRowVisible(m_autoConnectEnabled, !ble);
    m_programForm->setRowVisible(m_adb->parentWidget(), !ble);
    m_deviceForm->setRowVisible(m_mouseMode, !ble);
    m_autoConnect.setReady(!m_closing);
    if (m_closing)
        m_autoConnect.setEnabled(false);
    m_form->setEnabled(idle && !m_scanning && !m_closing && !m_stoppingBleService);
    m_sharedBox->setEnabled(sharedIdle && !m_closing);
    m_layoutBox->setEnabled(!m_closing);
    m_neighbor->setEnabled(sharedIdle && m_manage->isChecked());
    m_addDevice->setEnabled(sharedIdle && !m_closing);
    m_removeDevice->setEnabled(sharedIdle && m_records.size() > 1 && !m_session->hasBleProvider() && !m_closing);
    m_deviceSidebar->setEnabled(!m_closing);
    bool connectable = false;
    for (auto it = m_records.cbegin(); it != m_records.cend(); ++it) {
        const auto profile = it.key() == m_selectedId ? deviceProfile() : it.value();
        const auto identity = profile.value(profile.value("transport", "usb") == "ble" ? "blePeer" : "serial").toString();
        connectable |= m_connections.state(it.key()) == Session::State::Idle && !identity.isEmpty();
    }
    m_connectAll->setEnabled(connectable && !m_closing && !m_scanning && m_connections.serverState() != DeskflowServer::State::Stopping);
    m_disconnectAll->setEnabled(m_connections.busy() && !m_closing);
    m_connect->setEnabled(idle && !m_scanning && !m_closing && !m_stoppingBleService &&
                          m_connections.serverState() != DeskflowServer::State::Stopping &&
                          !(ble ? m_blePeer : m_serial)->text().trimmed().isEmpty());
    m_disconnect->setEnabled(!idle && m_connections.state(m_selectedId) != Session::State::Stopping);
    m_host->setEnabled(!m_manage->isChecked());
    m_direction->setEnabled(sharedIdle && m_manage->isChecked());
    m_computer->setEnabled(m_manage->isChecked());
    m_phone->setEnabled(sharedIdle || !m_manage->isChecked());
    m_serial->setEnabled(!ble);
    m_devices->setEnabled(!ble);
    m_refresh->setEnabled(!ble);
    m_adb->setEnabled(!ble);
    m_autoConnectEnabled->setEnabled(!ble);
    m_adapter->setEnabled(linuxBle);
    m_blePeer->setEnabled(ble);
    m_bleIsolate->setEnabled(linuxBle);
    m_stopBleService->setEnabled((linuxBle || m_session->hasBleProvider()) && !m_stoppingBleService);
    m_mouseMode->setEnabled(!ble);
    updateTray();
}

QSize MainWindow::virtualScreenSize() const
{
    const double multiplier = m_mouseMode->currentData() == "absolute" ? m_sensitivity->value() : 1.0;
    return QSize(qMax(1, qRound(m_width->value() / multiplier)),
                 qMax(1, qRound(m_height->value() / multiplier)));
}

void MainWindow::updateVirtualSize()
{
    const bool absolute = m_mouseMode->currentData() == "absolute";
    m_sensitivity->setEnabled(absolute);
    const auto size = virtualScreenSize();
    m_virtualSize->setText(QString("%1 × %2%3").arg(size.width()).arg(size.height()).arg(
        absolute ? qtTrId("otg.size.absolute_note") : qtTrId("otg.size.relative_note")));
}

void MainWindow::refreshDevices()
{
    if (m_transport->currentData() != "usb" || m_scanning || m_connections.state(m_selectedId) != Session::State::Idle || m_closing)
        return;
    if (m_adb->text().trimmed().isEmpty()) {
        setTranslatedProperty(m_deviceStatus, "text", "otg.device.adb_not_found");
        return;
    }
    m_scanning = true;
    updateEnabled();
    const auto request = ++m_request;
    const QString previous = m_serial->text();
    runCommand(this, m_adb->text(), {"devices", "-l"},
               [this, request, previous](bool ok, const QString &output) {
        if (request != m_request)
            return;
        m_scanning = false;
        if (!ok) {
            setTranslatedProperty(m_deviceStatus, "text", "otg.device.adb_failed");
            appendLog("[ADB] " + output);
            updateEnabled();
            return;
        }
        m_deviceList = parseDevices(output);
        {
            QSignalBlocker blocker(m_devices);
            m_devices->clear();
            m_devices->addItem(qtTrId("otg.device.manual"), -1);
            int selected = 0;
            for (int i = 0; i < m_deviceList.size(); ++i) {
                const auto &device = m_deviceList[i];
                m_devices->addItem(QString("%1 · %2 [%3]").arg(device.model, device.serial, device.state), i);
                if (device.serial == previous)
                    selected = i + 1;
            }
            if (!selected && m_deviceList.size() == 1)
                selected = 1;
            m_devices->setCurrentIndex(selected);
        }
        readDevice();
        retranslateUi();
    });
}

void MainWindow::readDevice()
{
    const auto request = ++m_request;
    const int index = m_devices->currentData().toInt();
    if (index < 0 || index >= m_deviceList.size()) {
        setTranslatedProperty(m_deviceStatus, "text", "otg.device.manual_hint");
        m_scanning = false;
        updateEnabled();
        return;
    }
    const auto device = m_deviceList[index];
    if (device.state != "device") {
        m_serial->clear();
        setTranslatedProperty(m_deviceStatus, "text", device.state == "unauthorized" ? "otg.device.authorize_hint" :
                                 "otg.device.offline_hint");
        updateEnabled();
        return;
    }
    m_serial->setText(device.serial);
    m_scanning = true;
    updateEnabled();
    const QString adb = m_adb->text();
    setTranslatedProperty(m_deviceStatus, "text", "otg.device.reading");
    runCommand(this, adb, {"-s", device.serial, "get-devpath"},
               [this, request, device, adb](bool ok, const QString &path) {
        if (request != m_request)
            return;
        const QString transportPath = path.trimmed();
        // Windows' native ADB USB backend registers transports with a null
        // devpath. adb reports "unknown" in that case. AOA will still require
        // an exact match against the physical USB descriptor before opening.
        const bool pathUnavailable = transportPath == "unknown";
        if (!ok || (!transportPath.startsWith("usb:") && !pathUnavailable)) {
            m_scanning = false;
            m_serial->clear();
            setTranslatedProperty(m_deviceStatus, "text", "otg.device.usb_unconfirmed");
            appendLog("[ADB transport] " + path);
            updateEnabled();
            return;
        }
        if (pathUnavailable)
            appendLog(qtTrId("otg.log.usb_path_unavailable"));
        runCommand(this, adb, {"-s", device.serial, "shell", "wm", "size"},
                   [this, request, device, adb](bool sizeOk, const QString &text) {
            if (request != m_request)
                return;
            const QSize size = sizeOk ? parseScreenSize(text) : QSize();
            if (!size.isValid()) {
                m_scanning = false;
                setTranslatedProperty(m_deviceStatus, "text", "otg.device.size_failed");
                appendLog("[ADB wm size] " + text);
                updateEnabled();
                return;
            }
            runCommand(this, adb, {"-s", device.serial, "shell", "dumpsys", "input"},
                       [this, request, size](bool rotationOk, const QString &text) {
                if (request != m_request)
                    return;
                const int rotation = rotationOk ? parseRotation(text) : -1;
                QSize oriented = size;
                if (rotation == 1 || rotation == 3)
                    oriented.transpose();
                m_width->setValue(oriented.width());
                m_height->setValue(oriented.height());
                m_scanning = false;
                setTranslatedProperty(m_deviceStatus, "text", rotation < 0 ?
                    "otg.device.orientation_unknown" :
                    "otg.device.ready");
                updateEnabled();
            });
        });
    });
}

void MainWindow::connectDevice()
{
    if (m_closing || m_scanning || m_connections.state(m_selectedId) != Session::State::Idle)
        return;
    saveSettings();
    const auto options = connectionOptions(deviceProfile());
    if (options.transport == "ble" && options.bleIsolate &&
        QMessageBox::warning(this, qtTrId("otg.ble.confirm_title"),
                             qtTrId("otg.ble.confirm").arg(options.adapter),
                             QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok)
        return;
    if (options.bridge.isEmpty()) {
        m_sessionStatus = false;
        setTranslatedProperty(m_status, "text", "otg.error.bridge_not_found");
        updateTray();
        return;
    }
    storeDevice();
    m_pendingProfiles[m_selectedId] = deviceProfile();
    if (options.transport == "usb") m_autoConnect.suppressSerial(options.serial);
    m_connections.start(m_selectedId, options, layoutConfiguration());
}

QVariantMap MainWindow::deviceProfile() const
{
    return {{"serial", m_serial->text().trimmed()},
            {"alias", m_alias->text().trimmed()},
            {"transport", m_transport->currentData()}, {"bleAdapter", m_adapter->text()},
            {"blePeer", m_blePeer->text().trimmed().toUpper()}, {"bleIsolate", m_bleIsolate->isChecked()},
            {"autoConnect", m_autoConnectEnabled->isChecked()}, {"neighbor", m_neighbor->currentData()},
            {"originalWidth", m_width->value()}, {"originalHeight", m_height->value()},
            {"sensitivityMultiplier", m_sensitivity->value()},
            {"deskflow", m_deskflow->text()}, {"computer", m_computer->text()},
            {"phone", m_phone->text()}, {"host", m_host->text()}, {"port", m_port->value()},
            {"manageServer", m_manage->isChecked()}, {"direction", m_direction->currentData()},
            {"mouseMode", m_mouseMode->currentData()}};
}

void MainWindow::applyDeviceProfile(const QVariantMap &profile)
{
    m_loadingDevice = true;
    ++m_request;
    m_scanning = false;
    const QSignalBlocker blocker(m_devices);
    m_devices->setCurrentIndex(0);
    m_serial->setText(profile.value("serial").toString());
    m_width->setValue(profile.value("originalWidth", 1080).toInt());
    m_height->setValue(profile.value("originalHeight", 1920).toInt());
    m_sensitivity->setValue(profile.value("sensitivityMultiplier", 1.0).toDouble());
    m_phone->setText(profile.value("phone", "android").toString());
    m_alias->setText(profile.value("alias", m_phone->text()).toString());
    m_transport->setCurrentIndex(qMax(0, m_transport->findData(profile.value("transport", "usb"))));
    m_adapter->setText(profile.value("bleAdapter", "hci0").toString());
    m_blePeer->setText(profile.value("blePeer").toString());
    m_bleIsolate->setChecked(profile.value("bleIsolate", false).toBool());
    m_autoConnectEnabled->setChecked(profile.value("autoConnect", false).toBool());
    m_direction->setCurrentIndex(qMax(0, m_direction->findData(profile.value("direction", "right"))));
    m_mouseMode->setCurrentIndex(qMax(0, m_mouseMode->findData(profile.value("mouseMode", "absolute"))));
    if (m_transport->currentData() == "ble") m_mouseMode->setCurrentIndex(0);
    const auto neighbor = profile.value("neighbor", m_computer->text()).toString();
    if (m_neighbor->findData(neighbor) < 0) m_neighbor->addItem(neighbor, neighbor);
    m_neighbor->setCurrentIndex(m_neighbor->findData(neighbor));
    setTranslatedProperty(m_deviceStatus, "text", "otg.auto_connect.restored");
    m_loadingDevice = false;
}

void MainWindow::rememberDevice()
{
    const auto serial = m_pendingProfile.value("serial").toString();
    if (serial.isEmpty())
        return;
    m_profiles.insert(serial, m_pendingProfile);
    m_pendingProfile.clear();
    QSettings settings;
    settings.beginWriteArray("rememberedDevices", m_profiles.size());
    int index = 0;
    for (const auto &profile : m_profiles) {
        settings.setArrayIndex(index++);
        settings.setValue("profile", profile);
    }
    settings.endArray();
    updateAutoConnect();
}

void MainWindow::saveSettings()
{
    storeDevice();
    persistDevices();
    QSettings settings;
    settings.setValue("adb", m_adb->text());
    settings.setValue("deskflow", m_deskflow->text());
    settings.setValue("serial", m_serial->text());
    settings.setValue("transport", m_transport->currentData());
    settings.setValue("bleAdapter", m_adapter->text());
    settings.setValue("blePeer", m_blePeer->text());
    settings.setValue("bleIsolate", m_bleIsolate->isChecked());
    settings.setValue("originalWidth", m_width->value());
    settings.setValue("originalHeight", m_height->value());
    settings.setValue("sensitivityMultiplier", m_sensitivity->value());
    const auto size = virtualScreenSize();
    settings.setValue("width", size.width());
    settings.setValue("height", size.height());
    settings.setValue("computer", m_computer->text());
    settings.setValue("phone", m_phone->text());
    settings.setValue("host", m_host->text());
    settings.setValue("port", m_port->value());
    settings.setValue("manageServer", m_manage->isChecked());
    settings.setValue("direction", m_direction->currentData());
    settings.setValue("mouseMode", m_mouseMode->currentData());
    settings.setValue("autoConnect", m_autoConnectEnabled->isChecked());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    if (!m_closing && m_tray->isVisible() && trayAvailable()) {
        event->ignore();
        hide();
        return;
    }
    if (m_connections.settled()) {
        m_closing = true;
        m_autoConnect.setEnabled(false);
        m_tray->hide();
        event->accept();
        emit exitReady();
        return;
    }
    m_closing = true;
    event->ignore();
    m_connections.shutdown();
    updateEnabled();
}
