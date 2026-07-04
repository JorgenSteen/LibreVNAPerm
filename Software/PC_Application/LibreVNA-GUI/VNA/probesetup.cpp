#include "probesetup.h"

#include "touchstone.h"
#include "appwindow.h"
#include "preferences.h"
#include "CustomWidgets/informationbox.h"
#include "ui_probesetupdialog.h"

#include <QDialog>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QRegularExpression>
#include <QSettings>

#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

using namespace std;

ProbeSetup::ProbeSetup()
{
    temperature = 22.5;
    epsSource = EpsSource::FileColumns;
    directoryMode = false;
    standardsLoaded = false;
    loadFromSettings();
}

const char *ProbeSetup::standardName(int standard)
{
    switch(standard) {
    case Air: return "air/open";
    case Water: return "water";
    case Saltwater: return "saltwater";
    default: return "invalid";
    }
}

std::complex<double> ProbeSetup::interpolateOrNaN(const std::vector<TraceMath::Data> &data, double x)
{
    if(data.empty() || x < data.front().x || x > data.back().x) {
        const double nan = numeric_limits<double>::quiet_NaN();
        return complex<double>(nan, nan);
    }
    return TraceMath::interpolatedSample(data, x);
}

QString ProbeSetup::temperatureSuffix(double tempC)
{
    return QString::number(tempC).replace('.', 'p') + "c";
}

bool ProbeSetup::resolveStandardsFromDirectory(const QString &dir, double tempC,
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

bool ProbeSetup::loadPermColumns(const QString &filename, std::vector<TraceMath::Data> &out)
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

bool ProbeSetup::loadStandardFiles(const std::array<QString, 3> &files,
                                   std::array<Standard, 3> &standards, QString &error)
{
    for(int i = 0; i < 3; i++) {
        standards[i].gamma.clear();
        standards[i].perm.clear();
    }
    for(int i = 0; i < 3; i++) {
        if(files[i].isEmpty()) {
            error = QString("No file configured for the ")+standardName(i)+" standard";
            return false;
        }
        try {
            auto t = Touchstone::fromFile(files[i].toStdString());
            if(t.points() == 0) {
                throw runtime_error("no data points in file");
            }
            for(unsigned int p = 0; p < t.points(); p++) {
                TraceMath::Data d;
                d.x = t.point(p).frequency;
                d.y = t.point(p).S[0]; // S11 is always the first parameter; extra columns of the custom format are ignored
                standards[i].gamma.push_back(d);
            }
        } catch (const exception &e) {
            error = QString("Failed to load the ")+standardName(i)+" standard: "+e.what();
            return false;
        }
        // optional extra permittivity columns (only present in the custom format)
        loadPermColumns(files[i], standards[i].perm);
    }
    error = QString();
    return true;
}

std::complex<double> ProbeSetup::knownEps(const std::array<Standard, 3> &standards, int standard,
                                          double freq, EpsSource source, double temperature)
{
    switch(source) {
    case EpsSource::FileColumns:
        // linear interpolation into the file's Perm columns, NaN outside/missing
        return interpolateOrNaN(standards[standard].perm, freq);
    case EpsSource::Models:
        switch(standard) {
        case Air: return PermittivityMath::airPermittivity();
        case Water: return PermittivityMath::waterDebye(freq, temperature);
        case Saltwater: return PermittivityMath::saltwaterColeCole(freq, temperature);
        }
        break;
    }
    const double nan = numeric_limits<double>::quiet_NaN();
    return complex<double>(nan, nan);
}

void ProbeSetup::edit()
{
    auto d = new QDialog();
    auto ui = new Ui::ProbeSetupDialog();
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
        invalidate();
        saveToSettings();
        emit settingsChanged();
    });
    if(AppWindow::showGUI()) {
        d->show();
    }
}

void ProbeSetup::exportDialog(const QString &device)
{
    bool ok = false;
    QString name = QInputDialog::getText(nullptr, "Export custom probe data",
                                         "Probe name (must not contain open/short/water/salt):",
                                         QLineEdit::Normal, "Probe1", &ok);
    if(!ok || name.isEmpty()) {
        return;
    }
    auto dir = QFileDialog::getExistingDirectory(nullptr, "Select destination directory for the probe data",
                                                 Preferences::getInstance().UISettings.Paths.data,
                                                 QFileDialog::ShowDirsOnly | Preferences::QFileDialogOptions());
    if(dir.isEmpty()) {
        return;
    }
    QString error;
    if(exportProbe(dir, name, device, error)) {
        InformationBox::ShowMessage("Export custom probe data", "Probe data exported to "+dir);
    } else {
        InformationBox::ShowError("Export custom probe data", error);
    }
}

bool ProbeSetup::exportProbe(const QString &destDir, const QString &probeName, const QString &device, QString &error)
{
    if(probeName.isEmpty()) {
        error = "No probe name given";
        return false;
    }
    const QString lower = probeName.toLower();
    for(const auto &token : {"open", "short", "water", "salt"}) {
        if(lower.contains(token)) {
            error = QString("The probe name must not contain \"")+token+"\", it would confuse the standard classification by filename";
            return false;
        }
    }
    QDir dest(destDir);
    if(!dest.exists()) {
        error = "Destination directory does not exist: "+destDir;
        return false;
    }
    QString base = probeName;
    base.replace(QRegularExpression("[^A-Za-z0-9._-]+"), "_");

    // the temperatures to export: every one found in the source directory
    // (directory mode) or the single configured one (individual files)
    vector<double> temps;
    if(directoryMode) {
        QRegularExpression tempRe("-(\\d+(?:p\\d+)?)c$");
        set<double> found;
        QDirIterator it(directory, {"*.s1p", "*.s2p"}, QDir::Files, QDirIterator::Subdirectories);
        while(it.hasNext()) {
            auto match = tempRe.match(QFileInfo(it.next()).completeBaseName().toLower());
            if(match.hasMatch()) {
                found.insert(QString(match.captured(1)).replace('p', '.').toDouble());
            }
        }
        temps.assign(found.begin(), found.end());
        if(temps.empty()) {
            error = "No temperature-suffixed standard files found in "+directory;
            return false;
        }
    } else {
        temps.push_back(temperature);
    }

    bool airWritten = false;
    unsigned int exported = 0;
    for(auto temp : temps) {
        std::array<QString, 3> src = files;
        std::array<Standard, 3> stds;
        if(directoryMode && !resolveStandardsFromDirectory(directory, temp, src, error)) {
            // no complete standard set at this temperature
            continue;
        }
        if(!loadStandardFiles(src, stds, error)) {
            return false;
        }
        if(!airWritten) {
            if(!writeStandardFile(dest.filePath(base+"-open.s2p"), Air, temp, probeName, device, stds[Air], epsSource, error)) {
                return false;
            }
            airWritten = true;
        }
        const QString suffix = temperatureSuffix(temp);
        if(!writeStandardFile(dest.filePath(base+"-water-"+suffix+".s2p"), Water, temp, probeName, device, stds[Water], epsSource, error)
                || !writeStandardFile(dest.filePath(base+"-saltwater-"+suffix+".s2p"), Saltwater, temp, probeName, device, stds[Saltwater], epsSource, error)) {
            return false;
        }
        exported++;
    }
    if(!exported) {
        // error still holds the last resolution failure
        error = "No complete standard set found to export ("+error+")";
        return false;
    }
    return true;
}

bool ProbeSetup::writeStandardFile(const QString &path, int standard, double tempC,
                                   const QString &probeName, const QString &device,
                                   const Standard &data, EpsSource source, QString &error)
{
    ofstream file(path.toStdString());
    if(!file.is_open()) {
        error = "Unable to create "+path;
        return false;
    }
    // Metadata as leading comment lines, then the column-naming info line
    // and the standard Touchstone option line. The data rows keep the
    // 9-column layout of the simulated data set: with 8 values per
    // frequency the file stays a valid 2-port Touchstone file (S11 in the
    // first data columns), the Perm columns ride in the S21 slot and the
    // remaining columns are unused filler.
    QString header;
    header += "! LibreVNA custom probe data\n";
    header += "! Probe: "+probeName+"\n";
    if(!device.isEmpty()) {
        header += "! Device: "+device+"\n";
    }
    header += QString("! Standard: ")+standardName(standard)+"\n";
    if(standard != Air) {
        header += "! Temperature: "+QString::number(tempC)+"\n";
    }
    header += QString("! ThirdStandard: ")+standardName(Saltwater)+"\n";
    header += "! S-Parameter data: F S11 e\n";
    header += "!   Frequency\tS11_Real\tS11_Imag\tPerm_Real\tPerm_Imag\tUnused\tUnused\tUnused\tUnused\n";
    header += "# Hz S RI R 50\n";
    file << header.toStdString();
    for(const auto &g : data.gamma) {
        complex<double> eps;
        switch(source) {
        case EpsSource::FileColumns:
            eps = interpolateOrNaN(data.perm, g.x);
            break;
        case EpsSource::Models:
            switch(standard) {
            case Air: eps = PermittivityMath::airPermittivity(); break;
            case Water: eps = PermittivityMath::waterDebye(g.x, tempC); break;
            case Saltwater: eps = PermittivityMath::saltwaterColeCole(g.x, tempC); break;
            }
            break;
        }
        if(isnan(eps.real())) {
            error = QString("No known eps* for the ")+standardName(standard)+" standard at "+QString::number(g.x)+" Hz (missing Perm columns?)";
            return false;
        }
        // Perm_Imag is stored positive in the file (internally eps* = eps' - j*eps'')
        file << QString::asprintf("%.9E\t%.9E\t%.9E\t%.9E\t%.9E\t0\t0\t0\t0\n",
                                  g.x, g.y.real(), g.y.imag(), eps.real(), -eps.imag()).toStdString();
    }
    if(!file.good()) {
        error = "Write error on "+path;
        return false;
    }
    return true;
}

bool ProbeSetup::valid()
{
    if(!ensureLoaded()) {
        return false;
    }
    if(epsSource == EpsSource::FileColumns) {
        for(int k = 0; k < 3; k++) {
            if(standards[k].perm.empty()) {
                lastError = QString("The ")+standardName(k)+" standard file has no permittivity columns, use reference models instead";
                return false;
            }
        }
    }
    return true;
}

std::complex<double> ProbeSetup::compute(double freq, std::complex<double> S11)
{
    const double nan = numeric_limits<double>::quiet_NaN();
    const complex<double> NaN(nan, nan);
    if(!valid()) {
        return NaN;
    }
    PermittivityMath::BilinearCoefficients coeff;
    auto it = coeffCache.find(freq);
    if(it != coeffCache.end()) {
        coeff = it->second;
    } else {
        array<complex<double>, 3> gamma, eps;
        bool inRange = true;
        for(int k = 0; k < 3; k++) {
            gamma[k] = interpolateOrNaN(standards[k].gamma, freq);
            eps[k] = knownEps(standards, k, freq, epsSource, temperature);
            if(isnan(gamma[k].real()) || isnan(eps[k].real())) {
                // outside of this standard's frequency coverage
                inRange = false;
            }
        }
        if(inRange) {
            coeff = PermittivityMath::solveBilinearCoefficients(gamma, eps);
        } else {
            coeff.A = coeff.B = coeff.C = NaN;
            coeff.conditionNumber = numeric_limits<double>::quiet_NaN();
        }
        // the cache grows by one entry per distinct frequency ever swept;
        // reset it should that ever get out of hand
        if(coeffCache.size() > 1000000) {
            coeffCache.clear();
        }
        coeffCache[freq] = coeff;
    }
    if(isnan(coeff.conditionNumber)) {
        return NaN;
    }
    auto epsSample = PermittivityMath::applyBilinear(S11, coeff);
    // internal convention is eps' - j*eps''; return the conjugate so that
    // "Real" displays eps' and "Imag" displays eps'' (both positive)
    return conj(epsSample);
}

bool ProbeSetup::ensureLoaded()
{
    if(standardsLoaded) {
        return true;
    }
    if(directoryMode && !resolveStandardsFromDirectory(directory, temperature, files, lastError)) {
        return false;
    }
    if(!loadStandardFiles(files, standards, lastError)) {
        return false;
    }
    standardsLoaded = true;
    return true;
}

void ProbeSetup::invalidate()
{
    standardsLoaded = false;
    coeffCache.clear();
}

void ProbeSetup::saveToSettings()
{
    QSettings settings;
    settings.setValue("ProbeSetup/airFile", files[Air]);
    settings.setValue("ProbeSetup/waterFile", files[Water]);
    settings.setValue("ProbeSetup/saltwaterFile", files[Saltwater]);
    settings.setValue("ProbeSetup/temperature", temperature);
    settings.setValue("ProbeSetup/epsSource", (int) epsSource);
    settings.setValue("ProbeSetup/directoryMode", directoryMode);
    settings.setValue("ProbeSetup/directory", directory);
}

void ProbeSetup::loadFromSettings()
{
    QSettings settings;
    files[Air] = settings.value("ProbeSetup/airFile", files[Air]).toString();
    files[Water] = settings.value("ProbeSetup/waterFile", files[Water]).toString();
    files[Saltwater] = settings.value("ProbeSetup/saltwaterFile", files[Saltwater]).toString();
    temperature = settings.value("ProbeSetup/temperature", temperature).toDouble();
    epsSource = (EpsSource) settings.value("ProbeSetup/epsSource", (int) epsSource).toInt();
    directoryMode = settings.value("ProbeSetup/directoryMode", directoryMode).toBool();
    directory = settings.value("ProbeSetup/directory", directory).toString();
}

nlohmann::json ProbeSetup::toJSON()
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

void ProbeSetup::fromJSON(nlohmann::json j)
{
    files[Air] = QString::fromStdString(j.value("airFile", files[Air].toStdString()));
    files[Water] = QString::fromStdString(j.value("waterFile", files[Water].toStdString()));
    files[Saltwater] = QString::fromStdString(j.value("saltwaterFile", files[Saltwater].toStdString()));
    temperature = j.value("temperature", 22.5);
    epsSource = j.value("epsSource", string("fileColumns")) == string("models") ? EpsSource::Models : EpsSource::FileColumns;
    directoryMode = j.value("directoryMode", false);
    directory = QString::fromStdString(j.value("directory", ""));
    invalidate();
    emit settingsChanged();
}
