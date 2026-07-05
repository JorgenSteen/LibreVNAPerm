#include "probesetuptests.h"

#include "VNA/probesetup.h"
#include "VNA/vna.h"
#include "appwindow.h"
#include "modehandler.h"
#include "touchstone.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStandardPaths>
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

void ProbeSetupTests::probeLibraryImport()
{
    // sandbox AppDataLocation so the user's real probe library is untouched
    QStandardPaths::setTestModeEnabled(true);

    // export a probe from the simulated data, import it into the library
    QTemporaryDir exportDir;
    QVERIFY(exportDir.isValid());
    ProbeSetup ps;
    ps.fromJSON(probeConfig(dataDir, 22.5, "fileColumns", true));
    QString error;
    QVERIFY2(ps.exportProbe(exportDir.path(), "P2sim", "unittest", error), qPrintable(error));

    auto name = ProbeSetup::importProbe(exportDir.path(), error);
    QVERIFY2(name == QString("P2sim"), qPrintable(name+" / "+error));
    QVERIFY(ProbeSetup::availableProbes().contains("P2sim"));
    auto probeDir = ProbeSetup::probeLibraryDir()+"/P2sim";
    QCOMPARE(ProbeSetup::availableTemperatures(probeDir).size(), (size_t) 8);

    // metadata of an imported file
    auto meta = ProbeSetup::readMetadata(probeDir+"/P2sim-water-22p5c.s2p");
    QCOMPARE(meta["Probe"], QString("P2sim"));
    QCOMPARE(meta["Standard"], QString("water"));
    QCOMPARE(meta["ThirdStandard"], QString("saltwater"));

    // probe mode must reproduce the golden numbers
    nlohmann::json j;
    j["mode"] = "probe";
    j["probe"] = "P2sim";
    j["temperature"] = 22.5;
    j["epsSource"] = "fileColumns";
    ProbeSetup probe;
    probe.fromJSON(j);
    QVERIFY2(probe.valid(), qPrintable(probe.getLastError()));
    double maeRe, maeIm;
    sampleMAE(probe, dataDir + "/Water/sim-P1-DI_water-30c.s2p", maeRe, maeIm);
    QVERIFY2(std::abs(maeRe - 0.6472) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 0.7541) <= maeTolerance, qPrintable(QString::number(maeIm)));

    // switching the temperature must pick the matching embedded set
    j["temperature"] = 10.0;
    j["epsSource"] = "models";
    ProbeSetup probe10;
    probe10.fromJSON(j);
    QVERIFY2(probe10.valid(), qPrintable(probe10.getLastError()));
    sampleMAE(probe10, dataDir + "/Water/sim-P1-DI_water-30c.s2p", maeRe, maeIm);
    QVERIFY2(std::abs(maeRe - 2.1988) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 10.5158) <= maeTolerance, qPrintable(QString::number(maeIm)));

    QStandardPaths::setTestModeEnabled(false);
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

void ProbeSetupTests::livePermittivityParameter()
{
    // End-to-end test of the live PERMITTIVITY parameter: construct the
    // full application (GUI hidden, see the test main), feed the sample
    // sweep through VNA::NewDatapoint as if it came from a device and
    // verify that a live PERMITTIVITY trace fills with the golden numbers.
    auto window = new AppWindow;
    auto cleanup = qScopeGuard([&](){ delete window; });

    if(window->getDevice()) {
        // a real device was auto-connected, its sweep would interfere
        QMetaObject::invokeMethod(window, "DisconnectDevice", Qt::DirectConnection);
        QTest::qWait(100);
    }

    auto vna = dynamic_cast<VNA*>(window->getModeHandler()->findFirstOfType(Mode::Type::VNA));
    QVERIFY(vna);

    // socketless SCPI console
    QStringList output;
    auto conn = connect(window->getSCPI(), &SCPI::output, [&](QString line){
        output.append(line);
    });
    auto connCleanup = qScopeGuard([&](){ disconnect(conn); });
    auto command = [&](const QString &cmd){
        window->getSCPI()->input(cmd);
        QTest::qWait(20);
    };
    auto query = [&](const QString &cmd) -> QString {
        auto before = output.size();
        window->getSCPI()->input(cmd);
        for(int i = 0; i < 250 && output.size() == before; i++) {
            QTest::qWait(20);
        }
        return output.size() > before ? output.last() : QString();
    };

    // the water-30c sample plays the role of the device measurements
    auto samplePath = dataDir + "/Water/sim-P1-DI_water-30c.s2p";
    auto sample = Touchstone::fromFile(samplePath.toStdString());
    QVERIFY(sample.points() > 0);

    command(":VNA:ACQ:POINTS "+QString::number(sample.points()));
    QCOMPARE(query(":VNA:ACQ:POINTS?"), QString::number(sample.points()));
    command(":VNA:TRAC:NEW live_eps");
    command(":VNA:TRAC:PARAM live_eps PERMITTIVITY");
    QCOMPARE(query(":VNA:TRAC:PARAM? live_eps"), QString("PERMITTIVITY"));
    // let the configuration timer fire (no device -> changingSettings cleared)
    QTest::qWait(300);

    auto feedSweep = [&](){
        for(unsigned int i = 0; i < sample.points(); i++) {
            DeviceDriver::VNAMeasurement m;
            m.pointNum = i;
            m.Z0 = 50.0;
            m.frequency = sample.point(i).frequency;
            m.dBm = -10.0;
            m.measurements["S11"] = sample.point(i).S[0];
            QMetaObject::invokeMethod(vna, "NewDatapoint", Qt::DirectConnection,
                                      Q_ARG(DeviceDriver::VNAMeasurement, m));
        }
        QTest::qWait(100);
    };

    // with an invalid probe setup nothing must be injected
    nlohmann::json invalid = nlohmann::json::object();
    invalid["mode"] = "files";
    invalid["airFile"] = "";
    invalid["waterFile"] = "";
    invalid["saltwaterFile"] = "";
    vna->getProbeSetup().fromJSON(invalid);
    feedSweep();
    auto raw = query(":VNA:TRAC:DATA? live_eps");
    QVERIFY2(!raw.contains(','), "trace received data although the probe setup is invalid");

    // with a valid probe setup the sweep must produce the golden numbers
    vna->getProbeSetup().fromJSON(probeConfig(dataDir, 22.5, "fileColumns", false));
    QVERIFY2(vna->getProbeSetup().valid(), qPrintable(vna->getProbeSetup().getLastError()));
    feedSweep();
    raw = query(":VNA:TRAC:DATA? live_eps");

    std::vector<TraceMath::Data> truth; // eps' - j*eps'' from the sample's Perm columns
    QVERIFY(ProbeSetup::loadPermColumns(samplePath, truth));
    // parse "[freq,re,im],[freq,re,im],..."
    auto points = raw.split("],[");
    QCOMPARE((unsigned int) points.size(), sample.points());
    double maeRe = 0.0, maeIm = 0.0;
    for(int i = 0; i < points.size(); i++) {
        auto fields = points[i].remove('[').remove(']').split(',');
        QCOMPARE(fields.size(), 3);
        QVERIFY(std::abs(fields[0].toDouble() - truth[i].x) < 1.0);
        maeRe += std::abs(fields[1].toDouble() - truth[i].y.real());
        maeIm += std::abs(fields[2].toDouble() - (-truth[i].y.imag()));
    }
    maeRe /= points.size();
    maeIm /= points.size();
    QVERIFY2(std::abs(maeRe - 0.6472) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 0.7541) <= maeTolerance, qPrintable(QString::number(maeIm)));
}

void ProbeSetupTests::oneClickSampleAcquisition()
{
    // End-to-end test of the one-click sample acquisition: arm it, stream
    // two averaged sweeps through VNA::NewDatapoint as if they came from a
    // device and verify that the auto-stop writes a valid augmented sample
    // file with the golden permittivity numbers.
    auto window = new AppWindow;
    auto cleanup = qScopeGuard([&](){ delete window; });

    if(window->getDevice()) {
        QMetaObject::invokeMethod(window, "DisconnectDevice", Qt::DirectConnection);
        QTest::qWait(100);
    }

    auto vna = dynamic_cast<VNA*>(window->getModeHandler()->findFirstOfType(Mode::Type::VNA));
    QVERIFY(vna);

    QStringList output;
    auto conn = connect(window->getSCPI(), &SCPI::output, [&](QString line){
        output.append(line);
    });
    auto connCleanup = qScopeGuard([&](){ disconnect(conn); });
    auto command = [&](const QString &cmd){
        window->getSCPI()->input(cmd);
        QTest::qWait(20);
    };

    auto samplePath = dataDir + "/Water/sim-P1-DI_water-30c.s2p";
    auto sample = Touchstone::fromFile(samplePath.toStdString());
    QVERIFY(sample.points() > 0);

    command(":VNA:ACQ:POINTS "+QString::number(sample.points()));
    command(":VNA:ACQ:AVG 2");
    QTest::qWait(300);

    vna->getProbeSetup().fromJSON(probeConfig(dataDir, 22.5, "fileColumns", false));
    QVERIFY2(vna->getProbeSetup().valid(), qPrintable(vna->getProbeSetup().getLastError()));

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    auto acq = vna->getSampleAcquisition();
    QVERIFY(acq->arm(tmp.path(), "unittest-water30"));
    QCOMPARE((int) acq->getState(), (int) SampleAcquisition::State::Armed);

    auto feedSweep = [&](){
        for(unsigned int i = 0; i < sample.points(); i++) {
            DeviceDriver::VNAMeasurement m;
            m.pointNum = i;
            m.Z0 = 50.0;
            m.frequency = sample.point(i).frequency;
            m.dBm = -10.0;
            m.measurements["S11"] = sample.point(i).S[0];
            QMetaObject::invokeMethod(vna, "NewDatapoint", Qt::DirectConnection,
                                      Q_ARG(DeviceDriver::VNAMeasurement, m));
        }
        QTest::qWait(100);
    };

    // two full sweeps settle the averaging (still Armed afterwards), the
    // next incoming point triggers the single-sweep auto-stop
    feedSweep();
    QCOMPARE((int) acq->getState(), (int) SampleAcquisition::State::Armed);
    feedSweep();
    DeviceDriver::VNAMeasurement extra;
    extra.pointNum = 0;
    extra.Z0 = 50.0;
    extra.frequency = sample.point(0).frequency;
    extra.dBm = -10.0;
    extra.measurements["S11"] = sample.point(0).S[0];
    QMetaObject::invokeMethod(vna, "NewDatapoint", Qt::DirectConnection,
                              Q_ARG(DeviceDriver::VNAMeasurement, extra));
    QTest::qWait(200);

    QVERIFY2((int) acq->getState() == (int) SampleAcquisition::State::Done,
             qPrintable("state not Done: "+acq->getLastError()));
    auto path = acq->getLastFile();
    QVERIFY(QFileInfo::exists(path));

    // still a valid touchstone file with the sample's S11
    auto written = Touchstone::fromFile(path.toStdString());
    QCOMPARE(written.points(), sample.points());
    for(unsigned int i = 0; i < written.points(); i++) {
        QVERIFY(std::abs(written.point(i).S[0] - sample.point(i).S[0]) < 1e-9);
    }

    // metadata
    auto meta = ProbeSetup::readMetadata(path);
    QCOMPARE(meta["Sample"], QString("unittest-water30"));
    QCOMPARE(meta["Averages"], QString("2"));
    QVERIFY(meta.count("Timestamp"));

    // permittivity columns must match the golden numbers (the file stores
    // Perm_Imag positive, loadPermColumns returns eps' - j*eps'')
    std::vector<TraceMath::Data> written_eps, truth;
    QVERIFY(ProbeSetup::loadPermColumns(path, written_eps));
    QVERIFY(ProbeSetup::loadPermColumns(samplePath, truth));
    QCOMPARE(written_eps.size(), truth.size());
    double maeRe = 0.0, maeIm = 0.0;
    for(unsigned int i = 0; i < truth.size(); i++) {
        maeRe += std::abs(written_eps[i].y.real() - truth[i].y.real());
        maeIm += std::abs(written_eps[i].y.imag() - truth[i].y.imag());
    }
    maeRe /= truth.size();
    maeIm /= truth.size();
    QVERIFY2(std::abs(maeRe - 0.6472) <= maeTolerance, qPrintable(QString::number(maeRe)));
    QVERIFY2(std::abs(maeIm - 0.7541) <= maeTolerance, qPrintable(QString::number(maeIm)));
}
