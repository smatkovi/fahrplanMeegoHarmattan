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

#include <QDebug>
#include <QNetworkReply>
#include <QRegExp>
#include <QStringList>
#include <QUrl>
#if defined(BUILD_FOR_QT5)
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>
#else
#include <QScriptEngine>
#endif

#include "parser_transitous.h"

// The lists here are walked with counting loops, not with Q_FOREACH as
// upstream does: with C++11 a newer GCC runs a Q_FOREACH body exactly once
// -- silently, so ten stops look like one -- and the GCC 4.4 of MADDE has no
// range-based for. A counting loop is right under both.
//
// Transitous (MOTIS 2 API) is a worldwide GTFS-RT based journey planner,
// see https://transitous.org. Its usage policy requires a User-Agent
// containing the application name and a way of contacting us.
// https://api.transitous.org/api/v1/geocode?text=Berlin fetches stations
static const QString BASE_URL = "https://api.transitous.org/api/v6/";
static const QString GEOCODE_URL = "https://api.transitous.org/api/v1/geocode";
static const int STOP_SEARCH_RADIUS_M = 100;
static const int DEPARTURES_COUNT = 30;

// Timestamps arrive as UTC with a trailing marker ("2026-10-16T10:14:00Z").
// Qt 4.7 reads date and time of such a string but ignores the marker, so the
// stamp would be taken for local time and every departure shown an hour or
// two off -- without anything looking wrong. Therefore the zone is applied
// here, and the result is always a UTC datetime the callers convert.
static QDateTime parseIsoUtc(const QString &stamp)
{
    const QString s = stamp.trimmed();
    if (s.isEmpty())
        return QDateTime();

    QRegExp re("^(\\d{4})-(\\d{2})-(\\d{2})[T ](\\d{2}):(\\d{2})(?::(\\d{2}))?"
               "(?:\\.\\d+)?(Z|[+-]\\d{2}:?\\d{2})?$");
    if (re.indexIn(s) < 0) {
        // Something unexpected: let Qt have a go at it rather than dropping
        // the value entirely.
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

ParserTransitous::ParserTransitous(QObject *parent)
    : ParserAbstract(parent),
      m_timetableRestriction(0),
      m_journeyRestriction(0)
{
    userAgent = "Fahrplan2/2.0 (MeeGo Harmattan; +https://github.com/smatkovi/fahrplanMeegoHarmattan)";
    // A journey plan is a few hundred kilobytes of JSON, most of it the
    // geometry of the legs. Over 3G that is worth compressing; Qt 4.7 does
    // not unpack it by itself, replyData() does.
    acceptEncoding = "gzip";
}

bool ParserTransitous::supportsGps()
{
    return true;
}

bool ParserTransitous::supportsVia()
{
    return true;
}

bool ParserTransitous::supportsTimeTable()
{
    return true;
}

bool ParserTransitous::supportsTimeTableDirection()
{
    return false;
}

QStringList ParserTransitous::getTrainRestrictions()
{
    QStringList result;
    result.append(tr("All"));
    result.append(tr("Bus"));
    result.append(tr("Tram"));
    result.append(tr("Metro"));
    result.append(tr("Ferry"));
    result.append(tr("Train"));
    return result;
}

bool ParserTransitous::isTransitMode(const QString &mode)
{
    return mode != "WALK" && mode != "CYCLE";
}

bool ParserTransitous::modeMatchesRestriction(const QString &mode, int restriction)
{
    switch (restriction) {
    case 1: // Bus
        return mode == "BUS" || mode == "COACH";
    case 2: // Tram
        return mode == "TRAM" || mode == "CABLE_CAR" || mode == "FUNICULAR";
    case 3: // Metro
        return mode == "SUBWAY" || mode == "METRO";
    case 4: // Ferry
        return mode == "FERRY";
    case 5: // Train
        return mode.endsWith("RAIL") || mode == "LONG_DISTANCE" || mode == "SUBURBAN";
    default:
        return true;
    }
}

bool ParserTransitous::itineraryMatchesRestriction(const QVariantMap &itinerary, int restriction)
{
    if (restriction == 0) {
        return true;
    }

    QVariantList legs = itinerary.value("legs").toList();
    for (int i = 0; i < legs.size(); ++i) {
        QVariantMap leg = legs.at(i).toMap();
        QString mode = leg.value("mode").toString();
        if (isTransitMode(mode) && modeMatchesRestriction(mode, restriction)) {
            return true;
        }
    }
    return false;
}

QDateTime ParserTransitous::placeTime(const QVariantMap &place, bool departure)
{
    const QString scheduledKey = departure ? "scheduledDeparture" : "scheduledArrival";
    const QString realtimeKey = departure ? "departure" : "arrival";

    QDateTime dt = parseIsoUtc(place.value(scheduledKey).toString());
    if (!dt.isValid()) {
        dt = parseIsoUtc(place.value(realtimeKey).toString());
    }
    if (dt.isValid()) {
        dt = dt.toLocalTime();
    }
    return dt;
}

QString ParserTransitous::placeName(const QVariantMap &place)
{
    QString name = place.value("name").toString();
    if (name.isEmpty()) {
        name = place.value("description").toString();
    }
    if (name.isEmpty()) {
        name = tr("Unknown stop");
    }
    return name;
}

QString ParserTransitous::lineName(const QVariantMap &leg)
{
    QString name = leg.value("routeShortName").toString();
    if (name.isEmpty()) {
        name = leg.value("displayName").toString();
    }
    if (name.isEmpty()) {
        name = leg.value("tripShortName").toString();
    }
    return name;
}

QString ParserTransitous::coordString(const Station &station)
{
    return QString::number(station.latitude, 'f', 6) + "," +
           QString::number(station.longitude, 'f', 6);
}

// Where a stop lies. The geocoder does not answer with a country field; it
// sends the administrative areas around the stop, the state or province being
// level 4 and the country level 2. Without this a search for "Hauptbahnhof"
// is a list of identical names.
QString ParserTransitous::areaInfo(const QVariantList &areas)
{
    QString region;
    QString country;

    for (int j = 0; j < areas.size(); ++j) {
        QVariantMap area = areas.at(j).toMap();
        const int level = area.value("adminLevel").toDouble();
        const QString name = area.value("name").toString();
        if (name.isEmpty()) {
            continue;
        }
        if (level == 2 && country.isEmpty()) {
            country = name;
        } else if (level == 4 && region.isEmpty()) {
            region = name;
        }
    }

    if (!region.isEmpty() && !country.isEmpty() && region != country) {
        return region + ", " + country;
    }
    return region.isEmpty() ? country : region;
}

QVariantList ParserTransitous::parseJsonList(const QByteArray &json) const
{
    QVariantList doc;
#ifdef BUILD_FOR_QT5
    doc = QJsonDocument::fromJson(json).toVariant().toList();
#else
    QString utf8(QString::fromUtf8(json));

    // Validation of JSON according to RFC 4627, section 6
    QString tmp(utf8);
    if (tmp.replace(QRegExp("\"(\\\\.|[^\"\\\\])*\""), "")
           .contains(QRegExp("[^,:{}\\[\\]0-9.\\-+Eaeflnr-u \\n\\r\\t]")))
        return doc;

    QScriptEngine *engine = new QScriptEngine();
    doc = engine->evaluate("(" + utf8 + ")").toVariant().toList();
    delete engine;
#endif

    return doc;
}

// A request that never arrived leaves an empty body, and an empty body read
// as an answer says "nothing found" -- which sends the user looking for a
// stop that does exist. On a device whose wifi comes and goes that is the
// common case, so it gets its own message.
bool ParserTransitous::replyFailed(QNetworkReply *networkReply)
{
    if (networkReply->error() == QNetworkReply::NoError) {
        return false;
    }
    qDebug() << "Transitous: request failed:" << networkReply->url().toString()
             << networkReply->errorString();
    if (networkReply->error() == QNetworkReply::OperationCanceledError) {
        // We aborted it ourselves, or the user left the page; the next
        // search is already on its way.
        return true;
    }
    emit errorOccured(tr("The server could not be reached: %1").arg(networkReply->errorString()));
    return true;
}

QByteArray ParserTransitous::replyData(QNetworkReply *networkReply)
{
    QByteArray data(networkReply->readAll());
    if (networkReply->rawHeader("Content-Encoding") == "gzip") {
        data = gzipDecompress(data);
    }
    return data;
}

QUrl ParserTransitous::planUrl(const Station &from, const Station &via, const Station &to,
                               const QDateTime &dateTime, const QString &pageCursor) const
{
    QUrl url(BASE_URL + "plan");
    // Qt 4 keeps the query in the URL itself, Qt 5 wants it built separately.
#if defined(BUILD_FOR_QT5)
    QUrlQuery query;
#else
    QUrl &query = url;
#endif
    query.addQueryItem("detailedTransfers", "false");
    query.addQueryItem("fromPlace", coordString(from));
    query.addQueryItem("toPlace", coordString(to));
    if (via.valid && via.latitude != 0.0 && via.longitude != 0.0) {
        query.addQueryItem("viaPlace", coordString(via));
    }
    if (!dateTime.isNull()) {
        query.addQueryItem("time", dateTime.toUTC().toString(Qt::ISODate));
    }
    if (!pageCursor.isEmpty()) {
        query.addQueryItem("pageCursor", pageCursor);
    }
#if defined(BUILD_FOR_QT5)
    url.setQuery(query);
#endif
    return url;
}

QUrl ParserTransitous::stoptimesUrl(const Station &station) const
{
    QUrl url(BASE_URL + "stoptimes");
#if defined(BUILD_FOR_QT5)
    QUrlQuery query;
#else
    QUrl &query = url;
#endif
    query.addQueryItem("fetchStops", "true");
    query.addQueryItem("center", coordString(station));
    query.addQueryItem("radius", QString::number(STOP_SEARCH_RADIUS_M));
    query.addQueryItem("n", QString::number(DEPARTURES_COUNT));
#if defined(BUILD_FOR_QT5)
    url.setQuery(query);
#endif
    return url;
}

QUrl ParserTransitous::stopsUrl(const Station &station) const
{
    QUrl url(BASE_URL + "stops");
#if defined(BUILD_FOR_QT5)
    QUrlQuery query;
#else
    QUrl &query = url;
#endif
    query.addQueryItem("center", coordString(station));
    query.addQueryItem("radius", QString::number(STOP_SEARCH_RADIUS_M));
#if defined(BUILD_FOR_QT5)
    url.setQuery(query);
#endif
    return url;
}

QUrl ParserTransitous::geocodeUrl(const QString &text) const
{
    QUrl url(GEOCODE_URL);
#if defined(BUILD_FOR_QT5)
    QUrlQuery query;
#else
    QUrl &query = url;
#endif
    query.addQueryItem("text", text);
#if defined(BUILD_FOR_QT5)
    url.setQuery(query);
#endif
    return url;
}

QString ParserTransitous::stationTypeFromModes(const QVariantList &modes)
{
    bool hasTrain = false;
    bool hasTram = false;
    bool hasFerry = false;
    for (int k = 0; k < modes.size(); ++k) {
        QString mode = modes.at(k).toString();
        if (mode.endsWith("RAIL") || mode == "LONG_DISTANCE" || mode == "SUBURBAN"
            || mode == "SUBWAY" || mode == "METRO" || mode == "MONORAIL") {
            hasTrain = true;
        } else if (mode == "TRAM" || mode == "CABLE_CAR" || mode == "FUNICULAR") {
            hasTram = true;
        } else if (mode == "FERRY") {
            hasFerry = true;
        }
    }
    if (hasTrain) {
        return "train";
    } else if (hasTram) {
        return "tram";
    } else if (hasFerry) {
        return "ferry";
    }
    return "bus";
}

void ParserTransitous::findStationsByName(const QString &stationName)
{
    if (stationName.trimmed().isEmpty()) {
        return;
    }

    // The station page searches while the name is being typed. Dropping the
    // new search because the previous one is still in flight makes the
    // backend look dead -- on a slow connection the first request can still
    // be running when the user has finished typing. So the old one is
    // dropped instead; its reply is then an abort, and an abort of our own
    // is not worth an error message.
    if (currentRequestState == FahrplanNS::stationsByNameRequest) {
        cancelRequest();
    } else if (currentRequestState != FahrplanNS::noneRequest) {
        return;
    }

    currentRequestState = FahrplanNS::stationsByNameRequest;
    sendHttpRequest(geocodeUrl(stationName.trimmed()));
}

void ParserTransitous::findStationsByCoordinates(qreal longitude, qreal latitude)
{
    if (currentRequestState != FahrplanNS::noneRequest) {
        return;
    }

    Station station;
    station.valid = true;
    station.latitude = latitude;
    station.longitude = longitude;
    station.name = tr("Nearest stop");

    currentRequestState = FahrplanNS::stationsByCoordinatesRequest;
    sendHttpRequest(stopsUrl(station));
}

void ParserTransitous::parseStationsByCoordinates(QNetworkReply *networkReply)
{
    if (replyFailed(networkReply)) {
        return;
    }

    QByteArray allData(replyData(networkReply));
    QVariantMap doc = parseJson(allData);

    QVariantMap place = doc.value("place").toMap();
    if (place.isEmpty()) {
        qDebug() << "Invalid reply:" << allData;
        emit errorOccured(tr("No stop found near your position."));
        return;
    }

    Station station;
    station.valid = true;
    station.id = place.value("stopId");
    station.name = placeName(place);
    station.latitude = place.value("lat").toDouble();
    station.longitude = place.value("lon").toDouble();

    QVariantList modes = place.value("modes").toList();
    station.type = stationTypeFromModes(modes);

    QStringList routeNames;
    QVariantList routes = doc.value("routes").toList();
    for (int l = 0; l < routes.size(); ++l) {
        QString shortName = routes.at(l).toMap().value("routeShortName").toString();
        if (!shortName.isEmpty() && !routeNames.contains(shortName)) {
            routeNames.append(shortName);
        }
    }
    if (!routeNames.isEmpty()) {
        station.miscInfo = routeNames.join(" / ");
    }

    StationsList results;
    results.append(station);
    emit stationsResult(results);
}

void ParserTransitous::parseStationsByName(QNetworkReply *networkReply)
{
    if (replyFailed(networkReply)) {
        return;
    }

    QByteArray allData(replyData(networkReply));
    QVariantList doc = parseJsonList(allData);

    if (doc.isEmpty()) {
        qDebug() << "Invalid reply:" << allData;
        emit errorOccured(tr("No stations found for your search."));
        return;
    }

    StationsList results;
    for (int m = 0; m < doc.size(); ++m) {
        QVariantMap entry = doc.at(m).toMap();

        Station station;
        station.valid = true;
        station.id = entry.value("id");
        station.name = entry.value("name").toString();
        station.latitude = entry.value("lat").toDouble();
        station.longitude = entry.value("lon").toDouble();

        if (entry.value("type").toString() == "STOP") {
            QVariantList modes = entry.value("modes").toList();
            station.type = stationTypeFromModes(modes);
        }

        station.miscInfo = areaInfo(entry.value("areas").toList());

        if (station.name.isEmpty()) {
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

void ParserTransitous::getTimeTableForStation(const Station &currentStation,
                                              const Station &directionStation,
                                              const QDateTime &dateTime,
                                              ParserAbstract::Mode mode,
                                              int trainrestrictions)
{
    Q_UNUSED(directionStation);
    Q_UNUSED(dateTime);
    Q_UNUSED(mode);

    if (currentRequestState != FahrplanNS::noneRequest) {
        cancelRequest();
    }

    if (currentStation.latitude == 0.0 && currentStation.longitude == 0.0) {
        emit errorOccured(tr("This station has no coordinates, so no timetable can be requested."));
        return;
    }

    m_timetableStation = currentStation;
    m_timetableRestriction = trainrestrictions;

    currentRequestState = FahrplanNS::getTimeTableForStationRequest;
    sendHttpRequest(stoptimesUrl(m_timetableStation));
}

void ParserTransitous::parseTimeTable(QNetworkReply *networkReply)
{
    if (replyFailed(networkReply)) {
        return;
    }

    TimetableEntriesList timetable;

    QByteArray allData(replyData(networkReply));
    QVariantMap doc = parseJson(allData);

    QVariantList stopTimes = doc.value("stopTimes").toList();
    if (stopTimes.isEmpty()) {
        qDebug() << "Invalid or empty reply:" << allData;
        emit errorOccured(tr("No departures found for this station."));
        return;
    }

    for (int n = 0; n < stopTimes.size(); ++n) {
        QVariantMap st = stopTimes.at(n).toMap();
        QString mode = st.value("mode").toString();

        if (!modeMatchesRestriction(mode, m_timetableRestriction)) {
            continue;
        }

        QVariantMap place = st.value("place").toMap();
        if (place.isEmpty()) {
            continue;
        }

        QDateTime scheduled = placeTime(place, true);
        if (!scheduled.isValid()) {
            continue;
        }

        TimetableEntry entry;
        entry.currentStation = m_timetableStation.name;

        QString destination = st.value("headsign").toString();
        if (destination.isEmpty()) {
            destination = placeName(st.value("tripTo").toMap());
        }
        entry.destinationStation = destination;

        QString line = st.value("routeShortName").toString();
        if (line.isEmpty()) {
            line = st.value("displayName").toString();
        }
        entry.trainType = line;

        QString platform = place.value("track").toString();
        if (platform.isEmpty()) {
            platform = place.value("scheduledTrack").toString();
        }
        entry.platform = platform;

        entry.time = scheduled.time();
        entry.latitude = place.value("lat").toDouble();
        entry.longitude = place.value("lon").toDouble();

        // Delay / early information
        QString misc;
        bool cancelled = st.value("cancelled").toBool()
            || st.value("tripCancelled").toBool()
            || place.value("cancelled").toBool();
        if (st.value("realTime").toBool()) {
            QDateTime scheduledUtc = parseIsoUtc(place.value("scheduledDeparture").toString());
            QDateTime realUtc = parseIsoUtc(place.value("departure").toString());
            if (scheduledUtc.isValid() && realUtc.isValid()) {
                int diff = scheduledUtc.secsTo(realUtc);
                int minutes = qAbs(diff) / 60;
                if (minutes >= 1) {
                    misc = (diff > 0 ? tr("Delay: %1 min") : tr("Early: %1 min")).arg(minutes);
                } else if (diff != 0) {
                    misc = diff > 0 ? tr("Delayed by less than a minute")
                                   : tr("Early by less than a minute");
                }
            }
        }
        if (cancelled) {
            misc = tr("Cancelled") + (misc.isEmpty() ? "" : QString("; ") + misc);
        }
        entry.miscInfo = misc;

        timetable.append(entry);
    }

    if (timetable.isEmpty()) {
        emit errorOccured(tr("No departures found for this station."));
        return;
    }

    emit timetableResult(timetable);
}

void ParserTransitous::searchJourney(const Station &departureStation,
                                     const Station &viaStation,
                                     const Station &arrivalStation,
                                     const QDateTime &dateTime,
                                     ParserAbstract::Mode mode,
                                     int trainrestrictions)
{
    Q_UNUSED(mode);

    // A journey search is something the user just asked for, so it takes
    // precedence over whatever is still in flight -- typically the station
    // search of the last keystroke, which would otherwise swallow it.
    if (currentRequestState != FahrplanNS::noneRequest) {
        cancelRequest();
    }

    // The service is asked for coordinates, not for station ids, so a stop
    // that carries none -- a favourite saved while another backend was
    // selected, for instance -- would be sent as 0,0 and answered with a
    // journey from the Atlantic, or with nothing at all.
    if ((departureStation.latitude == 0.0 && departureStation.longitude == 0.0)
            || (arrivalStation.latitude == 0.0 && arrivalStation.longitude == 0.0)) {
        qDebug() << "Transitous: stop without coordinates:"
                 << departureStation.name << departureStation.latitude << departureStation.longitude
                 << "->" << arrivalStation.name << arrivalStation.latitude << arrivalStation.longitude;
        emit errorOccured(tr("This stop has no coordinates. Please search for it again."));
        return;
    }

    m_journeyFrom = departureStation;
    m_journeyVia = viaStation;
    m_journeyTo = arrivalStation;
    m_journeyWhen = dateTime;
    m_journeyRestriction = trainrestrictions;
    m_nextPageCursor.clear();
    m_previousPageCursor.clear();

    while (!m_details.isEmpty()) {
        delete m_details.takeFirst();
    }

    currentRequestState = FahrplanNS::searchJourneyRequest;
    const QUrl url = planUrl(m_journeyFrom, m_journeyVia, m_journeyTo, m_journeyWhen, QString());
    qDebug() << "Transitous: journey search" << url.toString();
    sendHttpRequest(url);
}

void ParserTransitous::searchJourneyLater()
{
    if (currentRequestState != FahrplanNS::noneRequest) {
        return;
    }

    if (m_nextPageCursor.isEmpty()) {
        emit errorOccured(tr("No later journeys available."));
        return;
    }

    currentRequestState = FahrplanNS::searchJourneyLaterRequest;
    sendHttpRequest(planUrl(m_journeyFrom, m_journeyVia, m_journeyTo, QDateTime(), m_nextPageCursor));
}

void ParserTransitous::searchJourneyEarlier()
{
    if (currentRequestState != FahrplanNS::noneRequest) {
        return;
    }

    if (m_previousPageCursor.isEmpty()) {
        emit errorOccured(tr("No earlier journeys available."));
        return;
    }

    currentRequestState = FahrplanNS::searchJourneyEarlierRequest;
    sendHttpRequest(planUrl(m_journeyFrom, m_journeyVia, m_journeyTo, QDateTime(), m_previousPageCursor));
}

void ParserTransitous::handleJourneyReply(QNetworkReply *networkReply, bool isInitialSearch)
{
    if (replyFailed(networkReply)) {
        return;
    }

    QByteArray allData(replyData(networkReply));
    QVariantMap doc = parseJson(allData);

    QVariantList itineraries = doc.value("itineraries").toList();
    if (doc.isEmpty() && itineraries.isEmpty()) {
        qDebug() << "Invalid reply:" << allData;
        emit errorOccured(tr("Cannot parse reply from the server"));
        return;
    }

    m_nextPageCursor = doc.value("nextPageCursor").toString();
    m_previousPageCursor = doc.value("previousPageCursor").toString();

    if (isInitialSearch) {
        while (!m_details.isEmpty()) {
            delete m_details.takeFirst();
        }
    }

    JourneyResultList *result = new JourneyResultList();
    result->setDepartureStation(m_journeyFrom.name);
    result->setArrivalStation(m_journeyTo.name);
    if (m_journeyVia.valid) {
        result->setViaStation(m_journeyVia.name);
    }
    result->setTimeInfo(tr("Journeys from %1").arg(
        m_journeyWhen.toLocalTime().toString(tr("ddd MMM d, HH:mm"))));

    for (int o = 0; o < itineraries.size(); ++o) {
        QVariantMap itinerary = itineraries.at(o).toMap();
        QVariantList legs = itinerary.value("legs").toList();
        if (legs.isEmpty()) {
            continue;
        }

        if (!itineraryMatchesRestriction(itinerary, m_journeyRestriction)) {
            continue;
        }

        QVariantMap firstLeg = legs.first().toMap();
        QVariantMap lastLeg = legs.last().toMap();

        QDateTime firstDeparture = placeTime(firstLeg.value("from").toMap(), true);
        QDateTime lastArrival = placeTime(lastLeg.value("to").toMap(), false);
        if (!firstDeparture.isValid() || !lastArrival.isValid()) {
            continue;
        }

        // Summary of the journey
        JourneyResultItem *item = new JourneyResultItem();
        item->setId(QString::number(m_details.size()));
        item->setDate(firstDeparture.date());
        item->setDepartureTime(firstDeparture.toString("HH:mm"));
        item->setArrivalTime(lastArrival.toString("HH:mm"));

        int totalSeconds = qAbs(firstDeparture.secsTo(lastArrival));
        item->setDuration(QString("%1:%2")
            .arg(totalSeconds / 3600)
            .arg((totalSeconds % 3600) / 60, 2, 10, QChar('0')));

        QStringList lineNames;
        int transitLegs = 0;
        for (int p = 0; p < legs.size(); ++p) {
            QVariantMap leg = legs.at(p).toMap();
            QString mode = leg.value("mode").toString();
            if (isTransitMode(mode)) {
                transitLegs++;
                QString line = lineName(leg);
                if (!line.isEmpty() && !lineNames.contains(line)) {
                    lineNames.append(line);
                }
            }
        }
        item->setTrainType(lineNames.join(" / "));

        int transfers = qMax(0, transitLegs - 1);
        if (transfers > 0) {
            item->setTransfers(QString::number(transfers));
        }

        result->appendItem(item);

        // Details: one item per transit leg
        JourneyDetailResultList *details = new JourneyDetailResultList();
        details->setId(item->id());
        details->setDepartureStation(m_journeyFrom.name);
        details->setArrivalStation(m_journeyTo.name);
        details->setDepartureDateTime(firstDeparture);
        details->setArrivalDateTime(lastArrival);
        details->setDuration(item->duration());
        if (transfers > 0) {
            details->setInfo(tr("%1 transfers").arg(transfers));
        }

        for (int q = 0; q < legs.size(); ++q) {
            QVariantMap leg = legs.at(q).toMap();
            QString mode = leg.value("mode").toString();
            if (!isTransitMode(mode)) {
                continue;
            }

            QVariantMap from = leg.value("from").toMap();
            QVariantMap to = leg.value("to").toMap();

            QDateTime legDeparture = placeTime(from, true);
            QDateTime legArrival = placeTime(to, false);

            JourneyDetailResultItem *segment = new JourneyDetailResultItem();
            segment->setDepartureStation(placeName(from));
            segment->setDepartureInfo(from.value("track").toString());
            segment->setDepartureDateTime(legDeparture);
            segment->setArrivalStation(placeName(to));
            segment->setArrivalInfo(to.value("track").toString());
            segment->setArrivalDateTime(legArrival);
            segment->setTrain(lineName(leg));
            segment->setDirection(leg.value("headsign").toString());

            QVariantList intermediateStops = leg.value("intermediateStops").toList();
            if (!intermediateStops.isEmpty()) {
                segment->setInfo(tr("%1 intermediate stops").arg(intermediateStops.size()));
            }

            details->appendItem(segment);
        }

        if (!details->itemcount()) {
            delete details;
        } else {
            m_details.append(details);
        }
    }

    if (result->itemcount() == 0) {
        delete result;
        qDebug() << "Transitous: no journey left after filtering, itineraries:"
                 << itineraries.size() << "restriction:" << m_journeyRestriction;
        emit errorOccured(tr("No journeys found."));
        return;
    }

    emit journeyResult(result);
}

void ParserTransitous::parseSearchJourney(QNetworkReply *networkReply)
{
    handleJourneyReply(networkReply, true);
}

void ParserTransitous::parseSearchLaterJourney(QNetworkReply *networkReply)
{
    handleJourneyReply(networkReply, false);
}

void ParserTransitous::parseSearchEarlierJourney(QNetworkReply *networkReply)
{
    handleJourneyReply(networkReply, false);
}

void ParserTransitous::parseJourneyDetails(QNetworkReply *networkReply)
{
    Q_UNUSED(networkReply);
    // Journey details are available from the journey search response,
    // this slot is therefore not used by this parser.
}

void ParserTransitous::getJourneyDetails(const QString &id)
{
    int index = id.toInt();
    if (index >= 0 && index < m_details.size()) {
        emit journeyDetailsResult(m_details[index]);
    } else {
        emit errorOccured(tr("No journey details found."));
    }
}
