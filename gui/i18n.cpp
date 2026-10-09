// SPDX-License-Identifier: Apache-2.0
#include "i18n.h"
#include <QCoreApplication>
#include <QLocale>

LanguageManager &LanguageManager::instance()
{
    static const bool resourcesReady = [] {
        Q_INIT_RESOURCE(otg_translations);
        return true;
    }();
    Q_UNUSED(resourcesReady);
    static LanguageManager manager;
    return manager;
}

QString LanguageManager::systemLanguage()
{
    return QLocale::system().language() == QLocale::Chinese ? "zh_CN" : "en";
}

bool LanguageManager::setLanguage(const QString &language)
{
    if (language != "zh_CN" && language != "en")
        return false;
    if (language == m_language)
        return true;
    if (m_english.isEmpty() && !m_english.load(":/i18n/deskflow_otg_en.qm"))
        return false;
    if (language == "zh_CN" && m_chinese.isEmpty() &&
        !m_chinese.load(":/i18n/deskflow_otg_zh_CN.qm"))
        return false;
    if (m_language.isEmpty())
        QCoreApplication::installTranslator(&m_english);
    QCoreApplication::removeTranslator(&m_chinese);
    m_language = language;
    if (language == "zh_CN")
        QCoreApplication::installTranslator(&m_chinese);
    return true;
}
