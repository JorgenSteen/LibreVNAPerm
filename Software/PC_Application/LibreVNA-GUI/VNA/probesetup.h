#ifndef PROBESETUP_H
#define PROBESETUP_H

#include "savable.h"
#include "Traces/Math/tracemath.h"
#include "Traces/Math/permittivitymath.h"

#include <QObject>
#include <QString>

#include <array>
#include <complex>
#include <map>
#include <vector>

/*
 * Central configuration of the dielectric probe: the three calibration
 * standard measurements (open/air, water, saltwater), the liquid temperature
 * and the source of the standards' known eps*. Configured once under
 * Calibration->"Probe setup..." and consumed by everything that computes
 * permittivity (the live PERMITTIVITY parameter and the trace math
 * operation, which shares the loading/resolution code below).
 *
 * SIGN CONVENTION: the math internally uses the physics/lossy convention
 * eps* = eps' - j*eps'' with eps'' >= 0. compute() returns the CONJUGATE,
 * eps' + j*eps'', so that "Real" displays eps' and "Imag" displays eps'' --
 * both positive and intuitive for a lossy material.
 */
class ProbeSetup : public QObject, public Savable
{
    Q_OBJECT
public:
    ProbeSetup();

    // where the known eps* of the standards comes from
    enum class EpsSource {
        FileColumns = 0, // Perm_Real/Perm_Imag columns of the standard files (custom format)
        Models = 1,      // reference models (air=1, water Debye, saltwater Cole-Cole) at the configured temperature
    };

    // how the three standard files are selected
    enum class Mode {
        Files = 0,     // three individually selected files
        Directory = 1, // resolved from a directory + temperature by filename convention
        Probe = 2,     // an imported probe from the probe library + temperature
    };

    // indices into files/standards
    static constexpr int Air = 0;
    static constexpr int Water = 1;
    static constexpr int Saltwater = 2;

    struct Standard {
        std::vector<TraceMath::Data> gamma; // measured S11 (linear complex) vs frequency [Hz]
        std::vector<TraceMath::Data> perm;  // known eps* from the file's Perm columns, as eps' - j*eps''; empty if the file has none
    };

    static const char *standardName(int standard);
    // Like TraceMath::interpolatedSample, but returns a true NaN for x outside
    // the data range (TraceMath's version returns (0,0) there because
    // numeric_limits<complex>::quiet_NaN() is not a NaN -- unspecialized
    // numeric_limits returns a value-initialized complex).
    static std::complex<double> interpolateOrNaN(const std::vector<TraceMath::Data> &data, double x);
    // temperature -> filename suffix, e.g. 22.5 -> "22p5c", 10 -> "10c"
    static QString temperatureSuffix(double tempC);
    // Directory mode: find the three standard files in a directory (searched
    // recursively) by filename convention. The air standard contains "open"
    // (but not "short") in its name; water/saltwater contain "water" /
    // "salt" and must carry a temperature suffix matching tempC, e.g.
    // "-22p5c" for 22.5 degrees. Returns false and sets error on failure.
    static bool resolveStandardsFromDirectory(const QString &dir, double tempC,
                                              std::array<QString, 3> &resolved, QString &error);
    // Parse the extra permittivity columns of the project's custom touchstone
    // files: freq S11_re S11_im Perm_re Perm_im [...]. Perm_im is stored
    // positive, internally eps* = Perm_re - j*Perm_im. Returns false (empty
    // out) if the file has no such columns.
    static bool loadPermColumns(const QString &filename, std::vector<TraceMath::Data> &out);
    // load the three standard files (S11 + optional Perm columns), returns false and sets error on failure
    static bool loadStandardFiles(const std::array<QString, 3> &files,
                                  std::array<Standard, 3> &standards, QString &error);
    // known eps* (eps' - j*eps'') of a standard at freq [Hz], according to source (NaN if unavailable)
    static std::complex<double> knownEps(const std::array<Standard, 3> &standards, int standard,
                                         double freq, EpsSource source, double temperature);

    // ===== probe library (imported stage-6 probe data) =====
    // directory the imported probes live in (AppDataLocation/probes)
    static QString probeLibraryDir();
    // names of the probes in the library (subdirectory names)
    static QStringList availableProbes();
    // temperatures for which a directory contains a complete standard set,
    // from the filename suffixes (sorted ascending)
    static std::vector<double> availableTemperatures(const QString &dir);
    // leading "! Key: value" metadata comments of a probe .s2p file
    static std::map<QString, QString> readMetadata(const QString &path);
    // Import a directory of stage-6 probe .s2p files into the library. The
    // probe name is taken from the files' metadata (fallback: source
    // directory name). Returns the imported probe name, empty on failure.
    static QString importProbe(const QString &srcDir, QString &error);

    // shows the (non-modal) probe setup dialog
    void edit();
    // "Export custom probe data": asks for probe name + destination
    // directory, then runs exportProbe. device = serial of the connected
    // device (empty if none), recorded in the file metadata.
    void exportDialog(const QString &device);
    // Export the configured probe into destDir: each standard is written as
    // an augmented Touchstone .s2p in the same column layout the simulated
    // data set uses (freq, S11 re/im, Perm re/im, 4 filler columns -- a
    // valid 2-port Touchstone file), plus probe/device metadata comment
    // lines. In directory mode every temperature found in the source
    // directory is exported, in individual mode the three configured files.
    // The written files resolve/load again through the directory mode (the
    // probe name must not contain "open"/"short"/"water"/"salt", they would
    // confuse the filename classification).
    bool exportProbe(const QString &destDir, const QString &probeName, const QString &device, QString &error);
    // true if the configured standards can be (and are) loaded
    bool valid();
    // permittivity of a sample with reflection coefficient S11 (linear
    // complex) at freq [Hz]. Returns eps' + j*eps'' (display convention, see
    // above) or NaN outside the standards' coverage / while not valid().
    std::complex<double> compute(double freq, std::complex<double> S11);
    QString getLastError() const { return lastError; }

    nlohmann::json toJSON() override;
    void fromJSON(nlohmann::json j) override;

signals:
    void settingsChanged();

private:
    // write one standard as an augmented Touchstone .s2p (see exportProbe)
    static bool writeStandardFile(const QString &path, int standard, double tempC,
                                  const QString &probeName, const QString &device,
                                  const Standard &data, EpsSource source, QString &error);
    bool ensureLoaded();
    // invalidate loaded standards + coefficient cache after a config change
    void invalidate();
    // QSettings "last used" persistence, independent of a saved setup
    void saveToSettings();
    void loadFromSettings();

    // configuration
    Mode mode;
    std::array<QString, 3> files;
    double temperature; // liquid temperature of the standards, degrees Celsius
    EpsSource epsSource;
    // directory mode: files[] is filled by resolving directory + temperature
    QString directory;
    // probe mode: like directory mode over probeLibraryDir()/probeName
    QString probeName;

    // loaded state
    std::array<Standard, 3> standards;
    bool standardsLoaded;
    QString lastError;
    // per-frequency bilinear coefficients, cached because live sweeps repeat
    // the same frequency grid point by point; invalidated on config change
    std::map<double, PermittivityMath::BilinearCoefficients> coeffCache;
};

#endif // PROBESETUP_H
