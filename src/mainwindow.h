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
#include <QMessageBox>
#include <QPointer>

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

private slots:
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void toggleAutostart();
    void showLicense();
    void openSettingsFile();
    void openListArgsFile();
    void quit();
    void retryBonjourDiscovery();
    void openLogFile();
    void toggleBle(bool checked); // bluetooth
    void toggleForceFullscreen(bool checked);
    void onRendererChanged(int index);


private:
    void startServer();
    void stopServer();
    void setupTray();
    void setupUI();
    void updateStatus();
    bool startBluetoothBeacon(const QString &path);
    void stopBluetoothBeacon();
    void applyRendererAndFullscreenArgs(QStringList &args);
    void showDiscoveryFallbackWarning();
    void handleEngineOutput(QProcess *engine);
    void markEngineReady();
    void scheduleEngineRestart();
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
    QComboBox *m_rendererCombo = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_trayMenu = nullptr;
    QAction *m_autostartAction = nullptr;
    QAction *m_statusAction = nullptr;
    QAction *m_retryBonjourAction = nullptr;

    QPushButton *m_autostartBtn = nullptr;
    QPushButton *m_settingsBtn = nullptr;
    QPushButton *m_listargsBtn = nullptr;
    QPushButton *m_licenseBtn = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_windowMonitorTimer = nullptr;

    QPointer<QProcess> m_engine;
    QByteArray m_engineOutputBuffer;
    quint64 m_enginePid = 0;

    bool m_running = false;
    bool m_starting = false;
    bool m_quitting = false;
    bool m_bonjourAvailable = false;
    bool m_forceBleFallback = false;
    bool m_bleAvailable = false;
    int m_restartDelayMs = 1000;
    int m_beaconRestartDelayMs = 1000;
    bool m_beaconRestartPending = false;
};
