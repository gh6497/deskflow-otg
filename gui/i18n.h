// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <QTranslator>
#include <QtTranslation>

// Both ID-based catalogs are embedded; English remains installed as fallback.
class LanguageManager {
public:
    static LanguageManager &instance();
    static QString systemLanguage();
    QString language() const { return m_language; }
    bool setLanguage(const QString &language);
private:
    QTranslator m_english, m_chinese;
    QString m_language;
};
