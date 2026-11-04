// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "companion_transfer.h"
#include <QDialog>
class QListWidget;
class QPushButton;
class QLabel;
class QProgressBar;

class TransferDialog : public QDialog {
    Q_OBJECT
public:
    TransferDialog(const QString &adb, const QString &target, QWidget *parent = nullptr);
    void reject() override;
signals:
    void stopped();
protected:
    void changeEvent(QEvent *event) override;
private:
    void translate();
    void enabled();
    CompanionTransfer m_transfer;
    bool m_rejecting = false;
    QString m_statusId = "otg.transfer.help";
    QListWidget *m_files;
    QPushButton *m_refresh, *m_receive, *m_send, *m_cancel;
    QLabel *m_hint, *m_status, *m_current;
    QProgressBar *m_progress;
};
