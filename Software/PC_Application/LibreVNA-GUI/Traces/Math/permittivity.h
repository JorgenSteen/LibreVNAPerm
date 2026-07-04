#ifndef PERMITTIVITY_H
#define PERMITTIVITY_H

#include "tracemath.h"
#include "permittivitymath.h"

#include <array>

namespace Math {

/*
 * Complex permittivity extraction from a measured S11 trace using the
 * bilinear (Moebius) 3-standard calibration method (see permittivitymath.h).
 *
 * The chained input is the SAMPLE's S11 (frequency domain, linear complex).
 * The three calibration standards (open/air, water, saltwater) come from
 * touchstone files configured on the operation, together with the liquid
 * temperature and the source of the standards' known eps*.
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
    enum class EpsSource {
        FileColumns = 0, // Perm_Real/Perm_Imag columns of the standard files (custom format)
        Models = 1,      // reference models (air=1, water Debye, saltwater Cole-Cole) at the configured temperature
    };

    DataType outputType(DataType inputType) override;
    QString description() override;
    static QWidget *createExplanationWidget();
    nlohmann::json toJSON() override;
    void fromJSON(nlohmann::json j) override;
    Type getType() override {return Type::Permittivity;}

public slots:
    void inputSamplesChanged(unsigned int begin, unsigned int end) override;

private:
    // indices into files/standards
    static constexpr int Air = 0;
    static constexpr int Water = 1;
    static constexpr int Saltwater = 2;
    static const char *standardName(int standard);

    struct Standard {
        std::vector<Data> gamma; // measured S11 (linear complex) vs frequency [Hz]
        std::vector<Data> perm;  // known eps* from the file's Perm columns, as eps' - j*eps''; empty if the file has none
    };

    // (re)load all three standard files, returns false and sets loadError on failure
    bool loadStandards();
    // force a reload + recompute after the configuration changed
    void configurationChanged();
    // known eps* of a standard at freq [Hz], according to epsSource (NaN if unavailable)
    std::complex<double> knownEps(int standard, double freq);

    std::array<QString, 3> files;
    double temperature; // liquid temperature of the standards, degrees Celsius
    EpsSource epsSource;

    std::array<Standard, 3> standards;
    bool standardsLoaded;
    QString loadError;
    // per-output-sample condition number of the calibration solve (NaN where invalid)
    std::vector<double> condNumbers;
};

}

#endif // PERMITTIVITY_H
