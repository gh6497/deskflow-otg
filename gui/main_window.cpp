// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include "process_utils.h"
#include "i18n.h"

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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

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
    QSignalBlocker languageBlocker(m_language);
    m_language->setItemText(0, qtTrId("otg.language.chinese"));
    m_language->setItemText(1, qtTrId("otg.language.english"));
    if (m_sessionStatus)
        m_status->setText(m_session.statusMessage());
    updateVirtualSize();
    updateTray();
}

bool MainWindow::trayAvailable() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
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
    connect(m_trayDisconnect, &QAction::triggered, &m_session, &Session::stop);
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
    m_tray->setToolTip(QString("Deskflow OTG · %1").arg(m_status->text()));
    m_trayDisconnect->setEnabled(m_disconnect->isEnabled());
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

MainWindow::MainWindow() : m_session(this)
{
    QSettings settings;
    auto &languages = LanguageManager::instance();
    const QString savedLanguage = settings.value("language", LanguageManager::systemLanguage()).toString();
    if (!languages.setLanguage(savedLanguage))
        languages.setLanguage(LanguageManager::systemLanguage());
    setTranslatedProperty(this, "windowTitle", "otg.window.title");
    resize(800, 780);
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
    root->addLayout(headingRow);
    auto *intro = translated(new QLabel, "text", "otg.window.intro");
    root->addWidget(intro);

    m_form = new QWidget;
    auto *formLayout = new QVBoxLayout(m_form);
    formLayout->setContentsMargins(0, 0, 0, 0);
    auto *deviceBox = translated(new QGroupBox, "title", "otg.section.device");
    auto *deviceForm = new QFormLayout(deviceBox);
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
    addTranslatedRow(serverForm, "otg.label.position", m_direction);
    m_computer = new QLineEdit;
    m_phone = new QLineEdit;
    m_host = new QLineEdit;
    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    addTranslatedRow(serverForm, "otg.label.computer_name", m_computer);
    addTranslatedRow(serverForm, "otg.label.phone_name", m_phone);
    addTranslatedRow(serverForm, "otg.label.server_address", m_host);
    addTranslatedRow(serverForm, "otg.label.server_port", m_port);
    formLayout->addWidget(serverBox);

    auto *advancedBox = translated(new QGroupBox, "title", "otg.section.programs");
    auto *advancedForm = new QFormLayout(advancedBox);
    advancedForm->addRow("ADB", pathField(m_adb, "otg.path.adb_hint"));
    advancedForm->addRow("Deskflow", pathField(m_deskflow, "otg.path.deskflow_hint"));
    m_mouseMode = new QComboBox;
    m_mouseMode->setObjectName("mouseMode");
    m_mouseMode->addItem(qtTrId("otg.mouse.absolute"), "absolute");
    m_mouseMode->addItem(qtTrId("otg.mouse.relative"), "relative");
    addTranslatedRow(advancedForm, "otg.label.mouse_mode", m_mouseMode);
    formLayout->addWidget(advancedBox);
    root->addWidget(m_form);

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
    root->addLayout(buttons);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1200);
    m_log->setMinimumHeight(100);
    setTranslatedProperty(m_log, "placeholderText", "otg.log.placeholder");
    root->addWidget(m_log, 1);
    setCentralWidget(central);

    m_adb->setText(settings.value("adb", findAdb()).toString());
    m_deskflow->setText(settings.value("deskflow", findDeskflow()).toString());
    m_serial->setText(settings.value("serial").toString());
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
    connect(m_width, &QSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_height, &QSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_sensitivity, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateVirtualSize);
    connect(m_mouseMode, &QComboBox::currentIndexChanged, this, &MainWindow::updateVirtualSize);
    updateVirtualSize();
    connect(m_manage, &QCheckBox::toggled, this, &MainWindow::updateEnabled);
    connect(m_refresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(m_devices, &QComboBox::currentIndexChanged, this, &MainWindow::readDevice);
    connect(m_serial, &QLineEdit::textChanged, this, &MainWindow::updateEnabled);
    connect(m_connect, &QPushButton::clicked, this, &MainWindow::connectDevice);
    connect(m_disconnect, &QPushButton::clicked, &m_session, &Session::stop);
    connect(&m_session, &Session::log, this, &MainWindow::appendLog);
    connect(&m_session, &Session::changed, this, [this](Session::State state, const QString &message) {
        m_sessionStatus = true;
        m_status->setText(message);
        updateEnabled();
        if (m_closing && state == Session::State::Idle)
            QTimer::singleShot(0, this, &QWidget::close);
    });
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
    QTimer::singleShot(0, this, &MainWindow::refreshDevices);
}

void MainWindow::appendLog(const QString &text)
{
    if (!text.isEmpty())
        m_log->appendPlainText(QTime::currentTime().toString("HH:mm:ss ") + text);
}

void MainWindow::updateEnabled()
{
    const bool idle = m_session.state() == Session::State::Idle;
    m_form->setEnabled(idle && !m_scanning && !m_closing);
    m_connect->setEnabled(idle && !m_scanning && !m_closing && !m_serial->text().trimmed().isEmpty());
    m_disconnect->setEnabled(!idle && m_session.state() != Session::State::Stopping);
    m_host->setEnabled(!m_manage->isChecked());
    m_direction->setEnabled(m_manage->isChecked());
    m_computer->setEnabled(m_manage->isChecked());
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
    if (m_scanning || m_session.state() != Session::State::Idle || m_closing)
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
    saveSettings();
    ConnectionOptions options;
    options.bridge = bridgePath();
    options.deskflow = m_deskflow->text().trimmed();
    options.serial = m_serial->text().trimmed();
    options.host = m_host->text().trimmed();
    options.computer = m_computer->text().trimmed();
    options.phone = m_phone->text().trimmed();
    options.direction = m_direction->currentData().toString();
    options.mouseMode = m_mouseMode->currentData().toString();
    options.port = m_port->value();
    const auto size = virtualScreenSize();
    options.width = size.width();
    options.height = size.height();
    options.manageServer = m_manage->isChecked();
    if (options.bridge.isEmpty()) {
        m_sessionStatus = false;
        setTranslatedProperty(m_status, "text", "otg.error.bridge_not_found");
        updateTray();
        return;
    }
    m_session.start(options);
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue("adb", m_adb->text());
    settings.setValue("deskflow", m_deskflow->text());
    settings.setValue("serial", m_serial->text());
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
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    if (!m_closing && m_tray->isVisible() && trayAvailable()) {
        event->ignore();
        hide();
        return;
    }
    if (m_session.state() == Session::State::Idle) {
        m_closing = true;
        m_tray->hide();
        event->accept();
        emit exitReady();
        return;
    }
    m_closing = true;
    event->ignore();
    m_session.stop();
    updateEnabled();
}
