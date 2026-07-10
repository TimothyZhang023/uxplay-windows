#include "airplayworker.h"
#include "uxplay_api.h"
#include <QDebug>
#include <vector>

AirPlayWorker::AirPlayWorker(QObject *parent) : QThread(parent) {}

void AirPlayWorker::setArgs(const QStringList &args) {
    m_args = args;
}

void AirPlayWorker::run() {
    std::vector<QByteArray> argBytes;
    argBytes.reserve(static_cast<std::size_t>(m_args.size()) + 1);
    argBytes.push_back(QByteArray("uxplay"));

    for (const auto &arg : m_args) {
        if (arg.trimmed().isEmpty()) continue;
        argBytes.push_back(arg.toUtf8());
    }

    // Build argv only after argBytes has reached its final size. Pointers into
    // a std::vector may otherwise be invalidated when the vector grows.
    std::vector<char *> argv;
    argv.reserve(argBytes.size());
    for (auto &arg : argBytes) {
        argv.push_back(arg.data());
    }

    qDebug() << "Starting UxPlay engine with arguments:" << m_args;
    emit started();

    int ret = 0;

    // Loop con controllo di interruzione
    while (!isInterruptionRequested()) {
        ret = start_uxplay(static_cast<int>(argv.size()), argv.data());

        // Se il processo termina, esci dal loop
        if (ret != 0) {
            emit errorOccurred(QString("Engine exited with code %1").arg(ret));
            break;
        }
    }

    emit stopped();
}

void AirPlayWorker::stopAirplay() {
    stop_uxplay();
}
