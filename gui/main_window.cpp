// SPDX-License-Identifier: Apache-2.0
#include "main_window.h"
#include "process_utils.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

QWidget *MainWindow::pathField(QLineEdit *&field, const QString &placeholder)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    field = new QLineEdit;
    field->setPlaceholderText(placeholder);
    auto *browse = new QPushButton("浏览…");
    layout->addWidget(field, 1);
    layout->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this, field] {
        const auto path = QFileDialog::getOpenFileName(this, "选择可执行程序", field->text());
        if (!path.isEmpty())
            field->setText(path);
    });
    return row;
}

MainWindow::MainWindow()
{
    setWindowTitle("Deskflow OTG · 安卓键鼠桥接");
    resize(800, 780);
    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(20, 16, 20, 16);
    auto *heading = new QLabel("Deskflow OTG");
    QFont titleFont = heading->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    heading->setFont(titleFont);
    root->addWidget(heading);
    auto *intro = new QLabel("通过 USB 将电脑键鼠共享到安卓手机");
    root->addWidget(intro);

    m_form = new QWidget;
    auto *formLayout = new QVBoxLayout(m_form);
    formLayout->setContentsMargins(0, 0, 0, 0);
    auto *deviceBox = new QGroupBox("1  选择手机");
    auto *deviceForm = new QFormLayout(deviceBox);
    auto *deviceRow = new QWidget;
    auto *deviceLayout = new QHBoxLayout(deviceRow);
    deviceLayout->setContentsMargins(0, 0, 0, 0);
    m_devices = new QComboBox;
    m_devices->addItem("手动填写 USB 序列号", -1);
    m_refresh = new QPushButton("刷新设备");
    deviceLayout->addWidget(m_devices, 1);
    deviceLayout->addWidget(m_refresh);
    deviceForm->addRow("设备", deviceRow);
    m_serial = new QLineEdit;
    m_serial->setObjectName("usbSerial");
    deviceForm->addRow("USB 序列号", m_serial);
    m_deviceStatus = new QLabel("自动识别需要开启 USB 调试，并在手机上允许授权。");
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
    auto *swap = new QPushButton("交换宽高");
    swap->setObjectName("swapDimensions");
    sizeLayout->addWidget(swap);
    connect(swap, &QPushButton::clicked, this, [this] {
        const int width = m_width->value();
        m_width->setValue(m_height->value());
        m_height->setValue(width);
    });
    deviceForm->addRow("原始屏幕尺寸", sizeRow);
    m_sensitivity = new QDoubleSpinBox;
    m_sensitivity->setObjectName("sensitivityMultiplier");
    m_sensitivity->setRange(1.0, 10.0);
    m_sensitivity->setDecimals(2);
    m_sensitivity->setSingleStep(0.25);
    m_sensitivity->setSuffix(" ×");
    m_sensitivity->setToolTip("虚拟宽高 = 原始宽高 ÷ 倍率（四舍五入）。倍率越大，移动越省力；1× 恢复原始尺寸。仅用于绝对坐标模式。");
    deviceForm->addRow("灵敏度倍率", m_sensitivity);
    m_virtualSize = new QLabel;
    m_virtualSize->setObjectName("virtualScreenSize");
    m_virtualSize->setWordWrap(true);
    deviceForm->addRow("实际虚拟尺寸", m_virtualSize);
    formLayout->addWidget(deviceBox);

    auto *serverBox = new QGroupBox("2  Deskflow 连接");
    auto *serverForm = new QFormLayout(serverBox);
    m_manage = new QCheckBox("自动配置并启动本机 Deskflow");
    serverForm->addRow(m_manage);
    m_direction = new QComboBox;
    m_direction->addItem("手机在电脑右侧", "right");
    m_direction->addItem("手机在电脑左侧", "left");
    m_direction->addItem("手机在电脑上方", "up");
    m_direction->addItem("手机在电脑下方", "down");
    serverForm->addRow("屏幕位置", m_direction);
    m_computer = new QLineEdit;
    m_phone = new QLineEdit;
    m_host = new QLineEdit;
    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    serverForm->addRow("电脑屏幕名称", m_computer);
    serverForm->addRow("手机屏幕名称", m_phone);
    serverForm->addRow("服务地址", m_host);
    serverForm->addRow("服务端口", m_port);
    formLayout->addWidget(serverBox);

    auto *advancedBox = new QGroupBox("程序与模式");
    auto *advancedForm = new QFormLayout(advancedBox);
    advancedForm->addRow("ADB", pathField(m_adb, "优先使用随包提供的 ADB，也可指定外部程序"));
    advancedForm->addRow("Deskflow", pathField(m_deskflow, "选择已安装的 deskflow-core 或 deskflow-server"));
    m_mouseMode = new QComboBox;
    m_mouseMode->setObjectName("mouseMode");
    m_mouseMode->addItem("绝对坐标（推荐）", "absolute");
    m_mouseMode->addItem("相对坐标（兼容模式）", "relative");
    advancedForm->addRow("鼠标模式", m_mouseMode);
    formLayout->addWidget(advancedBox);
    root->addWidget(m_form);

    auto *buttons = new QHBoxLayout;
    m_status = new QLabel("未连接");
    m_status->setWordWrap(true);
    m_connect = new QPushButton("连接");
    m_connect->setObjectName("connectButton");
    m_connect->setDefault(true);
    m_disconnect = new QPushButton("断开");
    buttons->addWidget(m_status, 1);
    buttons->addWidget(m_connect);
    buttons->addWidget(m_disconnect);
    root->addLayout(buttons);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1200);
    m_log->setMinimumHeight(100);
    m_log->setPlaceholderText("运行日志将显示在这里");
    root->addWidget(m_log, 1);
    setCentralWidget(central);

    QSettings settings;
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
        m_status->setText(message);
        updateEnabled();
        if (m_closing && state == Session::State::Idle)
            QTimer::singleShot(0, this, &QWidget::close);
    });
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
        absolute ? "（原始尺寸 ÷ 倍率，连接时生效）" : "（相对模式使用原始尺寸）"));
}

void MainWindow::refreshDevices()
{
    if (m_scanning || m_session.state() != Session::State::Idle || m_closing)
        return;
    if (m_adb->text().trimmed().isEmpty()) {
        m_deviceStatus->setText("未找到 ADB，请选择 ADB 程序，或手动填写 USB 序列号和宽高。");
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
            m_deviceStatus->setText("ADB 识别失败，可重试或手动填写设备信息。");
            appendLog("[ADB] " + output);
            updateEnabled();
            return;
        }
        m_deviceList = parseDevices(output);
        {
            QSignalBlocker blocker(m_devices);
            m_devices->clear();
            m_devices->addItem("手动填写 USB 序列号", -1);
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
    });
}

void MainWindow::readDevice()
{
    const auto request = ++m_request;
    const int index = m_devices->currentData().toInt();
    if (index < 0 || index >= m_deviceList.size()) {
        m_deviceStatus->setText("手动模式：填写手机的 USB 序列号和当前方向尺寸；ADB 不是控制必需项。");
        m_scanning = false;
        updateEnabled();
        return;
    }
    const auto device = m_deviceList[index];
    if (device.state != "device") {
        m_serial->clear();
        m_deviceStatus->setText(device.state == "unauthorized" ? "请解锁手机并允许 USB 调试，然后刷新。" :
                                "设备离线或权限不足，请检查 USB 连接和 ADB 权限，然后刷新。");
        updateEnabled();
        return;
    }
    m_serial->setText(device.serial);
    m_scanning = true;
    updateEnabled();
    const QString adb = m_adb->text();
    m_deviceStatus->setText("正在读取 USB 传输信息及屏幕尺寸…");
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
            m_deviceStatus->setText("未确认 USB 传输，请使用 USB 连接；也可手动核对 USB 序列号。");
            appendLog("[ADB transport] " + path);
            updateEnabled();
            return;
        }
        if (pathUnavailable)
            appendLog("ADB 后端未提供设备路径；连接时将按 USB 序列号精确匹配设备。");
        runCommand(this, adb, {"-s", device.serial, "shell", "wm", "size"},
                   [this, request, device, adb](bool sizeOk, const QString &text) {
            if (request != m_request)
                return;
            const QSize size = sizeOk ? parseScreenSize(text) : QSize();
            if (!size.isValid()) {
                m_scanning = false;
                m_deviceStatus->setText("无法读取尺寸，请手动填写宽高。");
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
                m_deviceStatus->setText(rotation < 0 ?
                    "已识别尺寸；未识别屏幕方向，请按当前横竖屏交换宽高。" :
                    "已识别 USB 设备与当前方向尺寸，可手动调整宽高或连接。");
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
        m_status->setText("未找到同目录的 deskflow-otg 桥接程序，请检查安装包。");
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
    if (m_session.state() == Session::State::Idle) {
        event->accept();
        return;
    }
    m_closing = true;
    event->ignore();
    m_session.stop();
    updateEnabled();
}
