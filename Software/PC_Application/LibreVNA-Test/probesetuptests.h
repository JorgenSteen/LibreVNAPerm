#ifndef PROBESETUPTESTS_H
#define PROBESETUPTESTS_H

#include <QtTest>

/*
 * Verifies ProbeSetup::compute against the golden numbers established with
 * the Python reference implementation (water-30c sample of the simulated
 * data set). The data set lives outside the repository; the tests are
 * skipped when it is not found (set PERMITTIVITY_TEST_DATA to its root to
 * override the default search).
 */
class ProbeSetupTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void computeFileColumns();
    void computeModels();
    void computeDirectoryMode10C();
    void computeOutsideCoverage();
    void exportRoundtrip();
    void probeLibraryImport();

private:
    QString dataDir;
};

#endif // PROBESETUPTESTS_H
