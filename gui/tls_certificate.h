// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>

namespace TlsCertificate {

bool generate(const QString &path, int keyLength, QString *error = nullptr);
QString fingerprint(const QString &path);
int keyLength(const QString &path);

} // namespace TlsCertificate
