#pragma once

#include <QHostAddress>
#include <QElapsedTimer>
#include <QString>
#include <QByteArray>
#include <QTimer>
#include <atomic>
#include <cstdint>

class QUdpSocket;

namespace PyroWaveUdp {
// Opens an ephemeral receiver and returns the resolved remote address. Use that
// same address for the HTTPS probe so its sender and receiver agree on family.
QHostAddress bindReceiver(QUdpSocket& socket, const QString& hostname,
                         const std::atomic<bool>& cancelled);

// The paired HTTPS response announces the sender's ephemeral UDP port before
// transmission. Send the token from the receiving socket to establish the
// same UDP exchange as streaming, without an unsolicited inbound allowance.
class ProbeHandshake {
public:
    ProbeHandshake(QUdpSocket& socket, const QHostAddress& host,
                   const QByteArray& token, const std::atomic<bool>& cancelled);
    bool start(const QByteArray& portHeader);
    bool acceptsSource(const QHostAddress& source, quint16 port) const;
    void receivedPacket() { m_Retry.stop(); }
    QString errorString() const { return m_Error; }
    bool started() const { return m_Port != 0; }
private:
    void sendToken();
    QUdpSocket& m_Socket;
    QHostAddress m_Host;
    QByteArray m_Token;
    const std::atomic<bool>& m_Cancelled;
    QTimer m_Retry;
    quint16 m_Port = 0;
    QString m_Error;
};

// Darwin supplies packet enqueue timestamps independently of when Qt's event
// loop drains the socket. Other platforms retain the existing read-time clock.
class ReceiverTiming {
public:
    explicit ReceiverTiming(QUdpSocket& socket);
    qint64 readDatagram(char* data, qint64 capacity, QHostAddress* source, qint64& arrivalUs,
                       quint16* sourcePort = nullptr);
    bool usesKernelTimestamps() const { return m_KernelTimestamps; }
    qint64 lastReadDelayUs() const { return m_LastReadDelayUs; }
private:
    QUdpSocket& m_Socket;
    QElapsedTimer m_Clock;
    bool m_KernelTimestamps = false;
    uint64_t m_StartTicks = 0;
    uint32_t m_TimebaseNumer = 0;
    uint32_t m_TimebaseDenom = 1;
    qint64 m_LastReadDelayUs = 0;
};
}
