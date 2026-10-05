#pragma once

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>

#include "pyrowavecalibrationpolicy.h"

class ComputerManager;

// Finds, for each resolution and PyroWave format at one frame rate, the
// chosen bitrate target at which this device decodes and draws 99% of frames
// within the frame period, capped by a loss/delay-tested UDP wire budget
// including the host FEC policy. It does not change settings.
class PyroWaveCalibrator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool bandwidthReady READ bandwidthReady NOTIFY changed)
    Q_PROPERTY(int bandwidthKbps READ bandwidthKbps NOTIFY changed)
    Q_PROPERTY(bool networkTimingWarning READ networkTimingWarning NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString linkSummary READ linkSummary NOTIFY changed)
    Q_PROPERTY(QVariantList results READ results NOTIFY changed)

public:
    enum BitrateTarget {
        Minimum = PyroWaveCalibration::Minimum,
        Recommended = PyroWaveCalibration::Recommended,
        Moderate = PyroWaveCalibration::Moderate,
        Maximum = PyroWaveCalibration::Maximum
    };
    Q_ENUM(BitrateTarget)

    explicit PyroWaveCalibrator(QObject* parent = nullptr);
    ~PyroWaveCalibrator() override;

    bool running() const { return m_Running; }
    bool bandwidthReady() const { return m_BandwidthReady && !m_Running; }
    int bandwidthKbps() const { return m_LinkCapKbps; }
    bool networkTimingWarning() const { return m_NetworkTimingWarning; }
    QString message() const { return m_Message; }
    QString linkSummary() const { return m_LinkSummary; }
    QVariantList results() const { return m_Results; }

    // displayWidth/displayHeight size the render that each test frame goes
    // through, as a stream drawing to that display would. Zero renders at the
    // stream's own resolution.
    Q_INVOKABLE void start(ComputerManager* manager, const QString& hostUuid, int fps,
                           int displayWidth = 0, int displayHeight = 0,
                           int bitrateTarget = Recommended);
    // Step one tests only the connection. Step two uses that fresh, in-session
    // budget and must explicitly be started after the network worker finishes.
    Q_INVOKABLE void startDecoderTest(const QString& hostUuid, int bitrateTarget);
    // Closing calibration also invalidates the confirmed network budget.
    Q_INVOKABLE void reset();
    // Stops after the current network transfer or frame; finished results stay.
    Q_INVOKABLE void cancel();

signals:
    void changed();

private:
    void watchWorker();
    bool m_Running = false;
    bool m_BandwidthReady = false;
    bool m_NetworkTimingWarning = false;
    int m_LinkCapKbps = 0;
    int m_Fps = 60;
    int m_DisplayWidth = 0;
    int m_DisplayHeight = 0;
    int m_Target = Recommended;
    QString m_HostUuid;
    pyrowave::bandwidth::transport_t m_Transport;
    QString m_Message;
    QString m_LinkSummary;
    QVariantList m_Results;
    QPointer<QThread> m_Worker;
    std::shared_ptr<std::atomic<bool>> m_Cancel;
};
