#include "backend/pyrowaveudpprobe.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QUdpSocket>
#include <cstdio>
#include <stdexcept>
#include <chrono>
#include <thread>

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
        if (argc > 1) {
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
