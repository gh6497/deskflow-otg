// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QDialog>
#include <QList>
#include <QString>

struct WirelessAdbService {
    QString target;
    bool pairing = false;
};

QList<WirelessAdbService> parseWirelessAdbServices(const QString &output);

class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;

class WirelessAdbDialog : public QDialog {
    Q_OBJECT
public:
    explicit WirelessAdbDialog(const QString &adbPath, QWidget *parent = nullptr);

signals:
    void targetReady(const QString &target);
    void logMessage(const QString &message);

private:
    void refreshServices();
    void pairDevice();
    void connectDevice();
    void setBusy(bool busy);
    void setStatus(const QString &text);
    void addService(QComboBox *combo, const QString &target);

    QString m_adbPath;
    QComboBox *m_pairingTarget;
    QComboBox *m_connectionTarget;
    QLineEdit *m_pairingCode;
    QLabel *m_status;
    QPushButton *m_refresh;
    QPushButton *m_pair;
    QPushButton *m_connect;
    bool m_busy = false;
};
