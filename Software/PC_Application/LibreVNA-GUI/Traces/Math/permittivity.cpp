#include "permittivity.h"

#include "touchstone.h"
#include "unit.h"
#include "appwindow.h"
#include "preferences.h"
#include "ui_permittivitydialog.h"
#include "ui_permittivityexplanationwidget.h"

#include <QDialog>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QWidget>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

using namespace Math;
using namespace std;

Permittivity::Permittivity()
{
    temperature = 22.5;
    epsSource = EpsSource::FileColumns;
    directoryMode = false;
    standardsLoaded = false;
}

QString Permittivity::temperatureSuffix(double tempC)
{
    return QString::number(tempC).replace('.', 'p') + "c";
}

bool Permittivity::resolveStandardsFromDirectory(const QString &dir, double tempC,
                                                 std::array<QString, 3> &resolved, QString &error)
{
    if(dir.isEmpty()) {
        error = "No directory configured";
        return false;
    }
    if(!QFileInfo(dir).isDir()) {
        error = "Not a directory: " + dir;
        return false;
    }
    const QString suffix = temperatureSuffix(tempC);
    resolved = {QString(), QString(), QString()};
    QDirIterator it(dir, {"*.s1p", "*.s2p"}, QDir::Files, QDirIterator::Subdirectories);
    while(it.hasNext()) {
        const QString path = it.next();
        const QString base = QFileInfo(path).completeBaseName().toLower();
        int standard = -1;
        if(base.contains("salt")) {
            if(base.endsWith("-" + suffix)) {
                standard = Saltwater;
            }
        } else if(base.contains("water")) {
            if(base.endsWith("-" + suffix)) {
                standard = Water;
            }
        } else if(base.contains("open") && !base.contains("short")) {
            standard = Air;
        }
        if(standard >= 0) {
            if(!resolved[standard].isEmpty()) {
                error = QString("Ambiguous ") + standardName(standard) + " standard: both "
                        + QFileInfo(resolved[standard]).fileName() + " and " + QFileInfo(path).fileName() + " match";
                return false;
            }
            resolved[standard] = path;
        }
    }
    for(int i = 0; i < 3; i++) {
        if(resolved[i].isEmpty()) {
            error = QString("No ") + standardName(i) + " standard found";
            if(i != Air) {
                error += " for " + QString::number(tempC) + "°C (filename suffix \"-" + suffix + "\")";
            }
            error += " in " + dir;
            return false;
        }
    }
    error = QString();
    return true;
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
        if(resolveStandardsFromDirectory(ui->directory->text(), ui->temperature->value(), resolved, err)) {
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
        auto dir = QFileDialog::getExistingDirectory(nullptr, "Select standards directory", start,
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

const char *Permittivity::standardName(int standard)
{
    switch(standard) {
    case Air: return "air/open";
    case Water: return "water";
    case Saltwater: return "saltwater";
    default: return "invalid";
    }
}

// Parse the extra permittivity columns of the project's custom touchstone
// files: freq S11_re S11_im Perm_re Perm_im [...]. Perm_im is stored positive,
// internally eps* = Perm_re - j*Perm_im. Returns false (empty out) if the file
// has no such columns.
static bool loadPermColumns(const QString &filename, std::vector<TraceMath::Data> &out)
{
    out.clear();
    ifstream file(filename.toStdString());
    if(!file.is_open()) {
        return false;
    }
    double freqMultiplier = 1e9; // touchstone default unit is GHz
    bool optionLineFound = false;
    string line;
    while(getline(file, line)) {
        // remove comments and leading whitespace
        auto comment = line.find_first_of('!');
        if(comment != string::npos) {
            line.erase(comment);
        }
        auto first = line.find_first_not_of(" \t\r\n");
        if(first == string::npos) {
            continue;
        }
        line.erase(0, first);
        if(line[0] == '#') {
            if(optionLineFound) {
                break;
            }
            optionLineFound = true;
            transform(line.begin(), line.end(), line.begin(), ::toupper);
            if(line.find("HZ") != string::npos) {
                if(line.find("KHZ") != string::npos) {
                    freqMultiplier = 1e3;
                } else if(line.find("MHZ") != string::npos) {
                    freqMultiplier = 1e6;
                } else if(line.find("GHZ") != string::npos) {
                    freqMultiplier = 1e9;
                } else {
                    freqMultiplier = 1.0;
                }
            }
            continue;
        }
        if(!optionLineFound) {
            continue;
        }
        istringstream iss(line);
        double freq, s11_re, s11_im, perm_re, perm_im;
        if(iss >> freq >> s11_re >> s11_im >> perm_re >> perm_im) {
            TraceMath::Data d;
            d.x = freq * freqMultiplier;
            d.y = complex<double>(perm_re, -perm_im);
            out.push_back(d);
        } else {
            // file does not have the extra permittivity columns
            out.clear();
            return false;
        }
    }
    return !out.empty();
}

bool Permittivity::loadStandards()
{
    for(int i = 0; i < 3; i++) {
        standards[i].gamma.clear();
        standards[i].perm.clear();
    }
    if(directoryMode && !resolveStandardsFromDirectory(directory, temperature, files, loadError)) {
        return false;
    }
    for(int i = 0; i < 3; i++) {
        if(files[i].isEmpty()) {
            loadError = QString("No file configured for the ")+standardName(i)+" standard";
            return false;
        }
        try {
            auto t = Touchstone::fromFile(files[i].toStdString());
            if(t.points() == 0) {
                throw runtime_error("no data points in file");
            }
            for(unsigned int p = 0; p < t.points(); p++) {
                Data d;
                d.x = t.point(p).frequency;
                d.y = t.point(p).S[0]; // S11 is always the first parameter; extra columns of the custom format are ignored
                standards[i].gamma.push_back(d);
            }
        } catch (const exception &e) {
            loadError = QString("Failed to load the ")+standardName(i)+" standard: "+e.what();
            return false;
        }
        // optional extra permittivity columns (only present in the custom format)
        loadPermColumns(files[i], standards[i].perm);
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

std::complex<double> Permittivity::knownEps(int standard, double freq)
{
    switch(epsSource) {
    case EpsSource::FileColumns:
        // linear interpolation into the file's Perm columns, NaN outside/missing
        return interpolatedSample(standards[standard].perm, freq);
    case EpsSource::Models:
        switch(standard) {
        case Air: return PermittivityMath::airPermittivity();
        case Water: return PermittivityMath::waterDebye(freq, temperature);
        case Saltwater: return PermittivityMath::saltwaterColeCole(freq, temperature);
        }
        break;
    }
    return numeric_limits<complex<double>>::quiet_NaN();
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
                error(QString("The ")+standardName(k)+" standard file has no permittivity columns, use reference models instead");
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
            gamma[k] = interpolatedSample(standards[k].gamma, f);
            eps[k] = knownEps(k, f);
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
            data[i].y = numeric_limits<complex<double>>::quiet_NaN();
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
