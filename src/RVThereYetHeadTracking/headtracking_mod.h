#pragma once

namespace RVThereYetHeadTracking
{
    // Also pins the DLL for the process lifetime: with hooks and worker
    // threads live, an unmap (transient LoadLibrary/FreeLibrary of dxgi.dll
    // by anything in the process) would leave threads executing freed code.
    void Initialize(void* hModule);

    // Full cleanup. Joins worker threads, removes hooks, releases the log.
    // Safe only when other threads in the process are still live - i.e. a
    // manual FreeLibrary (DLL_PROCESS_DETACH with lpReserved == NULL). Since
    // Initialize pins the module, this path is unreachable in practice; it is
    // kept correct for any future non-DllMain teardown.
    void Shutdown();

    // No-cleanup variant for the DLL_PROCESS_DETACH-during-process-exit path
    // (lpReserved != NULL). Other threads have been killed by the kernel
    // without unwinding; any mutex they held is still "locked", and
    // std::thread::join on a kernel-terminated thread can hang or crash. Just
    // write a final log line and let the OS reclaim state.
    void EmergencyShutdown();
}
