// SPDX-License-Identifier: Apache-2.0
#include "wireless_adb_dialog.h"
#include "i18n.h"
#include "process_utils.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

class NoWheelComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        event->ignore();
    }
};

enum class ServiceKind { Pairing, Connect };

bool isDeviceState(const QString &output)
{
    const auto lines = output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
    for (const auto &line : lines) {
        if (line.trimmed() == QStringLiteral("device"))
            return true;
    }
    return false;
}

QString serviceEndpoint(const QString &line)
{
    static const QRegularExpression pattern(
        QStringLiteral("(?:(?:\\[[^\\]\\r\\n]+\\])|(?:[A-Za-z0-9_.-]+)):[0-9]{1,5}"));
    const auto match = pattern.match(line);
    return match.hasMatch() ? match.captured(0) : QString();
}

} // namespace

QList<WirelessAdbService> parseWirelessAdbServices(const QString &output)
{
    QList<WirelessAdbService> services;
    ServiceKind pending = ServiceKind::Connect;
    bool havePending = false;
    const auto lines = output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
    for (const auto &rawLine : lines) {
        const auto line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QStringLiteral("List of discovered")))
            continue;

        const bool pairing = line.contains(QStringLiteral("_adb-tls-pairing"));
        const bool connecting = line.contains(QStringLiteral("_adb-tls-connect"));
        if (pairing || connecting) {
            pending = pairing ? ServiceKind::Pairing : ServiceKind::Connect;
            havePending = true;
        }

        const auto endpoint = serviceEndpoint(line);
        if (endpoint.isEmpty() || !havePending)
            continue;

        const bool isPairing = pending == ServiceKind::Pairing;
        bool duplicate = false;
        for (const auto &service : services) {
            if (service.target == endpoint && service.pairing == isPairing) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            services.append({endpoint, isPairing});
        havePending = false;
    }
    return services;
}

WirelessAdbDialog::WirelessAdbDialog(const QString &adbPath, QWidget *parent)
    : QDialog(parent), m_adbPath(adbPath.trimmed()),
      m_pairingTarget(new NoWheelComboBox(this)),
      m_connectionTarget(new NoWheelComboBox(this)),
      m_pairingCode(new QLineEdit(this)), m_status(new QLabel(this)),
      m_refresh(new QPushButton(this)), m_pair(new QPushButton(this)),
      m_connect(new QPushButton(this))
{
    setObjectName(QStringLiteral("wirelessAdbDialog"));
    setWindowTitle(qtTrId("otg.wireless.title"));
    resize(620, 330);

    auto *layout = new QVBoxLayout(this);
    auto *instructions = new QLabel(qtTrId("otg.wireless.instructions"), this);
    instructions->setWordWrap(true);
    layout->addWidget(instructions);

    auto *form = new QFormLayout;
    m_pairingTarget->setObjectName(QStringLiteral("wirelessPairingTarget"));
    m_pairingTarget->setEditable(true);
    m_pairingTarget->setInsertPolicy(QComboBox::NoInsert);
    form->addRow(new QLabel(qtTrId("otg.wireless.pairing_target"), this), m_pairingTarget);

    m_pairingCode->setObjectName(QStringLiteral("wirelessPairingCode"));
    m_pairingCode->setMaxLength(6);
    m_pairingCode->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{0,6}")), m_pairingCode));
    m_pairingCode->setPlaceholderText(qtTrId("otg.wireless.pairing_code_hint"));
    form->addRow(new QLabel(qtTrId("otg.wireless.pairing_code"), this), m_pairingCode);

    m_connectionTarget->setObjectName(QStringLiteral("wirelessConnectionTarget"));
    m_connectionTarget->setEditable(true);
    m_connectionTarget->setInsertPolicy(QComboBox::NoInsert);
    form->addRow(new QLabel(qtTrId("otg.wireless.connection_target"), this), m_connectionTarget);
    layout->addLayout(form);

    auto *actions = new QHBoxLayout;
    m_refresh->setObjectName(QStringLiteral("wirelessRefresh"));
    m_refresh->setText(qtTrId("otg.wireless.refresh"));
    m_pair->setObjectName(QStringLiteral("wirelessPair"));
    m_pair->setText(qtTrId("otg.wireless.pair"));
    m_connect->setObjectName(QStringLiteral("wirelessConnect"));
    m_connect->setText(qtTrId("otg.wireless.connect"));
    actions->addWidget(m_refresh);
    actions->addWidget(m_pair);
    actions->addWidget(m_connect);
    actions->addStretch();
    layout->addLayout(actions);

    m_status->setObjectName(QStringLiteral("wirelessStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_refresh, &QPushButton::clicked, this, &WirelessAdbDialog::refreshServices);
    connect(m_pair, &QPushButton::clicked, this, &WirelessAdbDialog::pairDevice);
    connect(m_connect, &QPushButton::clicked, this, &WirelessAdbDialog::connectDevice);

    if (m_adbPath.isEmpty()) {
        setStatus(qtTrId("otg.wireless.status.adb_missing"));
        setBusy(true);
    } else {
        setStatus(qtTrId("otg.wireless.status.ready"));
        QMetaObject::invokeMethod(this, &WirelessAdbDialog::refreshServices, Qt::QueuedConnection);
    }
}

void WirelessAdbDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void WirelessAdbDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_refresh->setEnabled(!busy && !m_adbPath.isEmpty());
    m_pair->setEnabled(!busy && !m_adbPath.isEmpty());
    m_connect->setEnabled(!busy && !m_adbPath.isEmpty());
    m_pairingTarget->setEnabled(!busy);
    m_connectionTarget->setEnabled(!busy);
    m_pairingCode->setEnabled(!busy);
}

void WirelessAdbDialog::addService(QComboBox *combo, const QString &target)
{
    if (target.isEmpty() || combo->findText(target) >= 0)
        return;
    combo->addItem(target);
}

void WirelessAdbDialog::refreshServices()
{
    if (m_busy || m_adbPath.isEmpty())
        return;
    setBusy(true);
    setStatus(qtTrId("otg.wireless.status.discovering"));
    runCommand(this, m_adbPath, {QStringLiteral("start-server")},
               [this](bool started, const QString &startOutput) {
        if (!started) {
            emit logMessage(QStringLiteral("[ADB] start-server: ") + startOutput);
            setStatus(qtTrId("otg.wireless.status.adb_failed"));
            setBusy(false);
            return;
        }
        runCommand(this, m_adbPath, {QStringLiteral("mdns"), QStringLiteral("services")},
                   [this](bool ok, const QString &output) {
            emit logMessage(QStringLiteral("[ADB] mdns services:\n") + output);
            const auto services = parseWirelessAdbServices(output);
            for (const auto &service : services) {
                if (service.pairing)
                    addService(m_pairingTarget, service.target);
                else
                    addService(m_connectionTarget, service.target);
            }
            if (!ok || services.isEmpty())
                setStatus(qtTrId("otg.wireless.status.no_services"));
            else
                setStatus(qtTrId("otg.wireless.status.ready"));
            setBusy(false);
        }, 5000);
    });
}

void WirelessAdbDialog::pairDevice()
{
    if (m_busy)
        return;
    const auto target = m_pairingTarget->currentText().trimmed();
    const auto code = m_pairingCode->text().trimmed();
    if (target.isEmpty()) {
        setStatus(qtTrId("otg.wireless.status.invalid_pairing"));
        return;
    }
    if (code.size() != 6) {
        setStatus(qtTrId("otg.wireless.status.invalid_code"));
        return;
    }
    setBusy(true);
    setStatus(qtTrId("otg.wireless.status.pairing"));
    runCommand(this, m_adbPath, {QStringLiteral("pair"), target, code},
               [this, target](bool ok, const QString &output) {
        // Do not include the one-time pairing code in the log or any persisted state.
        m_pairingCode->clear();
        emit logMessage(QStringLiteral("[ADB] pair %1: %2\n%3")
                            .arg(target, ok ? QStringLiteral("ok") : QStringLiteral("failed"), output));
        setBusy(false);
        if (!ok) {
            setStatus(qtTrId("otg.wireless.status.pair_failed"));
            return;
        }
        setStatus(qtTrId("otg.wireless.status.paired"));
        refreshServices();
    }, 15000);
}

void WirelessAdbDialog::connectDevice()
{
    if (m_busy)
        return;
    const auto target = m_connectionTarget->currentText().trimmed();
    if (target.isEmpty()) {
        setStatus(qtTrId("otg.wireless.status.invalid_connection"));
        return;
    }
    setBusy(true);
    setStatus(qtTrId("otg.wireless.status.connecting"));
    runCommand(this, m_adbPath, {QStringLiteral("connect"), target},
               [this, target](bool ok, const QString &output) {
        emit logMessage(QStringLiteral("[ADB] connect %1: %2\n%3")
                            .arg(target, ok ? QStringLiteral("ok") : QStringLiteral("failed"), output));
        if (!ok) {
            setStatus(qtTrId("otg.wireless.status.connect_failed"));
            setBusy(false);
            return;
        }
        runCommand(this, m_adbPath,
                   {QStringLiteral("-s"), target, QStringLiteral("get-state")},
                   [this, target](bool stateOk, const QString &state) {
            const bool ready = stateOk && isDeviceState(state);
            emit logMessage(QStringLiteral("[ADB] get-state %1: %2")
                                .arg(target, state.trimmed()));
            setBusy(false);
            if (!ready) {
                setStatus(qtTrId("otg.wireless.status.connect_failed"));
                return;
            }
            setStatus(qtTrId("otg.wireless.status.connected"));
            emit targetReady(target);
        });
    }, 15000);
}
