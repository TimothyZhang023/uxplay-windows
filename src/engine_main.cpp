#include "uxplay_api.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void logNormalExit() {
    fprintf(stderr, "[engine-wrapper] process is exiting\n");
    fflush(stderr);
}

#ifdef _WIN32
LONG WINAPI logUnhandledException(EXCEPTION_POINTERS *info) {
    const unsigned long code = info && info->ExceptionRecord
        ? info->ExceptionRecord->ExceptionCode
        : 0;
    fprintf(stderr, "[engine-wrapper] unhandled Windows exception 0x%08lX\n", code);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
} // namespace

int main(int argc, char *argv[]) {
    // UxPlay parses some arguments with exit(), before its own Windows logging
    // setup runs. Make both streams unbuffered immediately so the supervising
    // GUI always receives the final diagnostic lines.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetUnhandledExceptionFilter(logUnhandledException);
#endif
    std::atexit(logNormalExit);
    fprintf(stderr, "[engine-wrapper] starting uxplay engine\n");
    return start_uxplay(argc, argv);
}
