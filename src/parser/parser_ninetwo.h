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

#ifndef PARSER_NINETWO_H
#define PARSER_NINETWO_H

#include <QList>
#include <QPair>

#include "parser_abstract.h"

// 9292 (9292.nl), the Dutch nationwide public-transport planner. This is a
// fresh client against the app host wrapper.9292.nl, not a port of the old
// parser that spoke to the now-closed web-api.9292.nl. Every request carries
// an x-api-token = HMAC-SHA256(key, correlationId + userId + path); the key
// was recovered from the app's libkeylib_flavor.so and the recipe measured
// against the real traffic. See the memory note "9292-Schnittstelle".
class ParserNineTwo : public ParserAbstract
{
    Q_OBJECT
public:
    explicit ParserNineTwo(QObject *parent = 0);

    static QString getName()
    {
        return "Netherlands (9292)";
    }
    virtual QString name() { return getName(); }
    virtual QString shortName() { return "9292.nl"; }

public slots:
    virtual bool supportsGps();
    virtual bool supportsVia();
    virtual bool supportsTimeTable();
    virtual bool supportsTimeTableDirection();
    virtual QStringList getTrainRestrictions();

    virtual void findStationsByName(const QString &stationName);
    virtual void searchJourney(const Station &departureStation,
                               const Station &viaStation,
                               const Station &arrivalStation,
                               const QDateTime &dateTime,
                               ParserAbstract::Mode mode,
                               int trainrestrictions);
    virtual void searchJourneyLater();
    virtual void searchJourneyEarlier();
    virtual void getJourneyDetails(const QString &id);

protected:
    virtual void parseStationsByName(QNetworkReply *networkReply);
    virtual void parseSearchJourney(QNetworkReply *networkReply);
    virtual void parseSearchLaterJourney(QNetworkReply *networkReply);
    virtual void parseSearchEarlierJourney(QNetworkReply *networkReply);

private:
    // The whole path after the host, exactly as it goes on the wire
    // (percent-encoded) -- it is both requested and signed, so the two must
    // be the same bytes.
    QString plansPath(const QString &fromId, const QString &toId,
                      const QDateTime &dateTime, ParserAbstract::Mode mode) const;
    QUrl apiUrl(const QString &path) const;
    // Builds the 9292 headers for a path, including the computed
    // x-correlation-id and x-api-token.
    QList<QPair<QByteArray, QByteArray> > signedHeaders(const QString &path);
    void sendSigned(const QString &path);

    QByteArray replyData(QNetworkReply *networkReply);
    bool replyFailed(QNetworkReply *networkReply);
    void handleJourneyReply(QNetworkReply *networkReply, bool isInitialSearch);

    QString m_userId;
    QString m_language;

    QString m_fromId;
    QString m_toId;
    QString m_fromName;
    QString m_toName;
    QDateTime m_journeyWhen;
    ParserAbstract::Mode m_journeyMode;

    QDateTime m_firstDeparture;
    QDateTime m_lastDeparture;

    QList<JourneyDetailResultList *> m_details;
};

#endif // PARSER_NINETWO_H
