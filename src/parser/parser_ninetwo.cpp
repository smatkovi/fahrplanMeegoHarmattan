/****************************************************************************
**
**  This file is a part of Fahrplan.
**
**  This program is free software; you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation; either version 2 of the License, or
**  (at your option) any later version.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License along
**  with this program.  If not, see <https://www.gnu.org/licenses/>.
**
****************************************************************************/

#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QNetworkReply>
#include <QRegExp>
#include <QSettings>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#if defined(BUILD_FOR_QT5)
#include <QJsonDocument>
#else
#include <QScriptEngine>
#endif

#include "parser_ninetwo.h"
#include "sha256.h"

// Lists are walked with counting loops, not Q_FOREACH: under C++11 a newer
// GCC runs a Q_FOREACH body exactly once (silently), and MADDE's GCC 4.4 has
// no range-based for.

static const QString BASE_HOST = "https://wrapper.9292.nl";
static const QString API = "/v2.15";

// apiAccKey from the app's libkeylib_flavor.so, AES-decrypted. The host
// wrapper.9292.nl accepts it (checked against three captured tokens). Not a
// user secret -- a client key baked into every copy of the app.
static const QByteArray API_KEY = "VqXJpLaSEX4nngH6qPce5wfv4mU2kqBP3MFPX7ncuzh6vU";

// Timestamps arrive as UTC with a trailing "Z" ("2026-10-17T08:19:00Z").
// Qt 4.7 reads such a string but ignores the zone marker and takes it for
// local time -- so the zone is applied here and the result is UTC.
static QDateTime parseIsoUtc(const QString &stamp)
{
    const QString s = stamp.trimmed();
    if (s.isEmpty())
        return QDateTime();

    QRegExp re("^(\\d{4})-(\\d{2})-(\\d{2})[T ](\\d{2}):(\\d{2})(?::(\\d{2}))?"
               "(?:\\.\\d+)?(Z|[+-]\\d{2}:?\\d{2})?$");
    if (re.indexIn(s) < 0) {
        return QDateTime::fromString(s, Qt::ISODate).toUTC();
    }

    const QDate date(re.cap(1).toInt(), re.cap(2).toInt(), re.cap(3).toInt());
    const QTime time(re.cap(4).toInt(), re.cap(5).toInt(), re.cap(6).toInt());
    if (!date.isValid() || !time.isValid())
        return QDateTime();

    QDateTime dt(date, time, Qt::UTC);

    const QString zone = re.cap(7);
    if (!zone.isEmpty() && zone != QLatin1String("Z")) {
        QString digits = zone.mid(1);
        digits.remove(QLatin1Char(':'));
        const int offset = digits.left(2).toInt() * 3600 + digits.right(2).toInt() * 60;
        dt = dt.addSecs(zone.startsWith(QLatin1Char('-')) ? offset : -offset);
    }

    return dt;
}

// The "time" and "platform" fields of a stop are objects {planned, actual};
// the realtime value wins, the planned one is the fallback.
static QString preferActual(const QVariant &field)
{
    QVariantMap m = field.toMap();
    QString v = m.value("actual").toString();
    if (v.isEmpty())
        v = m.value("planned").toString();
    return v;
}

static QDateTime stopTime(const QVariant &timeField)
{
    QDateTime dt = parseIsoUtc(preferActual(timeField));
    if (dt.isValid())
        dt = dt.toLocalTime();
    return dt;
}

static QString stopPlatform(const QVariant &platformField)
{
    QString p = preferActual(platformField);
    // The value reads "Platform 7" / "Spoor 7"; show just the track.
    int sp = p.indexOf(QLatin1Char(' '));
    if (sp > 0 && sp < p.length() - 1) {
        p = p.mid(sp + 1);
    }
    return p;
}

static QString pct(const QString &s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

ParserNineTwo::ParserNineTwo(QObject *parent)
    : ParserAbstract(parent),
      m_journeyMode(ParserAbstract::Departure)
{
    userAgent = "okhttp/5.3.2";
    acceptEncoding = "gzip";

    qsrand(uint(QDateTime::currentDateTime().toTime_t()) ^ uint(reinterpret_cast<quintptr>(this)));

    // The server expects a stable per-install id in x-user-id and signs it
    // into every token. We mint one on first use and keep it.
    QSettings settings(FAHRPLAN_SETTINGS_NAMESPACE, "fahrplan2");
    m_userId = settings.value("ninetwo/userId").toString();
    if (m_userId.isEmpty()) {
        QString uuid = QUuid::createUuid().toString();
        uuid.remove(QLatin1Char('{'));
        uuid.remove(QLatin1Char('}'));
        m_userId = uuid;
        settings.setValue("ninetwo/userId", m_userId);
    }

    m_language = QLocale::system().name().left(2);
    if (m_language.isEmpty()) {
        m_language = "en";
    }
}

bool ParserNineTwo::supportsGps()
{
    return false;
}

bool ParserNineTwo::supportsVia()
{
    return false;
}

bool ParserNineTwo::supportsTimeTable()
{
    return false;
}

bool ParserNineTwo::supportsTimeTableDirection()
{
    return false;
}

QStringList ParserNineTwo::getTrainRestrictions()
{
    // The planner has no server-side modality filter we rely on, and a wrong
    // local filter would hide good journeys; so only "All".
    QStringList result;
    result.append(tr("All"));
    return result;
}

QUrl ParserNineTwo::apiUrl(const QString &path) const
{
    // fromEncoded, so Qt does not re-encode the query -- the bytes on the
    // wire must equal the bytes we signed.
    return QUrl::fromEncoded(QString(BASE_HOST + path).toLatin1());
}

QList<QPair<QByteArray, QByteArray> > ParserNineTwo::signedHeaders(const QString &path)
{
    // correlationId: 8 bytes of Unix seconds (big-endian) + 8 random bytes,
    // all as hex -- the shape the app produces.
    const quint64 secs = QDateTime::currentDateTime().toTime_t();
    QString cid = QString("%1").arg(secs, 16, 16, QChar('0'));
    for (int i = 0; i < 8; ++i) {
        cid += QString("%1").arg(qrand() & 0xff, 2, 16, QChar('0'));
    }

    const QByteArray token =
        fahrplanHmacSha256(API_KEY, (cid + m_userId + path).toUtf8()).toHex();

    QList<QPair<QByteArray, QByteArray> > h;
    h.append(qMakePair(QByteArray("x-os"), QByteArray("android")));
    h.append(qMakePair(QByteArray("x-os-version"), QByteArray("15")));
    h.append(qMakePair(QByteArray("x-device-name"), QByteArray("Fahrplan2 (MeeGo Harmattan)")));
    h.append(qMakePair(QByteArray("x-brand"), QByteArray("9292")));
    h.append(qMakePair(QByteArray("x-app-version"), QByteArray("32189")));
    h.append(qMakePair(QByteArray("x-functional-consent"), QByteArray("false")));
    h.append(qMakePair(QByteArray("x-build-flavor"), QByteArray("prod")));
    h.append(qMakePair(QByteArray("x-skip-ads"), QByteArray("false")));
    h.append(qMakePair(QByteArray("accept-language"), m_language.toLatin1()));
    h.append(qMakePair(QByteArray("x-user-id"), m_userId.toLatin1()));
    h.append(qMakePair(QByteArray("x-correlation-id"), cid.toLatin1()));
    h.append(qMakePair(QByteArray("x-api-token"), token));
    return h;
}

void ParserNineTwo::sendSigned(const QString &path)
{
    sendHttpRequest(apiUrl(path), QByteArray(), signedHeaders(path));
}

bool ParserNineTwo::replyFailed(QNetworkReply *networkReply)
{
    if (networkReply->error() == QNetworkReply::NoError) {
        return false;
    }
    qDebug() << "9292: request failed:" << networkReply->url().toString()
             << networkReply->errorString();
    if (networkReply->error() == QNetworkReply::OperationCanceledError) {
        return true;
    }
    emit errorOccured(tr("The server could not be reached: %1").arg(networkReply->errorString()));
    return true;
}

QByteArray ParserNineTwo::replyData(QNetworkReply *networkReply)
{
    QByteArray data(networkReply->readAll());
    if (networkReply->rawHeader("Content-Encoding") == "gzip") {
        data = gzipDecompress(data);
    }
    return data;
}

void ParserNineTwo::findStationsByName(const QString &stationName)
{
    if (stationName.trimmed().isEmpty()) {
        return;
    }

    // Search-as-you-type: drop the previous in-flight search rather than the
    // new one, so a slow first request does not swallow a later keystroke.
    if (currentRequestState == FahrplanNS::stationsByNameRequest) {
        cancelRequest();
    } else if (currentRequestState != FahrplanNS::noneRequest) {
        return;
    }

    currentRequestState = FahrplanNS::stationsByNameRequest;
    sendSigned(API + "/locations?Query=" + pct(stationName.trimmed()));
}

void ParserNineTwo::parseStationsByName(QNetworkReply *networkReply)
{
    if (replyFailed(networkReply)) {
        return;
    }

    QByteArray allData(replyData(networkReply));
    QVariantMap doc = parseJson(allData);

    QVariantList locations = doc.value("locations").toList();
    if (locations.isEmpty()) {
        qDebug() << "9292: no locations in reply:" << allData.left(200);
        emit errorOccured(tr("No stations found for your search."));
        return;
    }

    StationsList results;
    for (int i = 0; i < locations.size(); ++i) {
        QVariantMap entry = locations.at(i).toMap();

        Station station;
        station.valid = true;
        station.id = entry.value("id");
        station.name = entry.value("label").toString();
        if (station.name.isEmpty()) {
            station.name = entry.value("displayLabel").toString();
        }
        station.miscInfo = entry.value("place").toString();

        const QString locationType = entry.value("locationType").toString();
        if (locationType == "Station") {
            station.type = "train";
        } else if (locationType == "Stop") {
            station.type = "bus";
        }

        station.latitude = 0.0;
        station.longitude = 0.0;

        if (station.id.toString().isEmpty() || station.name.isEmpty()) {
            continue;
        }
        results.append(station);
    }

    if (results.isEmpty()) {
        emit errorOccured(tr("No stations found for your search."));
        return;
    }

    emit stationsResult(results);
}

QString ParserNineTwo::plansPath(const QString &fromId, const QString &toId,
                                 const QDateTime &dateTime, ParserAbstract::Mode mode) const
{
    const QString dt = dateTime.toUTC().toString("yyyy-MM-ddThh:mm:ss.zzz") + "Z";
    const QString travel = (mode == ParserAbstract::Arrival) ? "Arrival" : "Departure";

    QString p = API + "/plans?from=" + pct(fromId) + "&to=" + pct(toId)
              + "&dateTime=" + pct(dt)
              + "&travelType=" + travel
              + "&planWithAccessibility=false&extraInterchangeTime=0&limitWalking=false"
              + "&firstMile=Walking&firstMileTime=20&firstMileSpeed=4.2"
              + "&lastMile=Walking&lastMileTime=20&lastMileSpeed=4.2"
              + "&origin=Plan&allowAlternative=true";
    return p;
}

void ParserNineTwo::searchJourney(const Station &departureStation,
                                  const Station &viaStation,
                                  const Station &arrivalStation,
                                  const QDateTime &dateTime,
                                  ParserAbstract::Mode mode,
                                  int trainrestrictions)
{
    Q_UNUSED(viaStation);
    Q_UNUSED(trainrestrictions);

    // A journey search takes precedence over the station search of the last
    // keystroke that may still be in flight.
    if (currentRequestState != FahrplanNS::noneRequest) {
        cancelRequest();
    }

    const QString fromId = departureStation.id.toString();
    const QString toId = arrivalStation.id.toString();
    if (fromId.isEmpty() || toId.isEmpty()) {
        // A favourite saved while another backend was selected carries no
        // 9292 id; it has to be looked up here again.
        emit errorOccured(tr("Please search for the stops again for this provider."));
        return;
    }

    m_fromId = fromId;
    m_toId = toId;
    m_fromName = departureStation.name;
    m_toName = arrivalStation.name;
    m_journeyWhen = dateTime.isValid() ? dateTime : QDateTime::currentDateTime();
    m_journeyMode = mode;
    m_firstDeparture = QDateTime();
    m_lastDeparture = QDateTime();

    while (!m_details.isEmpty()) {
        delete m_details.takeFirst();
    }

    currentRequestState = FahrplanNS::searchJourneyRequest;
    const QString path = plansPath(m_fromId, m_toId, m_journeyWhen, m_journeyMode);
    qDebug() << "9292: journey search" << path;
    sendSigned(path);
}

void ParserNineTwo::searchJourneyLater()
{
    if (currentRequestState != FahrplanNS::noneRequest) {
        return;
    }
    if (!m_lastDeparture.isValid()) {
        emit errorOccured(tr("No later journeys available."));
        return;
    }
    currentRequestState = FahrplanNS::searchJourneyLaterRequest;
    sendSigned(plansPath(m_fromId, m_toId, m_lastDeparture.addSecs(60), ParserAbstract::Departure));
}

void ParserNineTwo::searchJourneyEarlier()
{
    if (currentRequestState != FahrplanNS::noneRequest) {
        return;
    }
    if (!m_firstDeparture.isValid()) {
        emit errorOccured(tr("No earlier journeys available."));
        return;
    }
    currentRequestState = FahrplanNS::searchJourneyEarlierRequest;
    sendSigned(plansPath(m_fromId, m_toId, m_firstDeparture.addSecs(-60), ParserAbstract::Arrival));
}

void ParserNineTwo::handleJourneyReply(QNetworkReply *networkReply, bool isInitialSearch)
{
    if (replyFailed(networkReply)) {
        return;
    }

    QByteArray allData(replyData(networkReply));
    QVariantMap doc = parseJson(allData);

    QVariantList sections = doc.value("sections").toList();
    if (sections.isEmpty()) {
        qDebug() << "9292: no sections in reply:" << allData.left(200);
        emit errorOccured(tr("No journeys found."));
        return;
    }

    if (isInitialSearch) {
        while (!m_details.isEmpty()) {
            delete m_details.takeFirst();
        }
    }

    JourneyResultList *result = new JourneyResultList();
    result->setDepartureStation(m_fromName);
    result->setArrivalStation(m_toName);
    result->setTimeInfo(tr("Journeys from %1").arg(
        m_journeyWhen.toLocalTime().toString(tr("ddd MMM d, HH:mm"))));

    QDateTime firstDep;
    QDateTime lastDep;

    for (int s = 0; s < sections.size(); ++s) {
        QVariantList items = sections.at(s).toMap().value("items").toList();
        for (int it = 0; it < items.size(); ++it) {
            QVariantMap journey = items.at(it).toMap().value("journey").toMap();
            if (journey.isEmpty()) {
                continue;
            }

            QDateTime dep = stopTime(journey.value("departure"));
            QDateTime arr = stopTime(journey.value("arrival"));
            if (!dep.isValid() || !arr.isValid()) {
                continue;
            }

            QVariantList legs = journey.value("legs").toList();

            JourneyResultItem *item = new JourneyResultItem();
            item->setId(QString::number(m_details.size()));
            item->setDate(dep.date());
            item->setDepartureTime(dep.toString("HH:mm"));
            item->setArrivalTime(arr.toString("HH:mm"));

            int durationMin = journey.value("duration").toInt();
            if (durationMin <= 0) {
                durationMin = qAbs(dep.secsTo(arr)) / 60;
            }
            item->setDuration(QString("%1:%2")
                .arg(durationMin / 60)
                .arg(durationMin % 60, 2, 10, QChar('0')));

            const int changes = journey.value("numberOfChanges").toInt();
            if (changes > 0) {
                item->setTransfers(QString::number(changes));
            }

            QStringList lineNames;
            for (int l = 0; l < legs.size(); ++l) {
                QVariantMap leg = legs.at(l).toMap();
                if (leg.value("type").toString() != "Vehicle") {
                    continue;
                }
                QString code = leg.value("modalityCode").toString();
                if (code.isEmpty()) {
                    code = leg.value("modality").toString();
                }
                if (!code.isEmpty() && !lineNames.contains(code)) {
                    lineNames.append(code);
                }
            }
            item->setTrainType(lineNames.join(" / "));

            result->appendItem(item);

            // Details: one segment per non-advertisement leg.
            JourneyDetailResultList *details = new JourneyDetailResultList();
            details->setId(item->id());
            details->setDepartureStation(journey.value("departureFullName").toString());
            details->setArrivalStation(journey.value("arrivalFullName").toString());
            details->setDepartureDateTime(dep);
            details->setArrivalDateTime(arr);
            details->setDuration(item->duration());
            if (changes > 0) {
                details->setInfo(tr("%1 transfers").arg(changes));
            }

            for (int l = 0; l < legs.size(); ++l) {
                QVariantMap leg = legs.at(l).toMap();
                const QString type = leg.value("type").toString();
                if (type == "Advertisement") {
                    continue;
                }

                QVariantMap from = leg.value("from").toMap();
                QVariantMap to = leg.value("to").toMap();

                JourneyDetailResultItem *segment = new JourneyDetailResultItem();
                segment->setDepartureStation(from.value("fullName").toString());
                segment->setDepartureInfo(stopPlatform(from.value("platform")));
                segment->setDepartureDateTime(stopTime(from.value("time")));
                segment->setArrivalStation(to.value("fullName").toString());
                segment->setArrivalInfo(stopPlatform(to.value("platform")));
                segment->setArrivalDateTime(stopTime(to.value("time")));

                if (type == "Vehicle") {
                    QVariantMap description = leg.value("description").toMap();
                    QString train = leg.value("modalityCode").toString();
                    if (train.isEmpty()) {
                        train = description.value("transportModeName").toString();
                    }
                    segment->setTrain(train);
                    segment->setDirection(description.value("serviceDestination").toString());

                    const int intermediates = leg.value("intermediateStops").toList().size();
                    if (intermediates > 0) {
                        segment->setInfo(tr("%1 intermediate stops").arg(intermediates));
                    }
                } else {
                    segment->setTrain(tr("Walk"));
                }

                details->appendItem(segment);
            }

            if (!details->itemcount()) {
                delete details;
            } else {
                m_details.append(details);
            }

            if (!firstDep.isValid() || dep < firstDep) {
                firstDep = dep;
            }
            if (!lastDep.isValid() || dep > lastDep) {
                lastDep = dep;
            }
        }
    }

    if (result->itemcount() == 0) {
        delete result;
        emit errorOccured(tr("No journeys found."));
        return;
    }

    if (firstDep.isValid()) {
        m_firstDeparture = firstDep;
    }
    if (lastDep.isValid()) {
        m_lastDeparture = lastDep;
    }

    emit journeyResult(result);
}

void ParserNineTwo::parseSearchJourney(QNetworkReply *networkReply)
{
    handleJourneyReply(networkReply, true);
}

void ParserNineTwo::parseSearchLaterJourney(QNetworkReply *networkReply)
{
    handleJourneyReply(networkReply, false);
}

void ParserNineTwo::parseSearchEarlierJourney(QNetworkReply *networkReply)
{
    handleJourneyReply(networkReply, false);
}

void ParserNineTwo::getJourneyDetails(const QString &id)
{
    int index = id.toInt();
    if (index >= 0 && index < m_details.size()) {
        emit journeyDetailsResult(m_details[index]);
    } else {
        emit errorOccured(tr("No journey details found."));
    }
}
