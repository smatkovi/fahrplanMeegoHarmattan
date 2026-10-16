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

// Runs the Transitous backend on the device without the user interface:
// one station search, then one journey search between the first two hits.
// It answers the two questions the desktop cannot: does the TLS of
// api.transitous.org work with the libssl of this device at all, and how
// long does Qt 4 need for a reply of a few hundred kilobytes of JSON.
//
//   mad -t <target> qmake tools/transitous-probe.pro && mad make
//   ./transitous-probe "Wien Hbf" "Graz Hbf"
//
// Without a network, or on a machine whose OpenSSL is too new for Qt 4,
// the same parsing runs on saved replies:
//
//   ./transitous-probe --dateien geocode.json plan.json

#include <QtCore>
#include <QtNetwork>

#include "parser/parser_transitous.h"

// Which OpenSSL the process actually ended up with: Qt 4 opens one by
// soname at runtime, and the factory 0.9.8 next to the newer one in
// /usr/local/lib is the difference between TLS 1.0 and TLS 1.2.
static QString loadedSslLibraries()
{
    QFile maps("/proc/self/maps");
    if (!maps.open(QIODevice::ReadOnly))
        return QString("/proc/self/maps nicht lesbar");

    QStringList found;
    while (!maps.atEnd()) {
        const QString line = QString::fromLatin1(maps.readLine());
        const int slash = line.lastIndexOf(QLatin1Char('/'));
        if (slash < 0)
            continue;
        const QString file = line.mid(slash + 1).trimmed();
        if ((file.startsWith("libssl") || file.startsWith("libcrypto"))
                && !found.contains(file)) {
            found.append(file + " <- " + line.mid(line.indexOf(QLatin1Char('/'))).trimmed());
        }
    }
    return found.isEmpty() ? QString("keine geladen") : found.join("; ");
}

// The parse methods are protected in the parser, as they should be; a
// subclass is the honest way to hand them a saved reply.
class ReplayParser : public ParserTransitous
{
public:
    explicit ReplayParser(QObject *parent = 0) : ParserTransitous(parent) {}
    void replayStations(QNetworkReply *reply) { parseStationsByName(reply); }
    void replayJourneys(QNetworkReply *reply) { parseSearchJourney(reply); }
};

class Replay : public QObject
{
    Q_OBJECT
public:
    Replay(const QString &stationsFile, const QString &planFile)
        : m_stationsFile(stationsFile), m_planFile(planFile)
    {
        m_parser = new ReplayParser(this);
        connect(m_parser, SIGNAL(stationsResult(StationsList)),
                this, SLOT(onStations(StationsList)));
        connect(m_parser, SIGNAL(journeyResult(JourneyResultList*)),
                this, SLOT(onJourneys(JourneyResultList*)));
        connect(m_parser, SIGNAL(errorOccured(QString)),
                this, SLOT(onError(QString)));
    }

    void start()
    {
        out("Zeitzone des Rechners: " + QString::fromLatin1(qgetenv("TZ").isEmpty()
                ? QByteArray("(Systemvorgabe)") : qgetenv("TZ")));
        out("Haltestellen aus " + m_stationsFile);
        m_reply = m_nam.get(QNetworkRequest(QUrl::fromLocalFile(m_stationsFile)));
        connect(m_reply, SIGNAL(finished()), this, SLOT(onStationsFile()));
    }

private slots:
    void onStationsFile()
    {
        m_parser->replayStations(m_reply);
        m_reply->deleteLater();
        out("Verbindungen aus " + m_planFile);
        m_reply = m_nam.get(QNetworkRequest(QUrl::fromLocalFile(m_planFile)));
        connect(m_reply, SIGNAL(finished()), this, SLOT(onPlanFile()));
    }

    void onPlanFile()
    {
        m_parser->replayJourneys(m_reply);
        m_reply->deleteLater();
        out("FERTIG");
        QCoreApplication::exit(0);
    }

    void onStations(const StationsList &stations)
    {
        out(QString("  %1 Treffer").arg(stations.count()));
        for (int i = 0; i < stations.count() && i < 4; ++i) {
            const Station &s = stations.at(i);
            out(QString("  [%1] %2 (%3) %4,%5 -- %6")
                .arg(i).arg(s.name).arg(s.type)
                .arg(s.latitude, 0, 'f', 5).arg(s.longitude, 0, 'f', 5).arg(s.miscInfo));
        }
    }

    void onJourneys(JourneyResultList *result)
    {
        out(QString("  %1 Verbindungen").arg(result->itemcount()));
        for (int i = 0; i < result->itemcount() && i < 5; ++i) {
            JourneyResultItem *item = result->getItem(i);
            out(QString("  [%1] %2 ab %3 an %4, %5 h, %6 Umstiege, %7")
                .arg(i).arg(item->date().toString("dd.MM.yyyy"))
                .arg(item->departureTime()).arg(item->arrivalTime())
                .arg(item->duration())
                .arg(item->transfers().isEmpty() ? "0" : item->transfers())
                .arg(item->trainType()));
        }
    }

    void onError(const QString &msg) { out("FEHLER: " + msg); }

private:
    void out(const QString &line)
    {
        QTextStream(stdout) << line << "\n";
        fflush(stdout);
    }

    QString m_stationsFile;
    QString m_planFile;
    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply;
    ReplayParser *m_parser;
};

class Probe : public QObject
{
    Q_OBJECT
public:
    Probe(const QString &from, const QString &to)
        : m_from(from), m_to(to), m_stage(0)
    {
        m_parser = new ParserTransitous(this);
        connect(m_parser, SIGNAL(stationsResult(StationsList)),
                this, SLOT(onStations(StationsList)));
        connect(m_parser, SIGNAL(journeyResult(JourneyResultList*)),
                this, SLOT(onJourneys(JourneyResultList*)));
        connect(m_parser, SIGNAL(errorOccured(QString)),
                this, SLOT(onError(QString)));
    }

    void start()
    {
        out(QString("QSslSocket::supportsSsl: ") + (QSslSocket::supportsSsl() ? "ja" : "nein"));
        out("OpenSSL im Prozess: " + loadedSslLibraries());

        // A plain request before the parser: it says whether the handshake
        // works at all, and with which error it does not.
        out("Rohe Anfrage an den Geokoder");
        QNetworkRequest req(QUrl("https://api.transitous.org/api/v1/geocode?text=Wien"));
        req.setRawHeader("User-Agent", "Fahrplan2/2.0 (MeeGo Harmattan; probe)");
        m_raw = m_nam.get(req);
        connect(m_raw, SIGNAL(finished()), this, SLOT(onRawFinished()));
        connect(m_raw, SIGNAL(sslErrors(QList<QSslError>)),
                this, SLOT(onRawSslErrors(QList<QSslError>)));
        m_timer.start();
    }

private slots:
    void onRawSslErrors(const QList<QSslError> &errors)
    {
        for (int i = 0; i < errors.count(); ++i)
            out(QString("  Zertifikatsfehler: %1 (%2)")
                .arg(errors.at(i).errorString()).arg(errors.at(i).error()));
    }

    void onRawFinished()
    {
        const QByteArray body = m_raw->readAll();
        out(QString("  Fehlercode %1 (%2), %3 Byte Rumpf, HTTP %4, in %5 ms")
            .arg(m_raw->error())
            .arg(m_raw->errorString())
            .arg(body.size())
            .arg(m_raw->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
            .arg(m_timer.elapsed()));
        out("OpenSSL im Prozess: " + loadedSslLibraries());
        m_raw->deleteLater();

        out("Suche nach Haltestelle: " + m_from);
        m_timer.restart();
        m_parser->findStationsByName(m_from);
    }

    void onStations(const StationsList &stations)
    {
        out(QString("  %1 Treffer in %2 ms").arg(stations.count()).arg(m_timer.elapsed()));
        for (int i = 0; i < stations.count() && i < 5; ++i) {
            const Station &s = stations.at(i);
            out(QString("  [%1] %2 (%3) %4,%5 %6")
                .arg(i).arg(s.name).arg(s.type)
                .arg(s.latitude, 0, 'f', 5).arg(s.longitude, 0, 'f', 5)
                .arg(s.miscInfo));
        }
        if (stations.isEmpty()) {
            fail("keine Haltestelle gefunden");
            return;
        }

        if (m_stage == 0) {
            m_stage = 1;
            m_departure = stations.first();
            out("Suche nach Haltestelle: " + m_to);
            m_timer.restart();
            m_parser->findStationsByName(m_to);
            return;
        }

        m_arrival = stations.first();
        m_stage = 2;
        out(QString("Verbindungssuche %1 -> %2").arg(m_departure.name).arg(m_arrival.name));
        m_timer.restart();
        m_parser->searchJourney(m_departure, Station(false), m_arrival,
                                QDateTime::currentDateTime(), ParserAbstract::Departure, 0);
    }

    void onJourneys(JourneyResultList *result)
    {
        out(QString("  %1 Verbindungen in %2 ms (Antwort gelesen und zerlegt)")
            .arg(result->itemcount()).arg(m_timer.elapsed()));
        out("  " + result->timeInfo());
        for (int i = 0; i < result->itemcount() && i < 5; ++i) {
            JourneyResultItem *item = result->getItem(i);
            out(QString("  [%1] %2 ab %3 an %4, %5 h, %6 Umstiege, %7")
                .arg(i).arg(item->date().toString("dd.MM.yyyy"))
                .arg(item->departureTime()).arg(item->arrivalTime())
                .arg(item->duration())
                .arg(item->transfers().isEmpty() ? "0" : item->transfers())
                .arg(item->trainType()));
        }
        out("Ortszeit des Geraets jetzt: "
            + QDateTime::currentDateTime().toString("dd.MM.yyyy HH:mm"));
        out("FERTIG");
        QCoreApplication::exit(0);
    }

    void onError(const QString &msg)
    {
        fail(msg);
    }

private:
    void out(const QString &line)
    {
        QTextStream(stdout) << line << "\n";
        // Unbuffered, so a hanging request still leaves its trace.
        fflush(stdout);
    }
    void fail(const QString &msg)
    {
        out("FEHLER: " + msg);
        QCoreApplication::exit(1);
    }

    QString m_from;
    QString m_to;
    int m_stage;
    QTime m_timer;
    QNetworkAccessManager m_nam;
    QNetworkReply *m_raw;
    Station m_departure;
    Station m_arrival;
    ParserTransitous *m_parser;
};

#include "transitous-probe.moc"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // transitous-probe --dateien <geocode.json> <plan.json>
    if (argc > 3 && QString::fromLatin1(argv[1]) == QLatin1String("--dateien")) {
        Replay *replay = new Replay(QString::fromLocal8Bit(argv[2]),
                                    QString::fromLocal8Bit(argv[3]));
        replay->start();
        QTimer::singleShot(60000, &app, SLOT(quit()));
        return app.exec();
    }

    const QString from = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString("Wien Hbf");
    const QString to = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString("Graz Hbf");

    Probe probe(from, to);
    probe.start();

    // Nothing may hang forever on a device without a console.
    QTimer::singleShot(120000, &app, SLOT(quit()));
    return app.exec();
}
