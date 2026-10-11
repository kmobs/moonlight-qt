#include "backend/pyrowaveudpprobe.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QUdpSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <vector>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void loopback(const QString& hostname, QAbstractSocket::NetworkLayerProtocol protocol)
{
    std::atomic<bool> cancelled(false);
    QUdpSocket receiver;
    const auto remote = PyroWaveUdp::bindReceiver(receiver, hostname, cancelled);
    PyroWaveUdp::ReceiverTiming timing(receiver);
    require(remote.protocol() == protocol, "Unexpected resolved address family");
    require(receiver.localPort() != 0, "No ephemeral receiver port");
    require(receiver.localAddress().protocol() == protocol, "Receiver has wrong family");
    QUrl probeUrl(QStringLiteral("https://localhost:47984/pyrowave-udp-probe"));
    probeUrl.setHost(remote.toString());
    require(QHostAddress(probeUrl.host()) == remote, "Probe URL does not retain resolved address");
    const QHostAddress destination(protocol == QAbstractSocket::IPv4Protocol ?
                                   QHostAddress::LocalHost : QHostAddress::LocalHostIPv6);
    QUdpSocket sender;
    const QByteArray payload("pyrowave-udp-address-regression");
    require(sender.writeDatagram(payload, destination, receiver.localPort()) == payload.size(),
            "UDP send failed");
    require(receiver.waitForReadyRead(2000), "UDP did not reach the receiver");
    QByteArray received(payload.size(), '\0');
    QHostAddress source;
    qint64 arrivalUs;
    require(timing.readDatagram(received.data(), received.size(), &source, arrivalUs) == payload.size(),
            "Unexpected UDP receive size");
    require(arrivalUs >= 0, "No valid arrival timestamp");
    require(received == payload && source == destination, "Unexpected UDP payload or source");
    std::printf("%s: resolved %s, real UDP reception PASS\n", qPrintable(hostname), qPrintable(remote.toString()));
}

static void handshake(QAbstractSocket::NetworkLayerProtocol protocol)
{
    std::atomic<bool> cancelled(false);
    QUdpSocket receiver, sender;
    const QString hostname = protocol == QAbstractSocket::IPv4Protocol ? "127.0.0.1" : "::1";
    const auto host = PyroWaveUdp::bindReceiver(receiver, hostname, cancelled);
    require(sender.bind(host, 0), "Could not bind handshake sender");
    const QByteArray token(32, 'a');
    PyroWaveUdp::ProbeHandshake hello(receiver, host, token, cancelled);
    const auto header = QByteArray::number(sender.localPort());
    require(hello.start(header), "Handshake did not start");
    require(sender.waitForReadyRead(1000), "No outbound handshake arrived");
    char data[64];
    QHostAddress source;
    quint16 sourcePort;
    require(sender.readDatagram(data, sizeof(data), &source, &sourcePort) == token.size() &&
            QByteArray(data, token.size()) == token && source == host && sourcePort == receiver.localPort(),
            "Handshake did not originate from the actual receiver socket");
    // Lose the first token: the event loop must retransmit until data arrives.
    QEventLoop loop;
    bool retried = false;
    QObject::connect(&sender, &QUdpSocket::readyRead, &loop, [&] {
        retried = sender.readDatagram(data, sizeof(data)) == token.size();
        loop.quit();
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    require(retried, "Lost handshake was not retried");
    require(hello.acceptsSource(host, sender.localPort()), "Real sender was rejected");
    require(!hello.acceptsSource(host, receiver.localPort()), "Unannounced sender port was accepted");
    hello.receivedPacket();
    require(!sender.waitForReadyRead(250), "Handshake continued after measured packets arrived");
    cancelled = true;
    PyroWaveUdp::ProbeHandshake stopped(receiver, host, token, cancelled);
    require(stopped.start(header) && !sender.waitForReadyRead(150), "Cancelled handshake sent a packet");
    for (const QByteArray& invalid : {QByteArray(), QByteArray("0"), QByteArray("1023"),
             QByteArray("65536"), QByteArray("+4096"), QByteArray("4096 "), QByteArray("abcd")}) {
        PyroWaveUdp::ProbeHandshake bad(receiver, host, token, cancelled);
        require(!bad.start(invalid) && !bad.errorString().isEmpty(), "Invalid sender port was accepted");
    }
    std::printf("%s UDP handshake, retry, source-port filtering and cancellation PASS\n", qPrintable(hostname));
}

static void progressiveHeaders()
{
    std::atomic<bool> cancelled(false);
    QUdpSocket receiver, sender;
    const auto host = PyroWaveUdp::bindReceiver(receiver, "127.0.0.1", cancelled);
    require(sender.bind(host, 0), "Could not bind progressive sender");
    const QByteArray token(32, 'b');
    PyroWaveUdp::ProbeHandshake hello(receiver, host, token, cancelled);
    QTcpServer server;
    require(server.listen(host, 0), "Could not open header fixture");
    QTcpSocket* connection = nullptr;
    bool announced = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        connection = server.nextPendingConnection();
        QObject::connect(connection, &QTcpSocket::readyRead, &server, [&] {
            connection->readAll();
            if (announced) return;
            announced = true;
            connection->write("HTTP/1.1 200 OK\r\nConnection: close\r\nX-PyroWave-Udp-Port: " +
                              QByteArray::number(sender.localPort()) + "\r\n\r\n");
        });
    });
    bool confirmed = false;
    QObject::connect(&sender, &QUdpSocket::readyRead, &server, [&] {
        char bytes[64];
        QHostAddress source;
        quint16 port;
        if (sender.readDatagram(bytes, sizeof(bytes), &source, &port) == token.size() &&
            QByteArray(bytes, token.size()) == token && port == receiver.localPort()) {
            confirmed = true;
            connection->write("<root status_code=\"200\"/>");
            connection->disconnectFromHost();
        }
    });
    QNetworkAccessManager network;
    auto* reply = network.get(QNetworkRequest(QUrl(QString("http://127.0.0.1:%1/probe").arg(server.serverPort()))));
    bool headersBeforeBody = false;
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] {
        headersBeforeBody = !reply->isFinished() && !confirmed;
        if (!hello.start(reply->rawHeader("X-PyroWave-Udp-Port"))) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    require(headersBeforeBody && confirmed && reply->isFinished() && reply->error() == QNetworkReply::NoError &&
            reply->readAll() == "<root status_code=\"200\"/>",
            "HTTPS-style headers could not establish UDP before the final response body");
    hello.receivedPacket();
    delete reply;
    std::puts("Progressive port headers precede UDP handshake and final body PASS");
}

static void hostFixture(const QString& executable)
{
    QProcess fixture;
    fixture.start(executable);
    require(fixture.waitForStarted(2000) && fixture.waitForReadyRead(2000), "Host fixture did not start");
    bool valid = false;
    const auto httpPort = fixture.readLine().trimmed().toUShort(&valid);
    require(valid && httpPort != 0, "Host fixture did not announce its HTTP port");
    std::atomic<bool> cancelled(false);
    QUdpSocket receiver;
    const auto host = PyroWaveUdp::bindReceiver(receiver, "127.0.0.1", cancelled);
    receiver.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
    const QByteArray token(32, 'd');
    PyroWaveUdp::ProbeHandshake hello(receiver, host, token, cancelled);
    PyroWaveUdp::ReceiverTiming timing(receiver);
    constexpr unsigned expected = uint64_t(5000) * 2000 / (8 * (1392 + 134));
    std::vector<bool> seen(expected, false);
    unsigned received = 0;
    bool invalidPacket = false;
    QObject::connect(&receiver, &QUdpSocket::readyRead, &receiver, [&] {
        while (receiver.hasPendingDatagrams()) {
            char data[2048];
            QHostAddress source;
            quint16 port;
            qint64 arrivalUs;
            const auto bytes = timing.readDatagram(data, sizeof(data), &source, arrivalUs, &port);
            if (bytes == token.size()) continue; // Host warmups are never scored.
            if (bytes != 1440 || arrivalUs < 0 || !hello.acceptsSource(source, port) ||
                QByteArray(data, 32) != token) { invalidPacket = true; continue; }
            hello.receivedPacket();
            const auto seq = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(data + 32));
            if (seq >= expected || seen[seq]) { invalidPacket = true; continue; }
            seen[seq] = true;
            ++received;
        }
    });
    QUrl url(QString("http://127.0.0.1:%1/pyrowave-udp-probe").arg(httpPort));
    QUrlQuery query;
    query.addQueryItem("kbps", "5000");
    query.addQueryItem("packetsize", "1392");
    query.addQueryItem("port", QString::number(receiver.localPort()));
    query.addQueryItem("token", QString::fromLatin1(token));
    query.addQueryItem("handshake", "1");
    url.setQuery(query);
    QNetworkAccessManager network;
    auto* reply = network.get(QNetworkRequest(url));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] {
        if (!hello.start(reply->rawHeader("X-PyroWave-Udp-Port"))) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(6000, &loop, &QEventLoop::quit);
    loop.exec();
    const auto body = reply->readAll();
    const bool success = reply->isFinished() && reply->error() == QNetworkReply::NoError;
    delete reply;
    fixture.kill();
    fixture.waitForFinished(2000);
    QXmlStreamReader xml(body);
    unsigned sent = 0;
    bool statusOkay = false;
    while (!xml.atEnd()) {
        if (!xml.readNextStartElement()) continue;
        if (xml.name() == u"root") statusOkay = xml.attributes().value("status_code") == u"200";
        if (xml.name() == u"sent") sent = xml.readElementText().toUInt();
    }
    if (!success || xml.hasError() || !statusOkay || invalidPacket || sent != expected || received != expected) {
        std::fprintf(stderr, "Host fixture: HTTP success=%d, XML valid=%d, status=%d, invalid packet=%d, sent=%u received=%u expected=%u, body=%s\n",
                     success, !xml.hasError(), statusOkay, invalidPacket, sent, received, expected, body.constData());
    }
    require(success && !xml.hasError() && statusOkay && !invalidPacket && sent == expected && received == expected,
            "Production host sender/HTTP header framing did not deliver the complete measured probe");
    std::printf("Native host fixture: %u/%u measured packets, progressive headers and response completion PASS\n",
                received, expected);
}

static void delayedReader(QAbstractSocket::NetworkLayerProtocol protocol)
{
    std::atomic<bool> cancelled(false);
    QUdpSocket receiver, sender;
    const QString host = protocol == QAbstractSocket::IPv4Protocol ? "127.0.0.1" : "::1";
    const auto remote = PyroWaveUdp::bindReceiver(receiver, host, cancelled);
    PyroWaveUdp::ReceiverTiming timing(receiver);
    require(sender.writeDatagram("a", 1, remote, receiver.localPort()) == 1, "First send failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    require(sender.writeDatagram("b", 1, remote, receiver.localPort()) == 1, "Second send failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    qint64 firstUs, secondUs;
    char packet;
    QHostAddress source;
    require(timing.readDatagram(&packet, 1, &source, firstUs) == 1 && packet == 'a', "First delayed read failed");
    const auto firstReadDelayUs = timing.lastReadDelayUs();
    require(timing.readDatagram(&packet, 1, &source, secondUs) == 1 && packet == 'b', "Second delayed read failed");
    require(firstUs >= 0 && secondUs >= 0, "Missing delayed-read timestamps");
#ifdef Q_OS_DARWIN
    require(timing.usesKernelTimestamps(), "Kernel arrival timestamping was not enabled");
    require(secondUs - firstUs >= 40000, "Reader stall collapsed the actual packet spacing");
    require(firstReadDelayUs >= 120000, "Application receive-queue delay was not separated");
#endif
    // Native peeking must not prevent Qt from notifying the next datagram.
    QEventLoop loop;
    bool notified = false;
    QObject::connect(&receiver, &QUdpSocket::readyRead, &loop, [&] {
        qint64 nextUs;
        if (timing.readDatagram(&packet, 1, &source, nextUs) == 1 && packet == 'c' && nextUs >= 0) {
            notified = true;
            loop.quit();
        }
    });
    QTimer::singleShot(10, &loop, [&] { sender.writeDatagram("c", 1, remote, receiver.localPort()); });
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    require(notified, "Qt datagram notification did not rearm after native timestamp peek");
    std::printf("%s delayed reader: kernel=%d, packet spacing %.2f ms, reader delay %.2f ms PASS\n",
                qPrintable(host), timing.usesKernelTimestamps(), (secondUs-firstUs)/1000.0,
                firstReadDelayUs/1000.0);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        loopback(QStringLiteral("127.0.0.1"), QAbstractSocket::IPv4Protocol);
        loopback(QStringLiteral("::1"), QAbstractSocket::IPv6Protocol);
        // QHostAddress alone cannot parse this hostname: the original bug.
        require(QHostAddress(QStringLiteral("localhost")).isNull(), "Hostname fixture is numeric");
        loopback(QStringLiteral("localhost"), QAbstractSocket::IPv4Protocol);
        delayedReader(QAbstractSocket::IPv4Protocol);
        delayedReader(QAbstractSocket::IPv6Protocol);
        handshake(QAbstractSocket::IPv4Protocol);
        handshake(QAbstractSocket::IPv6Protocol);
        progressiveHeaders();
        std::atomic<bool> cancelled(true);
        QUdpSocket stopped;
        QElapsedTimer time;
        time.start();
        bool rejected = false;
        try { PyroWaveUdp::bindReceiver(stopped, QStringLiteral("localhost"), cancelled); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected && stopped.state() == QAbstractSocket::UnconnectedState && time.elapsed() < 1000,
                "Cancelled resolution opened a socket or did not stop promptly");
        cancelled = false;
        QUdpSocket invalid;
        rejected = false;
        try { PyroWaveUdp::bindReceiver(invalid, QString(), cancelled); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected && invalid.state() == QAbstractSocket::UnconnectedState,
                "Invalid host opened a socket");
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--host-fixture") {
            hostFixture(QString::fromLocal8Bit(argv[2]));
        }
        else if (argc > 1) {
            QUdpSocket actual;
            const auto remote = PyroWaveUdp::bindReceiver(actual, QString::fromLocal8Bit(argv[1]), cancelled);
            std::printf("Actual host %s: resolved %s, receiver port %u PASS\n",
                        argv[1], qPrintable(remote.toString()), actual.localPort());
        }
        std::puts("PyroWave UDP hostname/address checks passed");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
