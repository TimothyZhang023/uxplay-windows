#include "mainwindow.h"
#include "mdns_responder.hpp"
#include "single_instance.hpp"
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>

#include <QProcess>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QComboBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QTextStream>

struct RenameData {
    DWORD pid;
    QString newTitle;
};

namespace {
constexpr DWORD kServiceMissing = 0;
constexpr DWORD kBonjourStartTimeoutMs = 15000;
constexpr DWORD kBonjourPollMs = 250;

DWORD queryWindowsServiceState(const std::wstring &serviceName) {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        qDebug() << "OpenSCManagerW failed while checking Bonjour. Error=" << GetLastError();
        return kServiceMissing;
    }

    SC_HANDLE hSvc = OpenServiceW(hSCM, serviceName.c_str(), SERVICE_QUERY_STATUS);
    if (!hSvc) {
        DWORD err = GetLastError();
        CloseServiceHandle(hSCM);
        if (err != ERROR_SERVICE_DOES_NOT_EXIST) {
            qDebug() << "OpenServiceW failed while checking Bonjour. Error=" << err;
        }
        return kServiceMissing;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    DWORD state = kServiceMissing;
    if (QueryServiceStatusEx(
            hSvc,
            SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),
            sizeof(status),
            &bytesNeeded)) {
        state = status.dwCurrentState;
    } else {
        qDebug() << "QueryServiceStatusEx failed for Bonjour. Error=" << GetLastError();
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return state;
}

bool waitForWindowsServiceState(const std::wstring &serviceName,
                                DWORD desiredState,
                                DWORD timeoutMs = kBonjourStartTimeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        if (queryWindowsServiceState(serviceName) == desiredState) {
            return true;
        }
        Sleep(kBonjourPollMs);
    } while (GetTickCount64() < deadline);

    return queryWindowsServiceState(serviceName) == desiredState;
}

bool startWindowsServiceNormally(const std::wstring &serviceName) {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) {
        qDebug() << "OpenSCManagerW failed while starting Bonjour. Error=" << GetLastError();
        return false;
    }

    SC_HANDLE hSvc = OpenServiceW(
        hSCM,
        serviceName.c_str(),
        SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hSvc) {
        qDebug() << "OpenServiceW failed while starting Bonjour. Error=" << GetLastError();
        CloseServiceHandle(hSCM);
        return false;
    }

    BOOL ok = StartServiceW(hSvc, 0, nullptr);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);

    if (ok || err == ERROR_SERVICE_ALREADY_RUNNING) {
        return true;
    }

    qDebug() << "StartServiceW failed for Bonjour. Error=" << err;
    return false;
}

bool startWindowsServiceElevated(const std::wstring &serviceName) {
    std::wstring params = L"start \"" + serviceName + L"\"";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"sc.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        qDebug() << "ShellExecuteExW(runas sc start Bonjour) failed. Error=" << GetLastError();
        return false;
    }

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, kBonjourStartTimeoutMs);
        CloseHandle(sei.hProcess);
    }

    return waitForWindowsServiceState(serviceName, SERVICE_RUNNING, 10000);
}

QString bonjourStateText(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING: return "running";
    case SERVICE_START_PENDING: return "starting";
    case SERVICE_STOP_PENDING: return "stopping";
    case SERVICE_STOPPED: return "stopped";
    case SERVICE_PAUSED: return "paused";
    case kServiceMissing: return "missing";
    default: return QString("state %1").arg(state);
    }
}

QString stableDeviceId() {
    QSettings settings;
    QString deviceId = settings.value("device_id").toString().toLower();
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9a-f]{2}(:[0-9a-f]{2}){5}$"));
    if (pattern.match(deviceId).hasMatch()) {
        return deviceId;
    }

    quint8 bytes[6];
    for (quint8 &byte : bytes) {
        byte = static_cast<quint8>(QRandomGenerator::system()->generate() & 0xff);
    }
    bytes[0] = static_cast<quint8>((bytes[0] | 0x02) & 0xfe); // local, unicast

    QStringList parts;
    parts.reserve(6);
    for (quint8 byte : bytes) {
        parts << QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0'));
    }
    deviceId = parts.join(':');
    settings.setValue("device_id", deviceId);
    qInfo() << "Generated persistent AirPlay device ID:" << deviceId;
    return deviceId;
}
} // namespace

// Callback method that Windows calls for every opened window
BOOL CALLBACK EnumWindowsProcRename(HWND hwnd, LPARAM lParam) {
    RenameData *data = reinterpret_cast<RenameData*>(lParam);
    DWORD windowPid;
    GetWindowThreadProcessId(hwnd, &windowPid);

    if (windowPid == data->pid) {
        char windowTitle[512];
        if (GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle))) {
            QString title = QString::fromLocal8Bit(windowTitle);
            
            if (title.contains("Direct") && title.contains("enderer")) {
                printf("found window to rename, setting new name...\n");
                SetWindowTextW(hwnd, reinterpret_cast<const wchar_t*>(data->newTitle.utf16()));
                return FALSE;
            }
        }
    }
    return TRUE;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    ensureSettingsFileExists();
    setupTray();
    setupUI();

    // Bonjour is preferred, but it is not a hard startup dependency. If it is
    // unavailable, force BLE for this session so libuxplay keeps serving.
    m_bonjourAvailable = ensureBonjourServiceAvailable(false);
    m_forceBleFallback = !m_bonjourAvailable;
    startServer();

    if (!m_bonjourAvailable) {
        QTimer::singleShot(500, this, &MainWindow::showDiscoveryFallbackWarning);
    }
}

MainWindow::~MainWindow() {
    m_quitting = true;
    stopServer();
}

void MainWindow::ensureSettingsFileExists() {
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir(appDataPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QString filePath = appDataPath + "/arguments.txt";
    QFile file(filePath);
    if (!file.exists()) {
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << "-n uxplay-windows -nh";
            file.close();
        }
    }
}

QStringList MainWindow::getArgumentsFromFile() {
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QFile file(appDataPath + "/arguments.txt");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString content = QTextStream(&file).readAll().trimmed();
        file.close();
        return QProcess::splitCommand(content);
    }
    return QStringList() << "-n" << "uxplay-windows" << "-nh";
}

void MainWindow::setupUI() {
    setWindowTitle("uxplay-windows");
    setWindowIcon(QApplication::windowIcon());
    setFixedSize(300, 260);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *layout = new QVBoxLayout(central);

    m_statusLabel = new QLabel("Initializing...", this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_statusLabel);

    m_windowMonitorTimer = new QTimer(this);
    connect(m_windowMonitorTimer, &QTimer::timeout, this, [this]() {
        if (!m_running || m_enginePid == 0) return;
        RenameData info;
        info.pid = static_cast<DWORD>(m_enginePid);
        info.newTitle = "AirPlay Video Stream (ALT+ENTER for Fullscreen)";
        EnumWindows(EnumWindowsProcRename, reinterpret_cast<LPARAM>(&info));
    });

    // Bluetooth Discovery Checkbox
    m_bleCheckbox = new QCheckBox("Enable Bluetooth Discovery", this);
    QSettings settings;
    m_bleCheckbox->setChecked(settings.value("ble_enabled", true).toBool());
    connect(m_bleCheckbox, &QCheckBox::toggled, this, &MainWindow::toggleBle);
    layout->addWidget(m_bleCheckbox);

    // Force Fullscreen Checkbox
    m_fullscreenCheckbox = new QCheckBox("Force Fullscreen (must select renderer)", this);
    m_fullscreenCheckbox->setChecked(
        settings.value("force_fs_enabled", false).toBool()
    );
    connect(m_fullscreenCheckbox, &QCheckBox::toggled, this,
            &MainWindow::toggleForceFullscreen);
    layout->addWidget(m_fullscreenCheckbox);

    // Renderer dropdown
    m_rendererCombo = new QComboBox(this);
    m_rendererCombo->addItem("Video Renderer (Auto)", "auto");
    m_rendererCombo->addItem("D3D11", "d3d11");
    m_rendererCombo->addItem("D3D12", "d3d12");

    {
        QString saved = settings.value("renderer_mode", "auto").toString();
        int idx = m_rendererCombo->findData(saved);
        if (idx >= 0) m_rendererCombo->setCurrentIndex(idx);
    }

    connect(m_rendererCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onRendererChanged);

    layout->addWidget(m_rendererCombo);


    m_settingsBtn = new QPushButton("Edit UxPlay Arguments (Advanced)", this);
    connect(m_settingsBtn, &QPushButton::clicked, this, &MainWindow::openSettingsFile);
    layout->addWidget(m_settingsBtn);

    m_listargsBtn = new QPushButton("List UxPlay arguments", this);
    connect(m_listargsBtn, &QPushButton::clicked, this, &MainWindow::openListArgsFile);
    layout->addWidget(m_listargsBtn);

    m_autostartBtn = new QPushButton(this);
    connect(m_autostartBtn, &QPushButton::clicked, this, &MainWindow::toggleAutostart);
    layout->addWidget(m_autostartBtn);

    m_licenseBtn = new QPushButton("License Information", this);
    connect(m_licenseBtn, &QPushButton::clicked, this, &MainWindow::showLicense);
    layout->addWidget(m_licenseBtn);

    layout->addStretch();
    updateStatus();
}

void MainWindow::openSettingsFile() {
    QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString filePath = appDataPath + "/arguments.txt";
    QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    
    m_tray->showMessage("Settings", "Restart the app to apply new arguments.", 
                        QSystemTrayIcon::Information, 3000);
}

void MainWindow::openListArgsFile() {
    QString filePath = QApplication::applicationDirPath() + "/resources/uxplay_arguments_list.txt";
    QFile::setPermissions(filePath, QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther);
    QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
}

void MainWindow::setupTray() {
    m_tray = new QSystemTrayIcon(this);
    QIcon trayIcon;
    QString icoPath = QApplication::applicationDirPath() + "/resources/icon.ico";
    trayIcon = QIcon(icoPath);
    if (trayIcon.isNull()) {
        trayIcon = QApplication::style()->standardIcon(QStyle::SP_MediaPlay);
    }
    m_tray->setIcon(trayIcon);
    m_tray->setToolTip("uxplay-windows");

    m_trayMenu = new QMenu(this);
    m_retryBonjourAction = m_trayMenu->addAction(
        "Retry Bonjour Discovery", this, &MainWindow::retryBonjourDiscovery);
    m_trayMenu->addAction("Open Log File", this, &MainWindow::openLogFile);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction("Quit", this, &MainWindow::quit);
    m_trayMenu->addAction("Restart", this, &MainWindow::restartApplication);

    m_tray->setContextMenu(m_trayMenu);
    
    connect(m_tray, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    m_tray->show();
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger) {
        isVisible() ? hide() : showNormal();
    }
}

void MainWindow::toggleBle(bool checked) {
    QSettings settings;
    
    if (settings.value("ble_enabled").toBool() == checked) {
        return;
    }

    settings.setValue("ble_enabled", checked);
    
    m_tray->showMessage("uxplay-windows", "Please restart the uxplay-windows to apply changes.\n(Right-click the Tray Icon)", 
                        QSystemTrayIcon::Information, 3000);
}

void MainWindow::toggleForceFullscreen(bool checked) {
    QSettings settings;
    if (settings.value("force_fs_enabled").toBool() == checked) {
        return;
    }

    settings.setValue("force_fs_enabled", checked);
    m_tray->showMessage("uxplay-windows", "Please restart the uxplay-windows to apply changes.\n(Right-click the Tray Icon)", 
                        QSystemTrayIcon::Information, 3000);
}

void MainWindow::onRendererChanged(int /*index*/) {
    if (!m_rendererCombo) return;

    QString mode = m_rendererCombo->currentData().toString();

    QSettings settings;
    QString saved = settings.value("renderer_mode", "auto").toString();
    if (saved == mode) return;

    settings.setValue("renderer_mode", mode);
    m_tray->showMessage("uxplay-windows", "Please restart the uxplay-windows to apply changes.\n(Right-click the Tray Icon)", 
                        QSystemTrayIcon::Information, 3000);
}

void MainWindow::applyRendererAndFullscreenArgs(QStringList &args) {
    while (true) {
        int idx = args.indexOf("-fs");
        if (idx < 0) break;
        args.removeAt(idx);
    }
    if (m_fullscreenCheckbox && m_fullscreenCheckbox->isChecked()) {
        args << "-fs";
    }

    // Remove existing "-vs <sink>" pairs
    for (int i = 0; i < args.size();) {
        if (args[i] == "-vs") {
            args.removeAt(i); // -vs
            if (i < args.size()) {
                args.removeAt(i); // sink
            }
            continue;
        }
        ++i;
    }

    QString mode = "auto";
    if (m_rendererCombo) {
        mode = m_rendererCombo->currentData().toString();
    }

    if (mode == "d3d11") {
        args << "-vs" << "d3d11videosink";
    } else if (mode == "d3d12") {
        args << "-vs" << "d3d12videosink";
    }
}

void MainWindow::startServer() {
    if (m_engine && m_engine->state() != QProcess::NotRunning) return;

    if (m_engine) {
        m_engine->deleteLater();
        m_engine.clear();
    }

    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(appData);
    
    QStringList args = getArgumentsFromFile();

    // Network adapter order changes frequently on Windows (VPN, Hyper-V,
    // USB Ethernet). Keep the AirPlay DeviceID stable unless the user set -m.
    if (!args.contains("-m")) {
        args << "-m" << stableDeviceId();
    }
    
    int bleIdx = -1;
    while ((bleIdx = args.indexOf("-ble")) != -1) {
        args.removeAt(bleIdx);
        if (bleIdx < args.size() && !args[bleIdx].startsWith("-")) {
            args.removeAt(bleIdx);
        }
    }

    applyRendererAndFullscreenArgs(args);

    // BLE is user-selectable during normal operation and is forced only for
    // this process when Bonjour is unavailable. Passing -ble also tells
    // libuxplay not to abort when DNS-SD registration fails.
    const bool useBle = m_forceBleFallback ||
                        (m_bleCheckbox && m_bleCheckbox->isChecked());
    m_bleAvailable = false;
    if (useBle) {
        QString bleFilePath = QDir::toNativeSeparators(appData + "/uxplay_status.ble");
        m_bleAvailable = startBluetoothBeacon(bleFilePath);
        if (!m_bleAvailable) scheduleBluetoothBeaconRestart();
        if (m_bleAvailable || m_forceBleFallback) {
            // Forced fallback deliberately keeps -ble even if the advertiser
            // is unavailable: the user's requested behavior is to keep the
            // receiver listening and show a degraded-discovery warning.
            args << "-ble" << bleFilePath;
        }
    } else {
        stopBluetoothBeacon();
    }

    const QString enginePath = QApplication::applicationDirPath() + "/uxplay-engine.exe";
    if (!QFileInfo::exists(enginePath)) {
        m_starting = false;
        m_running = false;
        updateStatus();
        QMessageBox::critical(
            this,
            "UxPlay Engine Missing",
            "uxplay-engine.exe is missing. Reinstall uxplay-windows using the MSI package."
        );
        return;
    }

    auto *engine = new QProcess(this);
    m_engine = engine;
    m_engineOutputBuffer.clear();
    engine->setProgram(enginePath);
    engine->setArguments(args);
    engine->setWorkingDirectory(QApplication::applicationDirPath());
    engine->setProcessChannelMode(QProcess::MergedChannels);
#ifdef _WIN32
    engine->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        });
#endif

    connect(engine, &QProcess::started, this, [this, engine]() {
        if (m_engine != engine) return;
        m_enginePid = engine->processId();
        m_starting = true;
        m_running = false;
        updateStatus();
        qInfo() << "UxPlay engine process started, pid=" << m_enginePid;
    });
    connect(engine, &QProcess::readyRead, this,
            [this, engine]() { handleEngineOutput(engine); });
    connect(engine, &QProcess::errorOccurred, this,
            [this, engine](QProcess::ProcessError error) {
        if (m_engine != engine) return;
        qWarning() << "UxPlay engine process error:" << error << engine->errorString();
        if (m_tray) {
            m_tray->showMessage(
                "UxPlay engine error",
                engine->errorString(),
                QSystemTrayIcon::Warning,
                5000);
        }
        // Qt does not emit finished() when CreateProcess itself fails.
        if (error == QProcess::FailedToStart && m_engine == engine) {
            m_engine.clear();
            m_engineOutputBuffer.clear();
            m_enginePid = 0;
            m_starting = false;
            m_running = false;
            updateStatus();
            engine->deleteLater();
            scheduleEngineRestart();
        }
    });
    connect(engine,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, engine](int exitCode, QProcess::ExitStatus exitStatus) {
        qWarning() << "UxPlay engine exited:" << exitCode << exitStatus;
        const bool wasCurrentEngine = (m_engine == engine);
        if (!wasCurrentEngine) {
            engine->deleteLater();
            return;
        }

        m_engine.clear();
        m_engineOutputBuffer.clear();
        m_enginePid = 0;
        m_starting = false;
        m_running = false;
        updateStatus();
        if (m_windowMonitorTimer) m_windowMonitorTimer->stop();
        engine->deleteLater();
        scheduleEngineRestart();
    });

    m_starting = true;
    m_running = false;
    updateStatus();
    engine->start();
}

void MainWindow::stopServer() {
    stopBluetoothBeacon();
    if (m_windowMonitorTimer) m_windowMonitorTimer->stop();
    if (m_engine) {
        QProcess *engine = m_engine.data();
        m_engine.clear();
        disconnect(engine, nullptr, this, nullptr);
        if (engine->state() != QProcess::NotRunning) {
            engine->terminate();
            if (!engine->waitForFinished(3000)) {
                qWarning() << "UxPlay engine did not exit; killing child process";
                engine->kill();
                engine->waitForFinished(1000);
            }
        }
        engine->deleteLater();
    }
    m_enginePid = 0;
    m_engineOutputBuffer.clear();
    m_starting = false;
    m_running = false;
    updateStatus();
}

void MainWindow::handleEngineOutput(QProcess *engine) {
    if (!engine) return;
    const QByteArray output = engine->readAll();
    if (output.isEmpty()) return;

    qInfo().noquote() << "[engine]" << QString::fromLocal8Bit(output).trimmed();
    m_engineOutputBuffer += output;
    if (!m_running && m_engineOutputBuffer.contains("Initialized server socket(s)")) {
        markEngineReady();
    }
    if (m_engineOutputBuffer.size() > 16384) {
        m_engineOutputBuffer = m_engineOutputBuffer.right(4096);
    }
}

void MainWindow::markEngineReady() {
    m_starting = false;
    m_running = true;
    m_restartDelayMs = 1000;
    updateStatus();
    if (m_windowMonitorTimer) m_windowMonitorTimer->start(2000);
    qInfo() << "UxPlay engine is listening";
}

void MainWindow::scheduleEngineRestart() {
    if (m_quitting) return;
    const int delay = m_restartDelayMs;
    m_restartDelayMs = qMin(m_restartDelayMs * 2, 30000);
    qWarning() << "Restarting UxPlay engine in" << delay << "ms";
    QTimer::singleShot(delay, this, [this]() {
        if (!m_quitting) startServer();
    });
}

void MainWindow::toggleAutostart() {
    bool enable = !isAutostartEnabled();
    setAutostart(enable);
    if (m_autostartAction)
        m_autostartAction->setChecked(enable);
    updateStatus();
}

bool MainWindow::isAutostartEnabled() const {
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    return reg.contains("uxplay-windows");
}

void MainWindow::setAutostart(bool enabled) {
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enabled) {
        QString path = QDir::toNativeSeparators(QApplication::applicationFilePath());
        reg.setValue("uxplay-windows", "\"" + path + "\"");
    } else {
        reg.remove("uxplay-windows");
    }
}

bool MainWindow::startBluetoothBeacon(const QString &path) {
    m_blePath = path;
    if (m_beacon && m_beacon->state() != QProcess::NotRunning)
        return true;

    if (m_beacon) {
        stopBluetoothBeacon();
    }

    QString exe = QApplication::applicationDirPath() + "/uxplay-bluetooth-beacon.exe";
    if (!QFile::exists(exe)) {
        qWarning() << "uxplay-bluetooth-beacon.exe not found:" << exe;
        return false;
    }

    auto *beacon = new QProcess(this);
    m_beacon = beacon;
    beacon->setProcessChannelMode(QProcess::MergedChannels);
#ifdef _WIN32
    beacon->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        });
#endif

    connect(beacon, &QProcess::errorOccurred, this,
            [this, beacon](QProcess::ProcessError error) {
        qWarning() << "Bluetooth beacon process error:" << error
                   << beacon->errorString();
        if (m_tray) {
            m_tray->showMessage(
                "Bluetooth discovery unavailable",
                m_bonjourAvailable
                    ? "The Bluetooth beacon could not be started. Bonjour discovery is still active."
                    : "Both Bonjour and Bluetooth discovery are unavailable. The server is still running; retry Bonjour from the tray menu.",
                QSystemTrayIcon::Warning,
                5000);
        }
        m_bleAvailable = false;
        updateStatus();
    });

    connect(beacon,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, beacon](int exitCode, QProcess::ExitStatus exitStatus) {
        qWarning() << "Bluetooth beacon exited:" << exitCode << exitStatus;
        if (m_beacon != beacon) {
            beacon->deleteLater();
            return;
        }
        m_beacon.clear();
        m_bleAvailable = false;
        updateStatus();
        beacon->deleteLater();
        scheduleBluetoothBeaconRestart();
    });

    // Pass the explicit path to the beacon file
    beacon->start(exe, {"--path", path});
    if (!beacon->waitForStarted(3000)) {
        qWarning() << "Bluetooth beacon failed to start:" << beacon->errorString();
        beacon->deleteLater();
        m_beacon.clear();
        return false;
    }

    QByteArray startupOutput;
    bool ready = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 8000 && beacon->state() != QProcess::NotRunning) {
        if (beacon->waitForReadyRead(500)) {
            startupOutput += beacon->readAll();
            if (startupOutput.contains("[beacon] READY")) {
                ready = true;
                break;
            }
            if (startupOutput.contains("[beacon] FATAL")) {
                break;
            }
        }
    }
    if (!startupOutput.isEmpty()) {
        qInfo().noquote() << QString::fromLocal8Bit(startupOutput).trimmed();
    }
    if (!ready) {
        qWarning() << "Bluetooth beacon did not report READY";
        disconnect(beacon, nullptr, this, nullptr);
        beacon->kill();
        beacon->waitForFinished(1000);
        beacon->deleteLater();
        m_beacon.clear();
        return false;
    }

    connect(beacon, &QProcess::readyRead, this, [beacon]() {
        qInfo().noquote() << "[beacon]" << QString::fromLocal8Bit(beacon->readAll()).trimmed();
    });

    qInfo() << "Beacon process READY, watching:" << path;
    m_beaconRestartDelayMs = 1000;
    return true;
}

void MainWindow::scheduleBluetoothBeaconRestart() {
    const bool wanted = m_forceBleFallback ||
                        (m_bleCheckbox && m_bleCheckbox->isChecked());
    if (m_quitting || !wanted || m_blePath.isEmpty() || m_beacon ||
        m_beaconRestartPending) {
        return;
    }

    const int delay = m_beaconRestartDelayMs;
    m_beaconRestartDelayMs = qMin(m_beaconRestartDelayMs * 2, 30000);
    m_beaconRestartPending = true;
    qWarning() << "Retrying Bluetooth beacon in" << delay << "ms";
    QTimer::singleShot(delay, this, [this]() {
        m_beaconRestartPending = false;
        const bool stillWanted = m_forceBleFallback ||
                                 (m_bleCheckbox && m_bleCheckbox->isChecked());
        if (m_quitting || !stillWanted || m_beacon) return;
        m_bleAvailable = startBluetoothBeacon(m_blePath);
        updateStatus();
        if (!m_bleAvailable) scheduleBluetoothBeaconRestart();
    });
}

void MainWindow::stopBluetoothBeacon() {
    if (!m_beacon) return;

    QProcess *beacon = m_beacon.data();
    m_beacon.clear();
    disconnect(beacon, nullptr, this, nullptr);

    qDebug() << "Stopping beacon process";
    if (beacon->state() != QProcess::NotRunning) {
        beacon->terminate();
        if (!beacon->waitForFinished(500)) {
            qDebug() << "Beacon didn't terminate, killing";
            beacon->kill();
            beacon->waitForFinished(100);
        }
    }

    beacon->deleteLater();
    m_bleAvailable = false;
    qDebug() << "Beacon stopped and cleaned up";
}

void MainWindow::showLicense() {
    QString path = QApplication::applicationDirPath() + "/LICENSE.rtf";
    if (QFile::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else qDebug() << "License file not found:" << path;
}

void MainWindow::quit() {
    m_quitting = true;
    stopServer();
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!m_quitting) {
        hide();
        event->ignore();
    } else {
        event->accept();
    }
}

void MainWindow::updateStatus() {
    QString status;
    if (m_starting) {
        status = "UxPlay server starting";
    } else if (m_running) {
        status = "UxPlay server running";
    } else {
        status = "UxPlay server stopped";
    }

    QString discovery;
    if (m_bonjourAvailable && m_bleAvailable) {
        discovery = "Bonjour + Bluetooth";
    } else if (m_bonjourAvailable) {
        discovery = "Bonjour";
    } else if (m_bleAvailable) {
        discovery = "Bluetooth fallback";
    } else {
        discovery = "unavailable (server still running)";
    }

    status += "\nDiscovery: " + discovery;
    m_statusLabel->setText(status);
    if (m_retryBonjourAction) {
        m_retryBonjourAction->setEnabled(!m_bonjourAvailable);
    }
    m_autostartBtn->setText(isAutostartEnabled() ? "Open uxplay-windows on login: ON " : "Open uxplay-windows on login: OFF");
}

bool MainWindow::ensureBonjourServiceAvailable(bool allowRepair) {
    const std::wstring serviceName = L"Bonjour Service";

    DWORD state = queryWindowsServiceState(serviceName);
    if (state == SERVICE_RUNNING) {
        return true;
    }

    if (state == SERVICE_START_PENDING &&
        waitForWindowsServiceState(serviceName, SERVICE_RUNNING, 5000)) {
        return true;
    }

    if (state != kServiceMissing) {
        if (startWindowsServiceNormally(serviceName) &&
            waitForWindowsServiceState(serviceName, SERVICE_RUNNING, 5000)) {
            Sleep(1000); // allow the DNS-SD IPC endpoint to become ready
            return true;
        }

        if (!allowRepair) {
            qWarning() << "Bonjour is installed but not running; continuing without blocking startup";
            return false;
        }

        int choice = QMessageBox::question(
            this,
            "Bonjour Service Not Running",
            QString("Bonjour Service is installed but %1. Start it with "
                    "administrator permission?\n\n"
                    "Choose No to continue with Bluetooth discovery.")
                .arg(bonjourStateText(state)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes
        );

        if (choice == QMessageBox::Yes &&
            startWindowsServiceElevated(serviceName) &&
            waitForWindowsServiceState(serviceName, SERVICE_RUNNING, 10000)) {
            Sleep(1000);
            return true;
        }

        qWarning() << "Bonjour is installed but could not be started; continuing with BLE";
        return false;
    }

    if (!allowRepair) {
        qWarning() << "Bonjour is not installed; continuing without blocking startup";
        return false;
    }

    int choice = QMessageBox::question(
        this,
        "Bonjour Service Unavailable",
        "Bonjour Service is not installed. Install it now for the most "
        "reliable macOS discovery?\n\nChoose No to continue with Bluetooth discovery.",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes
    );

    if (choice != QMessageBox::Yes) {
        qWarning() << "Bonjour installation declined; continuing with BLE";
        return false;
    }

    QMessageBox::information(
        this,
        "Installing Bonjour Service",
        "Starting installation of 'Bonjour Service'. Follow any UAC prompt."
    );

    int rc = mdns::MdnsResponder::install();

    // Re-check after install to be safe. mDNSResponder -install should also
    // start the service; if it is still starting, wait a short time.
    if (rc == 0 &&
        (queryWindowsServiceState(serviceName) == SERVICE_RUNNING ||
         waitForWindowsServiceState(serviceName, SERVICE_RUNNING, 15000))) {
        Sleep(1000);
        QMessageBox::information(
            this,
            "Installation Complete",
            "Bonjour Service installed successfully. UxPlay will continue starting."
        );
        return true;
    }

    QMessageBox::warning(
        this,
        "Bonjour Installation Failed",
        "Bonjour could not be installed or started. UxPlay will continue "
        "with Bluetooth discovery. You can retry Bonjour from the tray menu."
    );
    return false;
}

void MainWindow::showDiscoveryFallbackWarning() {
    QString detail = m_bleAvailable
        ? "Bonjour is unavailable. UxPlay is still running and will use Bluetooth discovery."
        : "Bonjour and Bluetooth discovery are unavailable. UxPlay is still running, but the Mac may not find it automatically.";

    QMessageBox::warning(
        this,
        "Discovery Running in Degraded Mode",
        detail + "\n\nUse 'Retry Bonjour Discovery' from the tray menu after fixing the service."
    );
    if (m_tray) {
        m_tray->showMessage(
            "Discovery degraded",
            detail,
            QSystemTrayIcon::Warning,
            8000);
    }
}

void MainWindow::retryBonjourDiscovery() {
    if (m_bonjourAvailable) return;

    if (!ensureBonjourServiceAvailable(true)) {
        showDiscoveryFallbackWarning();
        return;
    }

    m_bonjourAvailable = true;
    m_forceBleFallback = false;
    updateStatus();
    QMessageBox::information(
        this,
        "Bonjour Ready",
        "Bonjour is running. The AirPlay engine will restart once to register its services."
    );
    stopServer();
    startServer();
}

void MainWindow::openLogFile() {
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + "/uxplay-windows.log";
    if (!QFileInfo::exists(path)) {
        QMessageBox::information(this, "Log File", "No log file has been created yet.");
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::restartApplication() {
    m_quitting = true;

    // The engine is isolated in a child process, so it can be stopped fully
    // before the replacement GUI starts and re-registers the same DeviceID.
    stopServer();
    single_instance::release();

    QString exePath = QApplication::applicationFilePath();
    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty()) args.removeFirst(); // remove exe path

    if (!QProcess::startDetached(exePath, args)) {
        single_instance::acquire();
        m_quitting = false;
        QMessageBox::critical(this, "Restart Failed", "Could not start a new uxplay-windows process.");
        return;
    }

    // close GUI of current process after a short delay
    QTimer::singleShot(200, qApp, &QCoreApplication::quit);
}
