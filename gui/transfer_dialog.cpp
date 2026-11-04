// SPDX-License-Identifier: Apache-2.0
#include "transfer_dialog.h"
#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

TransferDialog::TransferDialog(const QString &adb, const QString &target, QWidget *parent)
    : QDialog(parent), m_transfer(adb, target, this)
{
    setObjectName("transferDialog");
    resize(620, 440);
    auto *layout = new QVBoxLayout(this);
    m_hint = new QLabel; m_hint->setWordWrap(true); layout->addWidget(m_hint);
    auto *targetLabel = new QLabel(target); targetLabel->setTextFormat(Qt::PlainText); layout->addWidget(targetLabel);
    m_files = new QListWidget; m_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_files->setObjectName("sharedFiles"); layout->addWidget(m_files);
    auto *buttons = new QHBoxLayout;
    m_refresh = new QPushButton; m_refresh->setObjectName("transferRefresh");
    m_receive = new QPushButton; m_receive->setObjectName("transferReceive");
    m_send = new QPushButton; m_send->setObjectName("transferSend");
    m_cancel = new QPushButton; m_cancel->setObjectName("transferCancel");
    for (auto *button : {m_refresh, m_receive, m_send, m_cancel}) buttons->addWidget(button);
    layout->addLayout(buttons);
    m_current = new QLabel; m_current->setTextFormat(Qt::PlainText); m_current->setWordWrap(true);
    layout->addWidget(m_current);
    m_progress = new QProgressBar; m_progress->setRange(0, 100); layout->addWidget(m_progress);
    m_status = new QLabel; m_status->setWordWrap(true); layout->addWidget(m_status);
    connect(m_refresh, &QPushButton::clicked, this, [this] { m_transfer.refresh(); enabled(); });
    connect(m_receive, &QPushButton::clicked, this, [this] {
        QStringList ids;
        for (const auto *item : m_files->selectedItems()) ids << item->data(Qt::UserRole).toString();
        if (ids.isEmpty()) return;
        const auto path = QFileDialog::getExistingDirectory(this, qtTrId("otg.transfer.directory"),
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        if (!path.isEmpty()) { m_transfer.receive(ids, path); enabled(); }
    });
    connect(m_send, &QPushButton::clicked, this, [this] {
        const auto paths = QFileDialog::getOpenFileNames(this, qtTrId("otg.transfer.send"));
        if (!paths.isEmpty()) { m_transfer.send(paths); enabled(); }
    });
    connect(m_cancel, &QPushButton::clicked, &m_transfer, &CompanionTransfer::cancel);
    connect(m_files, &QListWidget::itemSelectionChanged, this, &TransferDialog::enabled);
    connect(&m_transfer, &CompanionTransfer::listed, this, [this] {
        m_files->clear();
        for (const auto &f : m_transfer.files()) {
            auto *item = new QListWidgetItem(QString("%1 (%2 B)").arg(f.name).arg(f.size), m_files);
            item->setData(Qt::UserRole, f.id);
        }
    });
    connect(&m_transfer, &CompanionTransfer::progress, this, [this](const QString &name, qint64 bytes, qint64 total) {
        m_current->setText(QString("%1 · %2 / %3 B").arg(name).arg(bytes).arg(total));
        m_progress->setValue(total ? int(bytes * 100 / total) : 100);
    });
    connect(&m_transfer, &CompanionTransfer::completed, this, [this](bool ok, const QString &id, const QString &) {
        m_statusId = id; translate(); enabled();
        if (ok && id == "otg.transfer.done") m_files->clear();
        if (m_rejecting) {
            QDialog::reject();
            emit stopped();
        }
    });
    translate(); enabled();
    m_transfer.refresh(); enabled();
}

void TransferDialog::enabled()
{
    const bool idle = !m_transfer.busy();
    m_refresh->setEnabled(idle); m_send->setEnabled(idle);
    m_receive->setEnabled(idle && !m_files->selectedItems().isEmpty());
    m_cancel->setEnabled(!idle); m_files->setEnabled(idle);
    m_status->setText(idle ? qtTrId(m_statusId.toLatin1().constData()) : qtTrId("otg.transfer.working"));
}
void TransferDialog::translate()
{
    setWindowTitle(qtTrId("otg.transfer.title"));
    m_hint->setText(qtTrId("otg.transfer.help"));
    m_refresh->setText(qtTrId("otg.transfer.refresh"));
    m_receive->setText(qtTrId("otg.transfer.receive"));
    m_send->setText(qtTrId("otg.transfer.send"));
    m_cancel->setText(qtTrId("otg.transfer.cancel"));
    m_status->setText(qtTrId(m_statusId.toLatin1().constData()));
    enabled();
}
void TransferDialog::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) translate();
}
void TransferDialog::reject()
{
    m_rejecting = true;
    if (m_transfer.busy()) {
        // Do not destroy the worker until its child process has actually exited.
        setEnabled(false);
        m_transfer.cancel();
    } else {
        QDialog::reject();
        emit stopped();
    }
}
