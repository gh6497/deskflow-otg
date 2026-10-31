// SPDX-License-Identifier: Apache-2.0
#pragma once

class QApplication;

// Follow the desktop color scheme even when a bundled Qt runtime does not
// contain the host platform-theme plugin.
void setupSystemTheme(QApplication &application);

// Re-apply the palette after changing QStyleHints::colorScheme. This is
// exposed for the widget regression test as well as the startup hook.
void syncSystemThemePalette(QApplication &application);
