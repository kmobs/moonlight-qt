#include "pyrowaveudpprobe.h"

#include <QEventLoop>
#include <QHostInfo>
#include <QTimer>
#include <QUdpSocket>
#include <stdexcept>
#include <cstring>
#ifdef Q_OS_DARWIN
#include <mach/mach_time.h>
#include <sys/socket.h>
#include <cerrno>
#endif

namespace {
QHostAddress resolveHost(const QString& hostname, const std::atomic<bool>& cancelled)
{
    if (cancelled.load()) throw std::runtime_error("Calibration stopped.");
    QHostAddress literal(hostname);
    if (!literal.isNull()) return literal;
    if (hostname.isEmpty()) throw std::runtime_error("The PyroWave UDP host is empty");

    QEventLoop loop;
    QHostInfo result;
    bool completed = false;
    const int lookup = QHostInfo::lookupHost(hostname, &loop, [&](const QHostInfo& info) {
        result = info;
        completed = true;
        loop.quit();
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(5000);
    QTimer cancellation;
    QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
        if (cancelled.load()) loop.quit();
    });
    cancellation.start(50);
    loop.exec();
    if (!completed) QHostInfo::abortHostLookup(lookup);
    if (cancelled.load()) throw std::runtime_error("Calibration stopped.");
    if (!completed) {
        throw std::runtime_error(QString("Resolving PyroWave UDP host '%1' timed out")
                                 .arg(hostname).toStdString());
    }
    if (result.error() == QHostInfo::NoError) {
        // Prefer IPv4 for a dual-stack hostname, retaining literal IPv6 and
        // IPv6-only hosts. The HTTPS request is pinned to this chosen address.
        for (auto protocol : {QAbstractSocket::IPv4Protocol, QAbstractSocket::IPv6Protocol}) {
            for (const auto& address : result.addresses()) {
                if (!address.isNull() && address.protocol() == protocol) return address;
            }
        }
    }
    throw std::runtime_error(QString("Could not resolve PyroWave UDP host '%1': %2")
                             .arg(hostname, result.errorString()).toStdString());
}
}

QHostAddress PyroWaveUdp::bindReceiver(QUdpSocket& socket, const QString& hostname,
                                      const std::atomic<bool>& cancelled)
{
    const QHostAddress remote = resolveHost(hostname, cancelled);
    const QHostAddress local(remote.protocol() == QAbstractSocket::IPv6Protocol ?
                            QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4);
    if (!socket.bind(local, 0)) {
        throw std::runtime_error(QString("Could not open the PyroWave UDP receiver: %1")
                                 .arg(socket.errorString()).toStdString());
    }
    return remote;
}

PyroWaveUdp::ReceiverTiming::ReceiverTiming(QUdpSocket& socket) : m_Socket(socket)
{
    m_Clock.start();
#ifdef Q_OS_DARWIN
    const int enabled = 1;
    if (setsockopt(int(socket.socketDescriptor()), SOL_SOCKET, SO_TIMESTAMP_MONOTONIC,
                   &enabled, sizeof(enabled)) != 0) {
        throw std::runtime_error(QString("Could not enable PyroWave UDP arrival timestamps: %1")
                                 .arg(QString::fromLocal8Bit(std::strerror(errno))).toStdString());
    }
    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) {
        throw std::runtime_error("Could not obtain the PyroWave UDP timestamp clock");
    }
    m_TimebaseNumer = timebase.numer;
    m_TimebaseDenom = timebase.denom;
    m_StartTicks = mach_absolute_time();
    m_KernelTimestamps = true;
#endif
}

qint64 PyroWaveUdp::ReceiverTiming::readDatagram(char* data, qint64 capacity,
                                               QHostAddress* source, qint64& arrivalUs)
{
    arrivalUs = -1;
    m_LastReadDelayUs = -1;
#ifdef Q_OS_DARWIN
    // Peek only the first byte plus ancillary metadata, then let Qt consume
    // the packet and maintain its notifier state/address parsing. This socket
    // has one reader, so both operations observe the same queued datagram.
    char byte;
    iovec buffer {&byte, 1};
    alignas(cmsghdr) unsigned char control[256] {};
    msghdr message {};
    message.msg_iov = &buffer;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (recvmsg(int(m_Socket.socketDescriptor()), &message, MSG_PEEK | MSG_DONTWAIT) < 0) return -1;
    uint64_t ticks = 0;
    if (!(message.msg_flags & MSG_CTRUNC)) {
        for (auto* entry = CMSG_FIRSTHDR(&message); entry; entry = CMSG_NXTHDR(&message, entry)) {
            if (entry->cmsg_level == SOL_SOCKET && entry->cmsg_type == SCM_TIMESTAMP_MONOTONIC &&
                entry->cmsg_len >= CMSG_LEN(sizeof(ticks))) {
                std::memcpy(&ticks, CMSG_DATA(entry), sizeof(ticks));
                break;
            }
        }
    }
    const auto bytes = m_Socket.readDatagram(data, capacity, source);
    if (bytes < 0) return bytes;
    const uint64_t readTicks = mach_absolute_time();
    if (ticks >= m_StartTicks && ticks <= readTicks) {
        const auto toUs = [&](uint64_t delta) {
            return qint64((static_cast<unsigned __int128>(delta) * m_TimebaseNumer / m_TimebaseDenom) / 1000);
        };
        arrivalUs = toUs(ticks - m_StartTicks);
        m_LastReadDelayUs = toUs(readTicks - ticks);
    }
    // A missing/invalid native timestamp is never substituted with read time.
    // The caller can reject an authenticated packet instead of misgrading it.
    return bytes;
#else
    const auto bytes = m_Socket.readDatagram(data, capacity, source);
    if (bytes >= 0) {
        arrivalUs = m_Clock.nsecsElapsed() / 1000;
        m_LastReadDelayUs = 0;
    }
    return bytes;
#endif
}
