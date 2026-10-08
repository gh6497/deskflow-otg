// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QStringList>
#include <functional>

using CommandResult = std::function<void(bool, const QString &)>;
void runCommand(QObject *owner, const QString &program, const QStringList &args,
                CommandResult done, int timeoutMs = 10000);
QString findAdb();
QString findDeskflow();
QString bridgePath();
