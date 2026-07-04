#include "utiltests.h"
#include "portextensiontests.h"
#include "parametertests.h"
#include "ffttests.h"
#include "impedancerenormalizationtests.h"
#include "calibrationtests.h"
#include "probesetuptests.h"

#include <QtTest>

int main(int argc, char *argv[])
{
    // Some tests construct an AppWindow, which parses the application
    // arguments (and exits on options it does not know, like the QTest
    // ones). Give the application a fixed argument list with the GUI
    // disabled and keep the real one for the test framework.
    static int appArgc = 2;
    static char appName[] = "LibreVNA-Test";
    static char appNoGui[] = "--no-gui";
    static char *appArgv[] = {appName, appNoGui, nullptr};
    QApplication a(appArgc, appArgv);

    int status = 0;
    status |= QTest::qExec(new UtilTests, argc, argv);
    status |= QTest::qExec(new PortExtensionTests, argc, argv);
    status |= QTest::qExec(new ParameterTests, argc, argv);
    status |= QTest::qExec(new fftTests, argc, argv);
    status |= QTest::qExec(new ImpedanceRenormalizationTests, argc, argv);
    status |= QTest::qExec(new CalibrationTests, argc, argv);
    status |= QTest::qExec(new ProbeSetupTests, argc, argv);

    return status;
}
