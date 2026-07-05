#include "sampleacquisition.h"

#include "vna.h"
#include "probesetup.h"
#include "appwindow.h"
#include "preferences.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

using namespace std;

SampleAcquisition::SampleAcquisition(VNA *vna)
    : QObject(vna),
      vna(vna),
      state(State::Idle),
      light(nullptr),
      lastPointNum(-1)
{
    statusMsg = "No sample acquisition set up";
}

QWidget *SampleAcquisition::createToolWidget()
{
    auto w = new QWidget;
    auto layout = new QHBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    auto btn = new QPushButton("Acquire sample...");
    btn->setToolTip("Acquire an averaged single sweep and save it as a sample file");
    light = new QLabel(w);
    light->setFixedSize(14, 14);
    layout->addWidget(btn);
    layout->addWidget(light);
    connect(btn, &QPushButton::clicked, this, &SampleAcquisition::showDialog);
    connect(this, &SampleAcquisition::stateChanged, this, &SampleAcquisition::updateLight);
    updateLight();
    return w;
}

void SampleAcquisition::showDialog()
{
    auto d = new QDialog;
    d->setWindowTitle("Acquire sample");
    d->setWindowModality(Qt::ApplicationModal);
    d->setAttribute(Qt::WA_DeleteOnClose);

    QSettings settings;
    auto lastDir = settings.value("SampleAcquisition/directory",
                                  Preferences::getInstance().UISettings.Paths.data).toString();
    auto lastName = settings.value("SampleAcquisition/name", "sample").toString();

    auto layout = new QVBoxLayout(d);

    auto nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel("Sample name:"));
    auto nameEdit = new QLineEdit(lastName);
    nameRow->addWidget(nameEdit);
    layout->addLayout(nameRow);

    auto dirRow = new QHBoxLayout;
    dirRow->addWidget(new QLabel("Save to:"));
    auto dirEdit = new QLineEdit(lastDir);
    auto browse = new QPushButton("...");
    browse->setFixedWidth(30);
    dirRow->addWidget(dirEdit);
    dirRow->addWidget(browse);
    layout->addLayout(dirRow);

    auto info = new QLabel;
    info->setWordWrap(true);
    layout->addWidget(info);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    auto updateInfo = [=]() {
        auto file = nameEdit->text().trimmed();
        if(!file.isEmpty() && !file.toLower().endsWith(".s2p")) {
            file += ".s2p";
        }
        info->setText("Starts a single sweep with the configured averaging and saves \""
                      +file+"\" (S11 + permittivity columns) when the sweep completes.");
        buttons->button(QDialogButtonBox::Ok)->setEnabled(
                    !nameEdit->text().trimmed().isEmpty() && !dirEdit->text().trimmed().isEmpty());
    };
    connect(nameEdit, &QLineEdit::textChanged, updateInfo);
    connect(dirEdit, &QLineEdit::textChanged, updateInfo);
    updateInfo();

    connect(browse, &QPushButton::clicked, [=]() {
        auto dir = QFileDialog::getExistingDirectory(d, "Select sample directory", dirEdit->text());
        if(!dir.isEmpty()) {
            dirEdit->setText(dir);
        }
    });

    connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, [=]() {
        QSettings settings;
        settings.setValue("SampleAcquisition/directory", dirEdit->text().trimmed());
        settings.setValue("SampleAcquisition/name", nameEdit->text().trimmed());
        d->accept();
        arm(dirEdit->text().trimmed(), nameEdit->text().trimmed());
    });

    if(AppWindow::showGUI()) {
        d->show();
    }
}

bool SampleAcquisition::arm(const QString &directory, const QString &name)
{
    this->directory = directory;
    this->name = name;
    gamma.clear();
    perm.clear();
    lastPointNum = -1;
    lastFile.clear();
    lastError.clear();
    if(!vna->restartSingleSweep()) {
        setState(State::Failed, "Unable to start the sweep (calibration measurement in progress?)");
        return false;
    }
    setState(State::Armed, "Waiting for the averaged sweep to complete...");
    return true;
}

void SampleAcquisition::addDatapoint(const DeviceDriver::VNAMeasurement &m)
{
    if(state != State::Armed) {
        return;
    }
    if(!m.measurements.count("S11")) {
        return;
    }
    auto pointNum = m.pointNum;
    if((int) pointNum <= lastPointNum) {
        // new sweep started; discard the collector if the previous sweep did
        // not finish cleanly (partial sweep or changed point count)
        if(lastPointNum != (int) gamma.size() - 1) {
            gamma.clear();
            perm.clear();
        }
    }
    if(pointNum >= gamma.size()) {
        auto nan = numeric_limits<double>::quiet_NaN();
        TraceMath::Data unset;
        unset.x = 0.0;
        unset.y = complex<double>(nan, nan);
        gamma.resize(pointNum + 1);
        perm.resize(pointNum + 1, unset);
    }
    gamma[pointNum].x = m.frequency;
    gamma[pointNum].y = m.measurements.at("S11");
    perm[pointNum].x = m.frequency;
    if(m.measurements.count("PERMITTIVITY")) {
        perm[pointNum].y = m.measurements.at("PERMITTIVITY");
    } else {
        auto nan = numeric_limits<double>::quiet_NaN();
        perm[pointNum].y = complex<double>(nan, nan);
    }
    lastPointNum = pointNum;
}

void SampleAcquisition::sweepFinished(bool settled, double scatter, unsigned int averages, const QString &device)
{
    if(state != State::Armed) {
        return;
    }
    if(gamma.empty()) {
        // spurious stop before any data arrived (e.g. no device connected
        // yet) -- stay armed, the acquisition starts with the sweep
        return;
    }
    if(!settled || lastPointNum != (int) gamma.size() - 1) {
        setState(State::Failed, "Acquisition stopped before the averaging settled, no file written");
        return;
    }
    auto file = name;
    if(!file.toLower().endsWith(".s2p")) {
        file += ".s2p";
    }
    auto path = directory + "/" + file;
    // temperature from the probe setup, if one is configured
    auto nan = numeric_limits<double>::quiet_NaN();
    double tempC = nan;
    std::array<QString, 3> files;
    bool dirMode;
    QString dir;
    ProbeSetup::EpsSource source;
    if(vna->getProbeSetup().getConfig(files, dirMode, dir, tempC, source) == false) {
        tempC = nan;
    }
    QString error;
    if(!ProbeSetup::writeSampleFile(path, name, device, tempC, averages, scatter, gamma, perm, error)) {
        lastError = error;
        setState(State::Failed, error);
        return;
    }
    lastFile = path;
    setState(State::Done, "Saved "+path);
}

void SampleAcquisition::setState(State s, const QString &status)
{
    state = s;
    statusMsg = status;
    emit stateChanged();
}

void SampleAcquisition::updateLight()
{
    if(!light) {
        return;
    }
    QString color;
    switch(state) {
    case State::Idle: color = "#404040"; break;
    case State::Armed: color = "#e8c000"; break;
    case State::Done: color = "#28b428"; break;
    case State::Failed: color = "#d02828"; break;
    }
    light->setStyleSheet("background-color: "+color+"; border-radius: 7px; border: 1px solid #202020;");
    light->setToolTip(statusMsg);
}
