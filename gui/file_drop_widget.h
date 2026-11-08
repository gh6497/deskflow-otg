// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QFrame>

class QMimeData;

class FileDropWidget : public QFrame {
    Q_OBJECT
public:
    explicit FileDropWidget(QWidget *parent = nullptr);

signals:
    void filesDropped(const QStringList &paths);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    bool validUrls(const QMimeData *data) const;
    void setHover(bool hover);
};
