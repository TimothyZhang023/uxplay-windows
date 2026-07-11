#include "mainwindow.h"
#include "mdns_responder.hpp"
#include "single_instance.hpp"
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>

#include <QProcess>
#include <QProcessEnvironment>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDateTime>
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
#include <QSignalBlocker>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QComboBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QTextStream>

namespace {
constexpr DWORD kServiceMissing = 0;
constexpr DWORD kBonjourStartTimeoutMs = 15000;
constexpr DWORD kBonjourPollMs = 250;
constexpr int kMaximumStartupFailures = 3;
constexpr qint64 kMinimumHealthyLifetimeMs = 30000;

bool containsVideoPipelineFailure(const QByteArray &output) {
    const QByteArray lower = output.toLower();
    if (lower.contains("failed to initialize gstreamer video renderer") ||
           lower.contains("unable to construct a working video pipeline") ||
           lower.contains("no element \"d3d11h264dec\"") ||
           lower.contains("no element \"d3d11h265dec\"") ||
           lower.contains("no element \"d3d12h264dec\"") ||
           lower.contains("no element \"d3d12h265dec\"")) {
        return true;
    }

    // Only classify complete, renderer-specific errors. Network disconnects,
    // corrupt sender frames and the normal D3D window-close message must not
    // permanently change the user's renderer.
    for (const QByteArray &line : lower.split('\n')) {
        if (!line.contains("gstreamer error (video)") ||
            line.contains("output window was closed")) {
            continue;
        }
        if (line.contains("internal data stream error") ||
            line.contains("not-negotiated") ||
            line.contains("device lost") ||
            line.contains("failed to initialize")) {
            return true;
        }
    }
    return false;
}

QString diagnosticDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QStringList redactedArguments(const QStringList &arguments) {
    QStringList result = arguments;
    for (int i = 0; i < result.size(); ++i) {
        if ((result[i] == "-pw" || result[i] == "-pin") &&
            i + 1 < result.size() &&
            !result[i + 1].startsWith('-')) {
            result[i + 1] = "********";
        } else if (result[i].startsWith("-pin") && result[i].size() > 4) {
            result[i] = "-pin****";
        }
    }
    return result;
}

void removeOptionWithValue(QStringList &arguments, const QString &option,
                           bool allowNegativeNumber = false) {
    for (int index = arguments.indexOf(option); index >= 0;
         index = arguments.indexOf(option)) {
        arguments.removeAt(index);
        if (index < arguments.size()) {
            const QString value = arguments[index];
            const bool negativeNumber = allowNegativeNumber &&
                QRegularExpression("^-\\d+(\\.\\d+)?$").match(value).hasMatch();
            if (!value.startsWith('-') || negativeNumber) {
                arguments.removeAt(index);
            }
        }
    }
}

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
    setFixedSize(380, 440);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *layout = new QVBoxLayout(central);

    m_statusLabel = new QLabel("Initializing...", this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    // Bluetooth Discovery Checkbox
    m_bleCheckbox = new QCheckBox("Enable Bluetooth Discovery", this);
    QSettings settings;
    m_bleCheckbox->setChecked(settings.value("ble_enabled", true).toBool());
    connect(m_bleCheckbox, &QCheckBox::toggled, this, &MainWindow::toggleBle);
    layout->addWidget(m_bleCheckbox);

    // Force Fullscreen Checkbox
    m_fullscreenCheckbox = new QCheckBox("Start D3D video in fullscreen", this);
    m_fullscreenCheckbox->setToolTip(
        "Double-click the video or press F11 to enter/leave fullscreen. Alt+Enter is disabled.");
    if (!settings.value("native_fullscreen_defaults_v2", false).toBool()) {
        // v2.2.0 used cross-process window manipulation. Start the new native
        // D3D path windowed once so stale fullscreen state cannot affect startup.
        settings.setValue("force_fs_enabled", false);
        settings.setValue("native_fullscreen_defaults_v2", true);
    }
    m_fullscreenCheckbox->setChecked(
        settings.value("force_fs_enabled", false).toBool()
    );
    connect(m_fullscreenCheckbox, &QCheckBox::toggled, this,
            &MainWindow::toggleForceFullscreen);
    layout->addWidget(m_fullscreenCheckbox);

    m_lowLatencyCheckbox = new QCheckBox(
        "Low latency mode (experimental; may reduce A/V sync)", this);
    if (!settings.value("low_latency_defaults_v2", false).toBool()) {
        settings.setValue("low_latency_enabled", false);
        settings.setValue("low_latency_defaults_v2", true);
    }
    m_lowLatencyCheckbox->setChecked(
        settings.value("low_latency_enabled", false).toBool());
    m_lowLatencyCheckbox->setToolTip(
        "Disables timestamp-delayed presentation. Disable if audio/video synchronization is more important than latency.");
    connect(m_lowLatencyCheckbox, &QCheckBox::toggled,
            this, &MainWindow::toggleLowLatency);
    layout->addWidget(m_lowLatencyCheckbox);

    m_qualityCombo = new QComboBox(this);
    m_qualityCombo->addItem("Quality: 1080p @ 60 FPS (Recommended)", "1080p60");
    m_qualityCombo->addItem("Quality: 1440p @ 60 FPS", "1440p60");
    m_qualityCombo->addItem("Quality: 4K @ 60 FPS (HEVC)", "4k60");
    m_qualityCombo->addItem("Quality: 1080p @ 30 FPS (Compatibility)", "1080p30");
    m_qualityCombo->setToolTip(
        "The sender and GPU determine the final delivered resolution and frame rate.");
    {
        const QString saved = settings.value("quality_profile", "1080p60").toString();
        const int index = m_qualityCombo->findData(saved);
        if (index >= 0) m_qualityCombo->setCurrentIndex(index);
    }
    connect(m_qualityCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onQualityChanged);
    layout->addWidget(m_qualityCombo);

    // Renderer dropdown
    m_rendererCombo = new QComboBox(this);
    m_rendererCombo->addItem("Renderer: Automatic (Recommended)", "auto");
    m_rendererCombo->addItem("Renderer: D3D11 Hardware", "d3d11");
    m_rendererCombo->addItem("Renderer: D3D12 Hardware", "d3d12");
    m_rendererCombo->addItem("Renderer: Software Decode (Compatibility)", "software");
    m_rendererCombo->setToolTip(
        "Automatic lets GStreamer choose. Hardware modes pair the decoder and sink; failures fall back to software once.");

    {
        const bool hadSavedRenderer = settings.contains("renderer_mode");
        QString saved = settings.value("renderer_mode", "auto").toString();
        if (!settings.value("renderer_stability_defaults_v2", false).toBool()) {
            // Existing v2.2.0 installations may have been forced onto a D3D
            // mode that their adapter cannot initialize. Migrate them once to
            // the known-safe path; fresh installations keep Automatic.
            if (hadSavedRenderer && saved != "software") saved = "software";
            settings.setValue("renderer_mode", saved);
            settings.setValue("renderer_stability_defaults_v2", true);
        }
        if (!settings.value("performance_defaults_v1", false).toBool()) {
            settings.setValue("performance_defaults_v1", true);
        }
        int idx = m_rendererCombo->findData(saved);
        if (idx < 0) idx = m_rendererCombo->findData("software");
        m_rendererCombo->setCurrentIndex(idx);
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

    m_logsBtn = new QPushButton("Open Diagnostic Logs", this);
    connect(m_logsBtn, &QPushButton::clicked, this, &MainWindow::openLogFile);
    layout->addWidget(m_logsBtn);

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
    m_retryEngineAction = m_trayMenu->addAction(
        "Retry UxPlay Engine", this, &MainWindow::retryEngine);
    m_trayMenu->addAction("Open Diagnostic Logs", this, &MainWindow::openLogFile);
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
    if (settings.value("force_fs_enabled", false).toBool() == checked) return;
    settings.setValue("force_fs_enabled", checked);
    restartEngineForSettings(
        checked ? "Native D3D fullscreen enabled."
                : "Native D3D fullscreen disabled.");
}

void MainWindow::toggleLowLatency(bool checked) {
    QSettings settings;
    if (settings.value("low_latency_enabled", false).toBool() == checked) return;
    settings.setValue("low_latency_enabled", checked);
    restartEngineForSettings(
        checked ? "Low latency pipeline enabled."
                : "Timestamp-synchronized compatibility pipeline enabled.");
}

void MainWindow::onRendererChanged(int /*index*/) {
    if (!m_rendererCombo) return;

    QString mode = m_rendererCombo->currentData().toString();

    QSettings settings;
    QString saved = settings.value("renderer_mode", "auto").toString();
    if (saved == mode) return;

    m_rendererFallbackAttempted = false;
    settings.setValue("renderer_mode", mode);
    restartEngineForSettings("Video renderer changed to " +
                             m_rendererCombo->currentText() + ".");
}

void MainWindow::onQualityChanged(int /*index*/) {
    if (!m_qualityCombo) return;
    const QString profile = m_qualityCombo->currentData().toString();
    QSettings settings;
    if (settings.value("quality_profile", "1080p60").toString() == profile) return;
    settings.setValue("quality_profile", profile);
    restartEngineForSettings("Streaming profile changed to " +
                             m_qualityCombo->currentText() + ".");
}

void MainWindow::restartEngineForSettings(const QString &message) {
    if (m_streaming) {
        m_settingsRestartPending = true;
        if (m_tray) {
            m_tray->showMessage(
                "Video setting saved",
                message + " It will be applied after the current AirPlay session ends.",
                QSystemTrayIcon::Information, 5000);
        }
        return;
    }
    m_settingsRestartPending = false;
    if (m_tray) {
        m_tray->showMessage("uxplay-windows", message + " Restarting UxPlay engine...",
                            QSystemTrayIcon::Information, 3000);
    }
    stopServer();
    m_consecutiveEngineFailures = 0;
    m_restartDelayMs = 1000;
    m_lastEngineFailure.clear();
    QTimer::singleShot(250, this, [this]() {
        if (!m_quitting && !m_engine) startServer();
    });
}

bool MainWindow::fallbackToSoftwareRenderer(const QString &reason) {
    if (m_rendererFallbackAttempted || !m_rendererCombo ||
        m_rendererCombo->currentData().toString() == "software") {
        return false;
    }

    const int softwareIndex = m_rendererCombo->findData("software");
    if (softwareIndex < 0) return false;

    m_rendererFallbackAttempted = true;
    {
        QSignalBlocker blocker(m_rendererCombo);
        m_rendererCombo->setCurrentIndex(softwareIndex);
    }
    QSettings().setValue("renderer_mode", "software");
    const QString conciseReason = reason.trimmed().left(240);
    appendEngineLog(QString(
        "[supervisor] video renderer failed; selected software compatibility mode: %1\n")
        .arg(conciseReason).toUtf8());
    if (m_tray) {
        m_tray->showMessage(
            "Video renderer fallback",
            "The selected video pipeline failed. Software compatibility mode will be used.\n" +
                conciseReason,
            QSystemTrayIcon::Warning, 8000);
    }
    return true;
}

void MainWindow::applyRendererAndFullscreenArgs(QStringList &args) {
    while (true) {
        int idx = args.indexOf("-fs");
        if (idx < 0) break;
        args.removeAt(idx);
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
    removeOptionWithValue(args, "-vd");
    while (args.removeOne("-avdec")) {}

    QString mode = "auto";
    if (m_rendererCombo) {
        mode = m_rendererCombo->currentData().toString();
    }
    const bool fullscreen = m_fullscreenCheckbox &&
                            m_fullscreenCheckbox->isChecked();
    if (fullscreen) args << "-fs";

    if (mode == "d3d11") {
        QString sink = "d3d11videosink enable-navigation-events=true "
                       "fullscreen-toggle-mode=property";
        if (fullscreen) sink += " fullscreen=true";
        args << "-vd" << "d3d11h264dec" << "-vs" << sink;
    } else if (mode == "d3d12") {
        QString sink = "d3d12videosink enable-navigation-events=true "
                       "fullscreen-on-alt-enter=false";
        if (fullscreen) sink += " fullscreen=true";
        args << "-vd" << "d3d12h264dec" << "-vs" << sink;
    } else if (mode == "software") {
        // Keep the output sink automatic so compatibility mode is not tied to
        // a D3D API that may be unavailable on this adapter.
        args << "-avdec";
    }
}

void MainWindow::applyQualityAndLatencyArgs(QStringList &args) {
    removeOptionWithValue(args, "-s");
    removeOptionWithValue(args, "-fps");
    // These switches are owned by the checkbox. Remove stale values even when
    // low-latency mode is disabled so UI state and the launched engine agree.
    removeOptionWithValue(args, "-vsync", true);
    removeOptionWithValue(args, "-al");

    const QString profile = m_qualityCombo
        ? m_qualityCombo->currentData().toString()
        : QStringLiteral("1080p60");
    if (profile == "4k60") {
        args << "-s" << "3840x2160@60" << "-fps" << "60";
        if (!args.contains("-h265")) args << "-h265";
    } else if (profile == "1440p60") {
        args << "-s" << "2560x1440@60" << "-fps" << "60";
    } else if (profile == "1080p30") {
        args << "-s" << "1920x1080@30" << "-fps" << "30";
    } else {
        args << "-s" << "1920x1080@60" << "-fps" << "60";
    }

    const bool compatibilityProfile = profile == "1080p30";
    if (!m_lowLatencyCheckbox || !m_lowLatencyCheckbox->isChecked() ||
        compatibilityProfile) {
        return;
    }

    args << "-vsync" << "no" << "-al" << "0.15";

    // Never drop compressed H.264/H.265 buffers before decoding. Inter-frame
    // codecs depend on reference frames; dropping them causes visible macroblock
    // corruption until the next keyframe.
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
    applyQualityAndLatencyArgs(args);

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
        m_lastEngineFailure = "uxplay-engine.exe is missing";
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
    m_engineLineBuffer.clear();
    m_engineWasReady = false;
    m_streaming = false;
    m_settingsRestartPending = false;
    m_engineStartedAtMs = QDateTime::currentMSecsSinceEpoch();

    const QString engineLogPath = diagnosticDirectory() + "/uxplay-engine.log";
    if (m_engineLogFile.isOpen()) m_engineLogFile.close();
    QFileInfo engineLogInfo(engineLogPath);
    if (engineLogInfo.exists() && engineLogInfo.size() > 10 * 1024 * 1024) {
        QFile::remove(engineLogPath + ".old");
        QFile::rename(engineLogPath, engineLogPath + ".old");
    }
    m_engineLogFile.setFileName(engineLogPath);
    if (!m_engineLogFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        qWarning() << "Could not open engine diagnostic log:"
                   << engineLogPath << m_engineLogFile.errorString();
    } else {
        appendEngineLog(QString(
            "\n========== engine launch %1 ==========\nExecutable: %2\nArguments: %3\n")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                 QDir::toNativeSeparators(enginePath),
                 redactedArguments(args).join(' '))
            .toUtf8());
    }
    engine->setProgram(enginePath);
    engine->setArguments(args);
    engine->setWorkingDirectory(QApplication::applicationDirPath());
    QProcessEnvironment engineEnvironment = QProcessEnvironment::systemEnvironment();
    const bool compatibilityProfile = m_qualityCombo &&
        m_qualityCombo->currentData().toString() == "1080p30";
    const bool effectiveLowLatency = m_lowLatencyCheckbox &&
        m_lowLatencyCheckbox->isChecked() && !compatibilityProfile;
    engineEnvironment.insert("UXPLAY_LOW_LATENCY",
                             effectiveLowLatency ? "1" : "0");
    engine->setProcessEnvironment(engineEnvironment);
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
        HANDLE process = OpenProcess(PROCESS_SET_INFORMATION, FALSE,
                                     static_cast<DWORD>(m_enginePid));
        if (process) {
            if (!SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS)) {
                qWarning() << "Could not raise UxPlay engine priority. Error="
                           << GetLastError();
            }
            CloseHandle(process);
        }
        appendEngineLog(QString("[supervisor] process started; pid=%1\n")
                            .arg(m_enginePid).toUtf8());
    });
    connect(engine, &QProcess::readyRead, this,
            [this, engine]() { handleEngineOutput(engine); });
    connect(engine, &QProcess::errorOccurred, this,
            [this, engine](QProcess::ProcessError error) {
        if (m_engine != engine) return;
        qWarning() << "UxPlay engine process error:" << error << engine->errorString();
        appendEngineLog(QString("[supervisor] process error: %1\n")
                            .arg(engine->errorString()).toUtf8());
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
            m_enginePid = 0;
            m_starting = false;
            m_running = false;
            ++m_consecutiveEngineFailures;
            m_lastEngineFailure = "Could not start uxplay-engine.exe: " +
                                  engine->errorString();
            appendEngineLog(("[supervisor] " + m_lastEngineFailure + "\n").toUtf8());
            m_engineOutputBuffer.clear();
            m_engineLineBuffer.clear();
            m_engineLogFile.close();
            updateStatus();
            engine->deleteLater();
            if (m_consecutiveEngineFailures < kMaximumStartupFailures) {
                scheduleEngineRestart();
            }
        }
    });
    connect(engine,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, engine](int exitCode, QProcess::ExitStatus exitStatus) {
        const bool wasCurrentEngine = (m_engine == engine);
        if (!wasCurrentEngine) {
            engine->deleteLater();
            return;
        }

        // A final readyRead signal is not guaranteed before finished(). Drain
        // the pipe first so startup errors are never discarded.
        handleEngineOutput(engine);
        const qint64 lifetimeMs = qMax<qint64>(
            0, QDateTime::currentMSecsSinceEpoch() - m_engineStartedAtMs);
        const bool startupFailure = !m_engineWasReady ||
                                    lifetimeMs < kMinimumHealthyLifetimeMs;
        m_lastEngineFailure = engineExitReason(exitCode, exitStatus, lifetimeMs);
        qWarning() << "UxPlay engine exited:" << m_lastEngineFailure;
        appendEngineLog(QString(
            "[supervisor] engine exited after %1 ms; exitCode=%2; status=%3\n"
            "[supervisor] reason: %4\n")
            .arg(lifetimeMs)
            .arg(exitCode)
            .arg(exitStatus == QProcess::CrashExit ? "crash" : "normal")
            .arg(m_lastEngineFailure)
            .toUtf8());

        if (startupFailure) {
            ++m_consecutiveEngineFailures;
        } else {
            m_consecutiveEngineFailures = 1;
        }

        const bool pluginRegistryFailure =
            m_engineOutputBuffer.contains("registry may have been corrupted") ||
            m_engineOutputBuffer.contains("Required gstreamer plugin");
        if (pluginRegistryFailure && !m_registryRepairAttempted) {
            m_registryRepairAttempted = true;
            const QString registryPath = diagnosticDirectory() +
                                         "/gstreamer-registry.bin";
            const bool removed = !QFileInfo::exists(registryPath) ||
                                 QFile::remove(registryPath);
            appendEngineLog(QString(
                "[supervisor] GStreamer registry repair attempted: %1 (%2)\n")
                .arg(QDir::toNativeSeparators(registryPath),
                     removed ? "removed" : "remove failed")
                .toUtf8());
        }

        const bool rendererFallbackSelected =
            startupFailure && containsVideoPipelineFailure(m_engineOutputBuffer) &&
            fallbackToSoftwareRenderer(m_lastEngineFailure);
        if (rendererFallbackSelected) {
            m_consecutiveEngineFailures = 0;
        }

        m_engine.clear();
        m_enginePid = 0;
        m_starting = false;
        m_running = false;
        m_streaming = false;
        engine->deleteLater();
        m_engineLogFile.close();
        m_engineOutputBuffer.clear();
        m_engineLineBuffer.clear();
        updateStatus();

        if (!startupFailure ||
            rendererFallbackSelected ||
            m_consecutiveEngineFailures < kMaximumStartupFailures) {
            scheduleEngineRestart();
        } else if (m_tray) {
            m_tray->showMessage(
                "UxPlay engine stopped",
                m_lastEngineFailure +
                    "\nAutomatic retries were paused. Open Diagnostic Logs, then choose Retry UxPlay Engine.",
                QSystemTrayIcon::Critical,
                10000);
        }
    });

    m_starting = true;
    m_running = false;
    updateStatus();
    engine->start();
}

void MainWindow::stopServer() {
    stopBluetoothBeacon();
    m_streaming = false;
    if (m_engine) {
        QProcess *engine = m_engine.data();
        m_engine.clear();
        disconnect(engine, nullptr, this, nullptr);
        if (engine->state() != QProcess::NotRunning) {
            // QProcess::terminate() posts WM_CLOSE to every top-level child
            // window on Windows. That closes the GStreamer D3D window first
            // and creates the misleading "Output window was closed" failure.
            // The engine is process-isolated, so stop it directly instead.
            appendEngineLog(
                "[supervisor] stopping isolated engine without WM_CLOSE\n");
            engine->kill();
            if (!engine->waitForFinished(2000)) {
                qWarning() << "UxPlay engine did not exit after TerminateProcess";
            }
        }
        handleEngineOutput(engine);
        appendEngineLog("[supervisor] engine stopped by uxplay-windows\n");
        engine->deleteLater();
    }
    if (m_engineLogFile.isOpen()) m_engineLogFile.close();
    m_enginePid = 0;
    m_engineOutputBuffer.clear();
    m_engineLineBuffer.clear();
    m_starting = false;
    m_running = false;
    updateStatus();
}

void MainWindow::handleEngineOutput(QProcess *engine) {
    if (!engine) return;
    const QByteArray output = engine->readAll();
    if (output.isEmpty()) return;

    appendEngineLog(output);
    m_engineOutputBuffer += output;
    m_engineLineBuffer += output;

    QByteArray completeOutput;
    while (true) {
        const qsizetype newline = m_engineLineBuffer.indexOf('\n');
        if (newline < 0) break;
        completeOutput += m_engineLineBuffer.left(newline + 1);
        m_engineLineBuffer.remove(0, newline + 1);
    }
    if (m_engineLineBuffer.size() > 8192) {
        m_engineLineBuffer = m_engineLineBuffer.right(8192);
    }

    if (m_engine == engine) {
        if (completeOutput.contains("raop_rtp_mirror starting mirroring") ||
            completeOutput.contains("Begin streaming to GStreamer video pipeline")) {
            m_streaming = true;
        }
        if (completeOutput.contains("raop_rtp_mirror->running is no longer true") ||
            completeOutput.contains("Output window was closed")) {
            m_streaming = false;
            if (m_settingsRestartPending) {
                QTimer::singleShot(500, this, [this]() {
                    if (!m_quitting && !m_streaming &&
                        m_settingsRestartPending) {
                        restartEngineForSettings("Applying saved video settings.");
                    }
                });
            }
        }
    }
    if (m_engine == engine && m_engineWasReady &&
        containsVideoPipelineFailure(completeOutput)) {
        // The current renderer cannot recover this session; do not defer the
        // compatibility restart behind a stream that is already broken.
        m_streaming = false;
        const QString reason = QString::fromLocal8Bit(completeOutput).trimmed();
        if (fallbackToSoftwareRenderer(reason)) {
            QTimer::singleShot(0, this, [this]() {
                if (!m_quitting) {
                    restartEngineForSettings(
                        "Recovering from a failed video pipeline.");
                }
            });
        }
    }
    if (m_engine == engine && !m_running &&
        m_engineOutputBuffer.contains("Initialized server socket(s)")) {
        markEngineReady();
    }
    if (m_engineOutputBuffer.size() > 16384) {
        m_engineOutputBuffer = m_engineOutputBuffer.right(4096);
    }
}

void MainWindow::appendEngineLog(const QByteArray &data) {
    if (!m_engineLogFile.isOpen() || data.isEmpty()) return;
    m_engineLogFile.write(data);
    if (!data.endsWith('\n')) m_engineLogFile.write("\n");
}

QString MainWindow::engineExitReason(int exitCode,
                                     QProcess::ExitStatus exitStatus,
                                     qint64 lifetimeMs) const {
    const quint32 windowsCode = static_cast<quint32>(exitCode);
    QString crash;
    switch (windowsCode) {
    case 0xC0000135u: crash = "a required DLL is missing (0xC0000135)"; break;
    case 0xC000007Bu: crash = "an invalid 32/64-bit DLL was loaded (0xC000007B)"; break;
    case 0xC0000142u: crash = "a DLL failed to initialize (0xC0000142)"; break;
    case 0xC0000005u: crash = "access violation (0xC0000005)"; break;
    case 0xC0000409u: crash = "stack or security check failure (0xC0000409)"; break;
    default: break;
    }
    if (!crash.isEmpty()) return "Engine crashed: " + crash;

    const QString output = QString::fromLocal8Bit(m_engineOutputBuffer);
    const QStringList priorityMarkers = {
        "unknown option", "Required gstreamer plugin", "not found",
        "Error initializing", "Could not initialize", "failed", "stopping"
    };
    const QStringList lines = output.split(QRegularExpression("[\\r\\n]+"),
                                           Qt::SkipEmptyParts);
    for (const QString &marker : priorityMarkers) {
        for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
            if (it->contains(marker, Qt::CaseInsensitive)) {
                return it->trimmed().left(300);
            }
        }
    }
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (!line.isEmpty() && !line.startsWith("[engine-wrapper] process is exiting")) {
            return line.left(300);
        }
    }

    if (exitStatus == QProcess::CrashExit) {
        return QString("Engine crashed with Windows exit code 0x%1")
            .arg(windowsCode, 8, 16, QLatin1Char('0'));
    }
    if (exitCode != 0) {
        return QString("Engine exited with code %1 after %2 ms and produced no diagnostics")
            .arg(exitCode).arg(lifetimeMs);
    }
    return QString("Engine stopped during startup after %1 ms without an error message")
        .arg(lifetimeMs);
}

void MainWindow::markEngineReady() {
    m_starting = false;
    m_running = true;
    m_engineWasReady = true;
    m_lastEngineFailure.clear();
    m_restartDelayMs = 1000;
    updateStatus();
    qInfo() << "UxPlay engine is listening";
    QPointer<QProcess> readyEngine = m_engine;
    QTimer::singleShot(kMinimumHealthyLifetimeMs, this, [this, readyEngine]() {
        if (readyEngine && m_engine == readyEngine && m_running) {
            m_consecutiveEngineFailures = 0;
        }
    });
}

void MainWindow::scheduleEngineRestart() {
    if (m_quitting || m_engineRestartPending) return;
    const int delay = m_restartDelayMs;
    m_restartDelayMs = qMin(m_restartDelayMs * 2, 30000);
    m_engineRestartPending = true;
    qWarning() << "Restarting UxPlay engine in" << delay << "ms";
    QTimer::singleShot(delay, this, [this]() {
        m_engineRestartPending = false;
        if (!m_quitting && !m_engine) startServer();
    });
}

void MainWindow::retryEngine() {
    if (m_engine && m_engine->state() != QProcess::NotRunning) return;
    m_consecutiveEngineFailures = 0;
    m_restartDelayMs = 1000;
    m_lastEngineFailure.clear();
    startServer();
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
    } else if (!m_lastEngineFailure.isEmpty()) {
        status = "UxPlay server failed";
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
    if (m_qualityCombo && m_rendererCombo) {
        status += "\nVideo: " + m_qualityCombo->currentData().toString() +
                  " / " + m_rendererCombo->currentData().toString();
        if (m_lowLatencyCheckbox && m_lowLatencyCheckbox->isChecked()) {
            status += " / low latency";
        }
    }
    if (!m_lastEngineFailure.isEmpty() && !m_starting && !m_running) {
        status += "\nReason: " + m_lastEngineFailure.left(120);
    }
    m_statusLabel->setText(status);
    m_statusLabel->setToolTip(
        "Diagnostic logs: " + QDir::toNativeSeparators(diagnosticDirectory()));
    if (m_retryBonjourAction) {
        m_retryBonjourAction->setEnabled(!m_bonjourAvailable);
    }
    if (m_retryEngineAction) {
        m_retryEngineAction->setEnabled(!m_engine ||
                                        m_engine->state() == QProcess::NotRunning);
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
    const QString path = diagnosticDirectory();
    if (!QDir().mkpath(path)) {
        QMessageBox::warning(this, "Diagnostic Logs",
                             "Could not create the diagnostic log folder:\n" + path);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(this, "Diagnostic Logs",
                             "Could not open the diagnostic log folder:\n" + path);
    }
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
