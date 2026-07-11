#pragma once

#include <string>

#include <QLabel>
#include <QMainWindow>
#include <QComboBox>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QProcess>
#include <QCheckBox>
#include <QByteArray>
#include <QFile>
#include <QMessageBox>
#include <QPointer>
#include <QRect>

class QMenu;
class QAction;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message,
                     qintptr *result) override;

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void toggleAutostart();
    void showLicense();
    void openSettingsFile();
    void openListArgsFile();
    void quit();
    void retryBonjourDiscovery();
    void openLogFile();
    void retryEngine();
    void toggleBle(bool checked); // bluetooth
    void toggleForceFullscreen(bool checked);
    void toggleLowLatency(bool checked);
    void toggleVideoFullscreen();
    void onRendererChanged(int index);
    void onQualityChanged(int index);


private:
    void startServer();
    void stopServer();
    void setupTray();
    void setupUI();
    void updateStatus();
    bool startBluetoothBeacon(const QString &path);
    void stopBluetoothBeacon();
    void applyRendererAndFullscreenArgs(QStringList &args);
    void applyQualityAndLatencyArgs(QStringList &args);
    void restartEngineForSettings(const QString &message);
    quintptr findVideoWindow() const;
    void setVideoFullscreen(bool fullscreen);
    void monitorVideoWindow();
    void showDiscoveryFallbackWarning();
    void handleEngineOutput(QProcess *engine);
    void markEngineReady();
    void scheduleEngineRestart();
    QString engineExitReason(int exitCode, QProcess::ExitStatus exitStatus,
                             qint64 lifetimeMs) const;
    void appendEngineLog(const QByteArray &data);
    void scheduleBluetoothBeaconRestart();

    QPointer<QProcess> m_beacon;
    QString m_blePath;

    QStringList getArgumentsFromFile();
    void ensureSettingsFileExists();
    bool ensureBonjourServiceAvailable(bool allowRepair);
    void restartApplication();

    bool isAutostartEnabled() const;
    void setAutostart(bool enabled);

    QCheckBox *m_bleCheckbox = nullptr;
    QCheckBox *m_fullscreenCheckbox = nullptr;
    QCheckBox *m_lowLatencyCheckbox = nullptr;
    QComboBox *m_rendererCombo = nullptr;
    QComboBox *m_qualityCombo = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_trayMenu = nullptr;
    QAction *m_autostartAction = nullptr;
    QAction *m_statusAction = nullptr;
    QAction *m_retryBonjourAction = nullptr;
    QAction *m_retryEngineAction = nullptr;
    QAction *m_toggleFullscreenAction = nullptr;

    QPushButton *m_autostartBtn = nullptr;
    QPushButton *m_settingsBtn = nullptr;
    QPushButton *m_listargsBtn = nullptr;
    QPushButton *m_licenseBtn = nullptr;
    QPushButton *m_logsBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_windowMonitorTimer = nullptr;

    QPointer<QProcess> m_engine;
    QByteArray m_engineOutputBuffer;
    QFile m_engineLogFile;
    quint64 m_enginePid = 0;
    qint64 m_engineStartedAtMs = 0;
    quintptr m_videoWindow = 0;
    qintptr m_windowedStyle = 0;
    QRect m_windowedRect;

    bool m_running = false;
    bool m_starting = false;
    bool m_quitting = false;
    bool m_bonjourAvailable = false;
    bool m_forceBleFallback = false;
    bool m_bleAvailable = false;
    bool m_engineWasReady = false;
    bool m_engineRestartPending = false;
    bool m_videoFullscreen = false;
    bool m_altEnterHotkeyRegistered = false;
    bool m_registryRepairAttempted = false;
    int m_consecutiveEngineFailures = 0;
    QString m_lastEngineFailure;
    int m_restartDelayMs = 1000;
    int m_beaconRestartDelayMs = 1000;
    bool m_beaconRestartPending = false;
};
