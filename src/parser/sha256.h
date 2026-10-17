/****************************************************************************
**
**  This file is a part of Fahrplan.
**
**  This program is free software; you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation; either version 2 of the License, or
**  (at your option) any later version.
**
****************************************************************************/

#ifndef FAHRPLAN_SHA256_H
#define FAHRPLAN_SHA256_H

#include <QByteArray>

// Self-contained SHA-256 and HMAC-SHA256. Qt 4.7's QCryptographicHash knows
// only MD5/SHA1 (SHA256 arrived in Qt 5), and Harmattan's several OpenSSL
// builds are touchy to link against -- so this is done here with no outside
// dependency. Needed for the 9292 backend, whose x-api-token is
// HMAC-SHA256(key, correlationId + userId + path).
QByteArray fahrplanSha256(const QByteArray &data);
QByteArray fahrplanHmacSha256(const QByteArray &key, const QByteArray &message);

#endif // FAHRPLAN_SHA256_H
