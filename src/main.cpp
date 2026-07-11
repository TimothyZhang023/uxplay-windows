#include "mainwindow.h"
#include "single_instance.hpp"
#include <QApplication>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QDir>
#include <QProcessEnvironment>
#include <QIcon>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#endif

namespace {
QFile g_logFile;
QMutex g_logMutex;
bool g_forwardLogToConsole = false;

void fileMessageHandler(QtMsgType type,
                        const QMessageLogContext &context,
                        const QString &message) {
    const QByteArray line = (qFormatLogMessage(type, context, message) + '\n').toUtf8();
    {
        QMutexLocker locker(&g_logMutex);
        if (g_logFile.isOpen()) {
            g_logFile.write(line);
            g_logFile.flush();
        }
    }
#ifdef _WIN32
    const QString debugLine = QString::fromUtf8(line);
    OutputDebugStringW(reinterpret_cast<const wchar_t *>(debugLine.utf16()));
#endif
    if (g_forwardLogToConsole) {
        fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
        fflush(stderr);
    }
}

void rotateLogIfNeeded(const QString &path, qint64 maximumSize) {
    QFileInfo info(path);
    if (!info.exists() || info.size() <= maximumSize) return;
    const QString oldPath = path + ".old";
    QFile::remove(oldPath);
    QFile::rename(path, oldPath);
}
} // namespace

int main(int argc, char *argv[]) {
    bool attachedConsole = false;
#ifdef _WIN32
    // if the process was started from a console (CMD/PowerShell), attach to it so we can see qDebug() output.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        attachedConsole = true;
        // redirect stdout and stderr to the console
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
        std::ios::sync_with_stdio();
    }
#endif

    QApplication app(argc, argv);
    app.setOrganizationName("leapbtw");
    app.setApplicationName("uxplay-windows");
    app.setWindowIcon(QIcon(QApplication::applicationDirPath() + "/resources/icon.ico"));

    // Acquire before touching the shared log or launching any helper process.
    if (!single_instance::acquire()) {
        QMessageBox::information(
            nullptr,
            "uxplay-windows",
            "uxplay-windows is already running. Use its system tray icon."
        );
        return 0;
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     &app, &single_instance::release);

#ifdef _WIN32
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(appData);
    const QString logPath = appData + "/uxplay-windows.log";
    rotateLogIfNeeded(logPath, 5 * 1024 * 1024);
    g_forwardLogToConsole = attachedConsole;
    g_logFile.setFileName(logPath);
    const bool logReady = g_logFile.open(QIODevice::WriteOnly |
                                         QIODevice::Append |
                                         QIODevice::Text);
    qSetMessagePattern("[%{time yyyy-MM-dd hh:mm:ss.zzz}] [%{type}] %{message}");
    qInstallMessageHandler(fileMessageHandler);
    qInfo() << "uxplay-windows starting";
    qInfo() << "Log file:" << logPath;
    if (!logReady) {
        QMessageBox::warning(
            nullptr,
            "Diagnostic Log Unavailable",
            "uxplay-windows could not create its diagnostic log:\n" + logPath +
            "\n\nThe server will continue starting. Check folder permissions if troubleshooting is needed."
        );
    }
#endif
    
    QString appPath = QApplication::applicationDirPath();
    
    QString pluginPath = QDir::toNativeSeparators(appPath + "/lib/gstreamer-1.0");
    qputenv("GST_PLUGIN_PATH", pluginPath.toUtf8());
    qputenv("GST_PLUGIN_PATH_1_0", pluginPath.toUtf8());
    qputenv("GST_PLUGIN_SYSTEM_PATH_1_0", pluginPath.toUtf8());

    const QString scannerPath = QDir::toNativeSeparators(
        appPath + "/libexec/gstreamer-1.0/gst-plugin-scanner.exe");
    if (QFileInfo::exists(scannerPath)) {
        qputenv("GST_PLUGIN_SCANNER", scannerPath.toUtf8());
        qputenv("GST_PLUGIN_SCANNER_1_0", scannerPath.toUtf8());
    } else {
        qWarning() << "GStreamer plugin scanner is missing:" << scannerPath;
    }

    const QString gstRegistry = QDir::toNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        "/gstreamer-registry.bin");
    qputenv("GST_REGISTRY_1_0", gstRegistry.toUtf8());
    qInfo() << "GStreamer plugins:" << pluginPath;
    qInfo() << "GStreamer scanner:" << scannerPath;
    qInfo() << "GStreamer registry:" << gstRegistry;

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString path = QDir::toNativeSeparators(appPath) + ";" + env.value("PATH");
    qputenv("PATH", path.toUtf8());

    app.setQuitOnLastWindowClosed(false);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "Error", "System tray not available.");
        single_instance::release();
        return 1;
    }

    MainWindow window;
    return app.exec();
}
