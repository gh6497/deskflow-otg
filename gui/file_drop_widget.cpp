// SPDX-License-Identifier: Apache-2.0
#include "file_drop_widget.h"
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>
#include <QSet>
#include <QStyle>

FileDropWidget::FileDropWidget(QWidget *parent) : QFrame(parent)
{
    setObjectName("transferDropWidget");
    setAcceptDrops(true);
    setFrameShape(QFrame::StyledPanel);
    setProperty("dropActive", false);
    setStyleSheet("QFrame#transferDropWidget { border: 1px dashed palette(mid); }");
}

bool FileDropWidget::validUrls(const QMimeData *data) const
{
    if (!data || !data->hasUrls() || data->urls().isEmpty()) return false;
    for (const auto &url : data->urls()) {
        if (!url.isLocalFile()) return false;
        const QFileInfo info(url.toLocalFile());
        if (!info.isFile() || !info.isReadable()) return false;
    }
    return true;
}

void FileDropWidget::setHover(bool hover)
{
    if (property("dropActive").toBool() == hover) return;
    setProperty("dropActive", hover);
    setStyleSheet(hover
        ? "QFrame#transferDropWidget { border: 2px dashed palette(highlight); }"
        : "QFrame#transferDropWidget { border: 1px dashed palette(mid); }");
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void FileDropWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (!validUrls(event->mimeData())) return;
    event->setDropAction(Qt::CopyAction);
    event->accept();
    setHover(true);
}

void FileDropWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (!validUrls(event->mimeData())) { setHover(false); event->ignore(); return; }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void FileDropWidget::dragLeaveEvent(QDragLeaveEvent *event)
{
    setHover(false);
    event->accept();
}

void FileDropWidget::dropEvent(QDropEvent *event)
{
    setHover(false);
    if (!validUrls(event->mimeData())) { event->ignore(); return; }
    QStringList paths;
    QSet<QString> seen;
    for (const auto &url : event->mimeData()->urls()) {
        const auto path = QFileInfo(url.toLocalFile()).absoluteFilePath();
        if (!seen.contains(path)) { seen.insert(path); paths.append(path); }
    }
    emit filesDropped(paths);
    event->setDropAction(Qt::CopyAction);
    event->accept();
}
