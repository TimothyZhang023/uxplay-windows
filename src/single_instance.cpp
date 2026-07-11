#include "single_instance.hpp"

#include <windows.h>

namespace single_instance {
namespace {
HANDLE instanceMutex = nullptr;
}

bool acquire() {
    if (instanceMutex) return true;
    instanceMutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Local\\TimothyZhang023.uxplay-windows.single-instance");
    if (!instanceMutex) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        instanceMutex = nullptr;
        return false;
    }
    return true;
}

void release() {
    if (!instanceMutex) return;
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
    instanceMutex = nullptr;
}
} // namespace single_instance
