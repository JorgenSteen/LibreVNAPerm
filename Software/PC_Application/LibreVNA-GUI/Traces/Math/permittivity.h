#ifndef PERMITTIVITY_H
#define PERMITTIVITY_H

#include "tracemath.h"
#include "permittivitymath.h"
#include "VNA/probesetup.h"

#include <array>

namespace Math {

/*
 * Complex permittivity extraction from a measured S11 trace using the
 * bilinear (Moebius) 3-standard calibration method (see permittivitymath.h).
 *
 * The chained input is the SAMPLE's S11 (frequency domain, linear complex).
 * The three calibration standards (open/air, water, saltwater) come from
 * touchstone files configured on the operation, together with the liquid
 * temperature and the source of the standards' known eps*. The
 * loading/resolution code is shared with the central probe setup
 * (VNA/probesetup.h), this operation is the offline/file-trace path.
 *
 * SIGN CONVENTION OF THE OUTPUT (important):
 * The math internally uses the physics/lossy convention eps* = eps' - j*eps''
 * with eps'' >= 0. The output samples store the CONJUGATE, y = eps' + j*eps'',
 * so that a trace displaying "Real" shows eps' and "Imag" shows eps'' -- both
 * positive and intuitive for a lossy material. Only the display sign is
 * flipped, nothing else.
 */
class Permittivity : public TraceMath
{
public:
    Permittivity();

    // where the known eps* of the standards comes from
    using EpsSource = ProbeSetup::EpsSource;

    DataType outputType(DataType inputType) override;
    QString description() override;
    // take the standards/temperature/eps* source from the central probe
    // setup (Calibration->Probe setup...), returns false if it has no
    // standards configured
    bool initFromProbeSetup();
    void edit() override;
    static QWidget *createExplanationWidget();

    nlohmann::json toJSON() override;
    void fromJSON(nlohmann::json j) override;
    Type getType() override {return Type::Permittivity;}

public slots:
    void inputSamplesChanged(unsigned int begin, unsigned int end) override;

private:
    // indices into files/standards
    static constexpr int Air = ProbeSetup::Air;
    static constexpr int Water = ProbeSetup::Water;
    static constexpr int Saltwater = ProbeSetup::Saltwater;

    // (re)load all three standard files, returns false and sets loadError on failure
    bool loadStandards();
    // force a reload + recompute after the configuration changed
    void configurationChanged();

    std::array<QString, 3> files;
    double temperature; // liquid temperature of the standards, degrees Celsius
    EpsSource epsSource;
    // directory mode: files[] is filled by resolving directory + temperature
    bool directoryMode;
    QString directory;

    std::array<ProbeSetup::Standard, 3> standards;
    bool standardsLoaded;
    QString loadError;
    // per-output-sample condition number of the calibration solve (NaN where invalid)
    std::vector<double> condNumbers;
};

}

#endif // PERMITTIVITY_H
