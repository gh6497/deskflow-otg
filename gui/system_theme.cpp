// SPDX-License-Identifier: Apache-2.0
#include "system_theme.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleHints>
#include <QTimer>

namespace {

QPalette darkPalette()
{
    QPalette palette;
    const auto set = [&palette](QPalette::ColorRole role, const QColor &color) {
        palette.setColor(QPalette::All, role, color);
    };

    set(QPalette::Window, QColor(51, 51, 57));
    set(QPalette::WindowText, QColor(235, 235, 235));
    set(QPalette::Base, QColor(36, 36, 41));
    set(QPalette::AlternateBase, QColor(51, 51, 57));
    set(QPalette::ToolTipBase, QColor(255, 255, 255));
    set(QPalette::ToolTipText, QColor(30, 30, 30));
    set(QPalette::Text, QColor(235, 235, 235));
    set(QPalette::Button, QColor(62, 62, 68));
    set(QPalette::ButtonText, QColor(235, 235, 235));
    set(QPalette::BrightText, QColor(255, 80, 80));
    set(QPalette::Link, QColor(66, 165, 245));
    set(QPalette::Highlight, QColor(48, 140, 198));
    set(QPalette::HighlightedText, QColor(255, 255, 255));
    set(QPalette::PlaceholderText, QColor(155, 155, 165));
    set(QPalette::Light, QColor(91, 91, 98));
    set(QPalette::Midlight, QColor(76, 76, 83));
    set(QPalette::Mid, QColor(105, 105, 112));
    set(QPalette::Dark, QColor(30, 30, 34));
    set(QPalette::Shadow, QColor(15, 15, 17));

    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(145, 145, 150));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(145, 145, 150));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(145, 145, 150));
    palette.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor(115, 115, 120));
    return palette;
}

bool isDark(const QPalette &palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

} // namespace

void syncSystemThemePalette(QApplication &application)
{
    const bool dark = application.styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    if (dark) {
        if (!isDark(application.palette()))
            application.setPalette(darkPalette());
        return;
    }

    // Remove a palette installed by the dark-mode fallback and let Qt restore
    // the platform palette for light mode.
    if (isDark(application.palette()))
        application.setPalette(QPalette());
}

void setupSystemTheme(QApplication &application)
{
    auto *hints = application.styleHints();
    hints->setColorScheme(Qt::ColorScheme::Unknown);
    syncSystemThemePalette(application);

    QObject::connect(hints, &QStyleHints::colorSchemeChanged, &application,
        [&application] {
            // Qt updates its platform palette after this signal. Defer the
            // fallback check so we do not overwrite that palette needlessly.
            QTimer::singleShot(0, &application,
                [&application] { syncSystemThemePalette(application); });
        });
}
