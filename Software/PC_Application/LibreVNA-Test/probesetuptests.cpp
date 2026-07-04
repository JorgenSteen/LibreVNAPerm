#include "probesetuptests.h"

#include "VNA/probesetup.h"
#include "touchstone.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>

namespace {

// Golden numbers from the validated Python reference (see
// bilinear-share/verify_simulated.py): water-30c sample extracted with the
// three standards, mean absolute error of eps'/eps'' vs the sample file's
// own Perm columns.
constexpr double maeTolerance = 0.002;

nlohmann::json probeConfig(const QString &dataDir, double temperature, const char *epsSource, bool directoryMode)
{
    nlohmann::json j;
    j["airFile"] = (dataDir + "/ShortOpen/sim-P1-open.s2p").toStdString();
    j["waterFile"] = (dataDir + "/Water/sim-P1-DI_water-22p5c.s2p").toStdString();
    j["saltwaterFile"] = (dataDir + "/Saltwater/sim-P1-saltwater-22p5c.s2p").toStdString();
    j["temperature"] = temperature;
    j["epsSource"] = epsSource;
    j["directoryMode"] = directoryMode;
    j["directory"] = dataDir.toStdString();
    return j;
}

// mean absolute eps'/eps'' error of compute() over the water-30c sample,
// compared against the sample file's own Perm columns
void sampleMAE(ProbeSetup &ps, const QString &samplePath, double &maeRe, double &maeIm)
{
    auto t = Touchstone::fromFile(samplePath.toStdString());
    QVERIFY(t.points() > 0);
    std::vector<TraceMath::Data> truth; // eps' - j*eps'' from the Perm columns
    QVERIFY(ProbeSetup::loadPermColumns(samplePath, truth));
    QCOMPARE(truth.size(), (size_t) t.points());
    maeRe = maeIm = 0.0;
    for(unsigned int i = 0; i < t.points(); i++) {
        auto eps = ps.compute(t.point(i).frequency, t.point(i).S[0]); // eps' + j*eps''
        QVERIFY2(!std::isnan(eps.real()), qPrintable(QString("NaN at %1 Hz: %2").arg(t.point(i).frequency).arg(ps.getLastError())));
        maeRe += std::abs(eps.real() - truth[i].y.real());
        maeIm += std::abs(eps.imag() - (-truth[i].y.imag()));
    }
    maeRe /= t.points();
    maeIm /= t.points();
}

}

void ProbeSetupTests::initTestCase()
{
    // the simulated data set sits next to the repository checkout
    // (<workspace>/simulated data, see the workspace CLAUDE.md); allow an
    // explicit override for other layouts
    QStringList candidates;
    auto env = qgetenv("PERMITTIVITY_TEST_DATA");
    if(!env.isEmpty()) {
        candidates.append(QString::fromLocal8Bit(env));
    }
    candidates.append(QFileInfo(__FILE__).absolutePath() + "/../../../../simulated data");
    // relative to the test binary (in-source and release/ subdir builds)
    candidates.append(QCoreApplication::applicationDirPath() + "/../../../../simulated data");
    candidates.append(QCoreApplication::applicationDirPath() + "/../../../../../simulated data");
    for(const auto &c : candidates) {
        if(QFileInfo(c + "/Water/sim-P1-DI_water-30c.s2p").exists()) {
            dataDir = QFileInfo(c).absoluteFilePath();
            return;
        }
    }
    QSKIP("simulated data set not found, set PERMITTIVITY_TEST_DATA to its root");
}

void ProbeSetupTests::computeFileColumns()
{
    ProbeSetup ps;
    ps.fromJSON(probeConfig(dataDir, 22.5, "fileColumns", false));
    QVERIFY2(ps.valid(), qPrintable(ps.getLastError()));
    double maeRe, maeIm;
    sampleMAE(ps, dataDir + "/Water/sim-P1-DI_water-30c.s2p", maeRe, maeIm);
    QVERIFY2(std::abs(maeRe - 0.6472) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 0.7541) <= maeTolerance, qPrintable(QString::number(maeIm)));
}

void ProbeSetupTests::computeModels()
{
    ProbeSetup ps;
    ps.fromJSON(probeConfig(dataDir, 22.5, "models", false));
    QVERIFY2(ps.valid(), qPrintable(ps.getLastError()));
    double maeRe, maeIm;
    sampleMAE(ps, dataDir + "/Water/sim-P1-DI_water-30c.s2p", maeRe, maeIm);
    QVERIFY2(std::abs(maeRe - 0.6683) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 0.4523) <= maeTolerance, qPrintable(QString::number(maeIm)));
}

void ProbeSetupTests::computeDirectoryMode10C()
{
    ProbeSetup ps;
    ps.fromJSON(probeConfig(dataDir, 10.0, "models", true));
    QVERIFY2(ps.valid(), qPrintable(ps.getLastError()));
    double maeRe, maeIm;
    sampleMAE(ps, dataDir + "/Water/sim-P1-DI_water-30c.s2p", maeRe, maeIm);
    QVERIFY2(std::abs(maeRe - 2.1988) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 10.5158) <= maeTolerance, qPrintable(QString::number(maeIm)));
}

void ProbeSetupTests::exportRoundtrip()
{
    // export the whole probe (all temperatures) from the simulated data
    // set, then reimport it via directory mode and verify the results are
    // identical to computing directly from the source data
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    // set PERMITTIVITY_EXPORT_KEEP to a directory to keep the exported
    // probe for inspection / driving the application with it
    QString exportPath = tempDir.path();
    auto keep = qgetenv("PERMITTIVITY_EXPORT_KEEP");
    if(!keep.isEmpty()) {
        exportPath = QString::fromLocal8Bit(keep);
        QVERIFY(QDir().mkpath(exportPath));
    }
    ProbeSetup ps;
    ps.fromJSON(probeConfig(dataDir, 22.5, "fileColumns", true));
    QString error;
    QVERIFY2(ps.exportProbe(exportPath, "P1sim", "unittest", error), qPrintable(error));

    // every temperature of the source set must have been exported
    QDir dir(exportPath);
    auto written = dir.entryList({"*.s2p"}, QDir::Files);
    QVERIFY2(written.size() == 1 + 2 * 8, qPrintable(QString("expected 17 files, got %1").arg(written.size())));

    // each written file must still parse as a Touchstone file with the
    // source file's S11
    auto exported = Touchstone::fromFile((exportPath + "/P1sim-water-22p5c.s2p").toStdString());
    auto source = Touchstone::fromFile((dataDir + "/Water/sim-P1-DI_water-22p5c.s2p").toStdString());
    QCOMPARE(exported.points(), source.points());
    for(unsigned int i = 0; i < exported.points(); i++) {
        QCOMPARE(exported.point(i).frequency, source.point(i).frequency);
        QVERIFY(std::abs(exported.point(i).S[0] - source.point(i).S[0]) < 1e-9);
    }
    // metadata comments must be present
    QFile f(exportPath + "/P1sim-water-22p5c.s2p");
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    auto content = QString::fromUtf8(f.readAll());
    QVERIFY(content.contains("! Probe: P1sim"));
    QVERIFY(content.contains("! Device: unittest"));
    QVERIFY(content.contains("! Standard: water"));
    QVERIFY(content.contains("! Temperature: 22.5"));
    QVERIFY(content.contains("! ThirdStandard: saltwater"));

    // reimported probe must reproduce the golden numbers exactly
    ProbeSetup reimported;
    reimported.fromJSON(probeConfig(exportPath, 22.5, "fileColumns", true));
    QVERIFY2(reimported.valid(), qPrintable(reimported.getLastError()));
    double maeRe, maeIm;
    sampleMAE(reimported, dataDir + "/Water/sim-P1-DI_water-30c.s2p", maeRe, maeIm);
    QVERIFY2(std::abs(maeRe - 0.6472) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 0.7541) <= maeTolerance, qPrintable(QString::number(maeIm)));

    // switching the reimported probe to another exported temperature must
    // resolve the matching files (models golden numbers @ 10C)
    ProbeSetup reimported10;
    reimported10.fromJSON(probeConfig(exportPath, 10.0, "models", true));
    QVERIFY2(reimported10.valid(), qPrintable(reimported10.getLastError()));
    sampleMAE(reimported10, dataDir + "/Water/sim-P1-DI_water-30c.s2p", maeRe, maeIm);
    QVERIFY2(std::abs(maeRe - 2.1988) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 10.5158) <= maeTolerance, qPrintable(QString::number(maeIm)));
}

void ProbeSetupTests::computeOutsideCoverage()
{
    ProbeSetup ps;
    ps.fromJSON(probeConfig(dataDir, 22.5, "fileColumns", false));
    QVERIFY2(ps.valid(), qPrintable(ps.getLastError()));
    // the standards start at 100 MHz, below that there is no coverage
    auto eps = ps.compute(50e6, std::complex<double>(0.5, -0.5));
    QVERIFY(std::isnan(eps.real()));
    QVERIFY(std::isnan(eps.imag()));
}
