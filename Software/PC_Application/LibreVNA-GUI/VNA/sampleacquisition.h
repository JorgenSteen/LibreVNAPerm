#ifndef SAMPLEACQUISITION_H
#define SAMPLEACQUISITION_H

#include "Device/devicedriver.h"
#include "Traces/Math/tracemath.h"

#include <QObject>
#include <QString>

#include <vector>

class VNA;
class QLabel;

/*
 * One-click "solid measurement + save": arms a single-sweep acquisition with
 * the configured number of averages, collects the averaged (calibrated,
 * de-embedded if active) datapoints as they stream in and, when the sweep
 * auto-stops with the averaging settled, writes the result as an augmented
 * Touchstone .s2p (S11 + permittivity columns + metadata) into the chosen
 * directory. See ProbeSetup::writeSampleFile for the file format.
 *
 * State machine, mirrored by the indicator light in the toolbar:
 *   Idle   (dark)   - nothing armed yet
 *   Armed  (yellow) - waiting for the sweeps to complete; a sweepStopped with
 *                     no data collected (e.g. no device connected yet) keeps
 *                     the state Armed, the acquisition starts with the sweep
 *   Done   (green)  - file written
 *   Failed (red)    - stopped with incomplete data, or the file write failed
 */
class SampleAcquisition : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Idle,
        Armed,
        Done,
        Failed,
    };

    explicit SampleAcquisition(VNA *vna);

    // toolbar contents: "Acquire sample..." button + indicator light
    QWidget *createToolWidget();
    // configuration dialog (directory + sample name), arms on accept
    void showDialog();
    // arm programmatically: remembers directory/name, clears the collector
    // and restarts a single sweep. Returns false if the VNA cannot start
    // (calibration measurement in progress).
    bool arm(const QString &directory, const QString &name);

    // fed from VNA::NewDatapoint with the final per-point measurement
    // (averaged, calibrated, de-embedded if active); ignored unless Armed
    void addDatapoint(const DeviceDriver::VNAMeasurement &m);
    // fed from the VNA's sweepStopped signal with the averaging state
    void sweepFinished(bool settled, double scatter, unsigned int averages, const QString &device);

    State getState() const { return state; }
    QString getLastFile() const { return lastFile; }
    QString getLastError() const { return lastError; }

signals:
    void stateChanged();

private:
    void setState(State s, const QString &status);
    void updateLight();

    VNA *vna;
    State state;
    QLabel *light;
    QString directory, name;
    QString statusMsg, lastFile, lastError;
    std::vector<TraceMath::Data> gamma; // collected S11 per point
    std::vector<TraceMath::Data> perm;  // collected PERMITTIVITY per point (display convention), NaN if unavailable
    int lastPointNum;
};

#endif // SAMPLEACQUISITION_H
