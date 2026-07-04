#include "permittivity.h"

#include "unit.h"
#include "appwindow.h"
#include "preferences.h"
#include "ui_permittivitydialog.h"
#include "ui_permittivityexplanationwidget.h"

#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QWidget>
#include <cmath>
#include <limits>

using namespace Math;
using namespace std;

Permittivity::Permittivity()
{
    temperature = 22.5;
    epsSource = EpsSource::FileColumns;
    directoryMode = false;
    standardsLoaded = false;
}

TraceMath::DataType Permittivity::outputType(TraceMath::DataType inputType)
{
    // only frequency-domain S11 makes sense as input; the output is
    // permittivity over the same frequency axis
    if(inputType == DataType::Frequency) {
        return DataType::Frequency;
    } else {
        return DataType::Invalid;
    }
}

QString Permittivity::description()
{
    if(directoryMode ? directory.isEmpty()
                     : (files[Air].isEmpty() || files[Water].isEmpty() || files[Saltwater].isEmpty())) {
        return "Permittivity (bilinear), standards not configured";
    }
    QString source = epsSource == EpsSource::FileColumns ? "file Perm columns" : "reference models";
    QString standards = directoryMode ? "standards from directory" : "standards";
    return "Permittivity (bilinear), "+standards+" at "+QString::number(temperature)+"°C, ε* from "+source;
}

bool Permittivity::initFromProbeSetup()
{
    auto setup = ProbeSetup::getCurrent();
    if(!setup || !setup->getConfig(files, directoryMode, directory, temperature, epsSource)) {
        return false;
    }
    configurationChanged();
    return true;
}

void Permittivity::edit()
{
    auto d = new QDialog();
    auto ui = new Ui::PermittivityDialog();
    ui->setupUi(d);
    connect(d, &QDialog::finished, [=](){
        delete ui;
        d->deleteLater();
    });

    ui->airFile->setFile(files[Air]);
    ui->waterFile->setFile(files[Water]);
    ui->saltwaterFile->setFile(files[Saltwater]);
    ui->temperature->setValue(temperature);
    ui->epsSource->setCurrentIndex((int) epsSource);
    ui->individualMode->setChecked(!directoryMode);
    ui->directoryModeButton->setChecked(directoryMode);
    ui->modeStack->setCurrentIndex(directoryMode ? 1 : 0);
    ui->directory->setText(directory);

    connect(ui->individualMode, &QRadioButton::toggled, [=](bool checked) {
        ui->modeStack->setCurrentIndex(checked ? 0 : 1);
    });
    // live preview of the files the directory + temperature resolve to
    auto updatePreview = [=](){
        std::array<QString, 3> resolved;
        QString err;
        if(ProbeSetup::resolveStandardsFromDirectory(ui->directory->text(), ui->temperature->value(), resolved, err)) {
            ui->resolvedPreview->setText("Open (air): "+QFileInfo(resolved[Air]).fileName()
                    +"\nWater: "+QFileInfo(resolved[Water]).fileName()
                    +"\nSaltwater: "+QFileInfo(resolved[Saltwater]).fileName());
        } else {
            ui->resolvedPreview->setText(err);
        }
    };
    connect(ui->directory, &QLineEdit::textChanged, updatePreview);
    connect(ui->temperature, qOverload<double>(&QDoubleSpinBox::valueChanged), updatePreview);
    connect(ui->browseDirectory, &QPushButton::clicked, [=](){
        QString start = ui->directory->text().isEmpty() ? Preferences::getInstance().UISettings.Paths.data
                                                        : ui->directory->text();
        auto dir = QFileDialog::getExistingDirectory(d, "Select standards directory", start,
                                                     QFileDialog::ShowDirsOnly | Preferences::QFileDialogOptions());
        if(!dir.isEmpty()) {
            ui->directory->setText(dir);
        }
    });
    updatePreview();

    connect(d, &QDialog::accepted, [=](){
        directoryMode = ui->directoryModeButton->isChecked();
        directory = ui->directory->text();
        if(!directoryMode) {
            files[Air] = ui->airFile->getFilename();
            files[Water] = ui->waterFile->getFilename();
            files[Saltwater] = ui->saltwaterFile->getFilename();
        }
        temperature = ui->temperature->value();
        epsSource = (EpsSource) ui->epsSource->currentIndex();
        configurationChanged();
    });
    if(AppWindow::showGUI()) {
        d->show();
    }
}

bool Permittivity::loadStandards()
{
    if(directoryMode && !ProbeSetup::resolveStandardsFromDirectory(directory, temperature, files, loadError)) {
        return false;
    }
    if(!ProbeSetup::loadStandardFiles(files, standards, loadError)) {
        return false;
    }
    standardsLoaded = true;
    return true;
}

void Permittivity::configurationChanged()
{
    standardsLoaded = false;
    if(input) {
        inputSamplesChanged(0, input->numSamples());
    }
}

void Permittivity::inputSamplesChanged(unsigned int begin, unsigned int end)
{
    if(!standardsLoaded && !loadStandards()) {
        dataMutex.lock();
        data.clear();
        condNumbers.clear();
        dataMutex.unlock();
        error(loadError);
        emit outputSamplesChanged(0, 0);
        return;
    }
    if(epsSource == EpsSource::FileColumns) {
        for(int k = 0; k < 3; k++) {
            if(standards[k].perm.empty()) {
                dataMutex.lock();
                data.clear();
                condNumbers.clear();
                dataMutex.unlock();
                error(QString("The ")+ProbeSetup::standardName(k)+" standard file has no permittivity columns, use reference models instead");
                emit outputSamplesChanged(0, 0);
                return;
            }
        }
    }
    vector<Data> in;
    if(input) {
        in = input->getData();
    }
    dataMutex.lock();
    data.resize(in.size());
    condNumbers.resize(in.size(), numeric_limits<double>::quiet_NaN());
    if(end > in.size()) {
        end = in.size();
    }
    if(in.empty()) {
        dataMutex.unlock();
        warning("No input data");
        emit outputSamplesChanged(0, 0);
        return;
    }
    if(end <= begin) {
        dataMutex.unlock();
        return;
    }
    for(unsigned int i = begin; i < end; i++) {
        const double f = in[i].x;
        data[i].x = f;
        array<complex<double>, 3> gamma, eps;
        bool valid = true;
        for(int k = 0; k < 3; k++) {
            gamma[k] = ProbeSetup::interpolateOrNaN(standards[k].gamma, f);
            eps[k] = ProbeSetup::knownEps(standards, k, f, epsSource, temperature);
            if(isnan(gamma[k].real()) || isnan(eps[k].real())) {
                // outside of this standard's frequency coverage
                valid = false;
            }
        }
        if(valid) {
            auto coeff = PermittivityMath::solveBilinearCoefficients(gamma, eps);
            condNumbers[i] = coeff.conditionNumber;
            auto epsSample = PermittivityMath::applyBilinear(in[i].y, coeff);
            // internal convention is eps' - j*eps''; store the conjugate so that
            // "Real" displays eps' and "Imag" displays eps'' (both positive)
            data[i].y = conj(epsSample);
        } else {
            data[i].y = complex<double>(numeric_limits<double>::quiet_NaN(), numeric_limits<double>::quiet_NaN());
            condNumbers[i] = numeric_limits<double>::quiet_NaN();
        }
    }
    // evaluate the status over the complete output
    constexpr double condWarningThreshold = 1e6;
    unsigned int invalidPoints = 0, illConditionedPoints = 0;
    double validMin = numeric_limits<double>::quiet_NaN(), validMax = numeric_limits<double>::quiet_NaN();
    for(unsigned int i = 0; i < data.size(); i++) {
        if(isnan(condNumbers[i])) {
            invalidPoints++;
        } else {
            if(isnan(validMin)) {
                validMin = data[i].x;
            }
            validMax = data[i].x;
            if(condNumbers[i] > condWarningThreshold) {
                illConditionedPoints++;
            }
        }
    }
    auto points = data.size();
    dataMutex.unlock();
    emit outputSamplesChanged(begin, end);
    if(invalidPoints == points) {
        error("No frequency overlap between the input trace and the calibration standards");
    } else if(invalidPoints > 0) {
        warning("Standards only cover "+Unit::ToString(validMin, "Hz", " kMG", 4)+" to "+Unit::ToString(validMax, "Hz", " kMG", 4)+", "+QString::number(invalidPoints)+" points invalid");
    } else if(illConditionedPoints > 0) {
        warning(QString::number(illConditionedPoints)+" points with badly conditioned standards (too close in the Γ-plane), result unreliable there");
    } else {
        success();
    }
}

QWidget *Permittivity::createExplanationWidget()
{
    auto w = new QWidget();
    auto ui = new Ui::PermittivityExplanationWidget;
    ui->setupUi(w);
    QObject::connect(w, &QWidget::destroyed, [=](){
        delete ui;
    });
    return w;
}

nlohmann::json Permittivity::toJSON()
{
    nlohmann::json j;
    j["airFile"] = files[Air].toStdString();
    j["waterFile"] = files[Water].toStdString();
    j["saltwaterFile"] = files[Saltwater].toStdString();
    j["temperature"] = temperature;
    j["epsSource"] = epsSource == EpsSource::FileColumns ? "fileColumns" : "models";
    j["directoryMode"] = directoryMode;
    j["directory"] = directory.toStdString();
    return j;
}

void Permittivity::fromJSON(nlohmann::json j)
{
    files[Air] = QString::fromStdString(j.value("airFile", files[Air].toStdString()));
    files[Water] = QString::fromStdString(j.value("waterFile", files[Water].toStdString()));
    files[Saltwater] = QString::fromStdString(j.value("saltwaterFile", files[Saltwater].toStdString()));
    temperature = j.value("temperature", 22.5);
    epsSource = j.value("epsSource", string("fileColumns")) == string("models") ? EpsSource::Models : EpsSource::FileColumns;
    directoryMode = j.value("directoryMode", false);
    directory = QString::fromStdString(j.value("directory", ""));
    configurationChanged();
}
