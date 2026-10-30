// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "device_info.h"
#include <QMap>
#include <QPainter>
#include <QWidget>

// Read-only, palette-aware preview. Editing stays in the explicit edge controls.
class ScreenLayoutPreview : public QWidget {
public:
    explicit ScreenLayoutPreview(QWidget *parent = nullptr) : QWidget(parent) { setMinimumHeight(180); }
    QSize sizeHint() const override { return {480, 200}; }
    void setScreens(const QString &computer, const QList<ScreenLink> &screens, const QString &selected)
    {
        m_computer = computer; m_screens = screens; m_selected = selected;
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        if (serverConfiguration(m_computer, m_screens).isEmpty()) return;
        QMap<QString, QPoint> positions{{m_computer, {0, 0}}};
        for (qsizetype pass = 0; pass < m_screens.size(); ++pass) {
            for (const auto &screen : m_screens) {
                if (positions.contains(screen.name) || !positions.contains(screen.neighbor)) continue;
                const QPoint step = screen.direction == "left" ? QPoint(-1, 0) :
                    screen.direction == "right" ? QPoint(1, 0) : screen.direction == "up" ? QPoint(0, -1) : QPoint(0, 1);
                positions[screen.name] = positions.value(screen.neighbor) + step;
            }
        }
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        for (const auto &point : positions) {
            minX = qMin(minX, point.x()); maxX = qMax(maxX, point.x());
            minY = qMin(minY, point.y()); maxY = qMax(maxY, point.y());
        }
        const qreal spanW = (maxX - minX) * 150 + 120;
        const qreal spanH = (maxY - minY) * 90 + 55;
        const qreal scale = qMin(qreal(width() - 20) / spanW, qreal(height() - 20) / spanH);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate((width() - spanW * scale) / 2, (height() - spanH * scale) / 2);
        painter.scale(scale, scale);
        auto rect = [&](const QString &name) {
            const auto point = positions.value(name);
            return QRectF((point.x() - minX) * 150, (point.y() - minY) * 90, 120, 55);
        };
        painter.setPen(QPen(palette().color(QPalette::Mid), 2));
        for (const auto &screen : m_screens)
            painter.drawLine(rect(screen.name).center(), rect(screen.neighbor).center());
        for (auto it = positions.cbegin(); it != positions.cend(); ++it) {
            const bool selected = it.key() == m_selected;
            painter.setBrush(palette().color(selected ? QPalette::Highlight : QPalette::Base));
            painter.setPen(palette().color(QPalette::Mid));
            painter.drawRoundedRect(rect(it.key()), 4, 4);
            painter.setPen(palette().color(selected ? QPalette::HighlightedText : QPalette::Text));
            painter.drawText(rect(it.key()).adjusted(6, 0, -6, 0), Qt::AlignCenter,
                             painter.fontMetrics().elidedText(it.key(), Qt::ElideRight, 108));
        }
    }
private:
    QString m_computer, m_selected;
    QList<ScreenLink> m_screens;
};
