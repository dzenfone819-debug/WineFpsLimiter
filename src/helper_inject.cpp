// Minimal helper injector: launches target suspended, injects specified DLL, resumes process.
// Usage: helper_inject "C:\path\to\game.exe" "C:\path\to\dll.dll"

#include <windows.h>
#include <string>
#include <vector>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: helper_inject <target_exe> <dll_path>\n";
        return 2;
    }

    const char* target = argv[1];
    const char* dllPath = argv[2];

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    std::string cmd = std::string("\"") + target + "\"";
    // Create suspended so we can inject
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        std::cerr << "CreateProcess failed: " << GetLastError() << "\n";
        return 3;
    }

    void* pLoadLibrary = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (!pLoadLibrary) {
        std::cerr << "GetProcAddress failed\n";
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 4;
    }

    void* pRemote = VirtualAllocEx(pi.hProcess, NULL, strlen(dllPath) + 1, MEM_COMMIT, PAGE_READWRITE);
    if (!pRemote) {
        std::cerr << "VirtualAllocEx failed\n";
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 5;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(pi.hProcess, pRemote, dllPath, strlen(dllPath) + 1, &written)) {
        std::cerr << "WriteProcessMemory failed\n";
        VirtualFreeEx(pi.hProcess, pRemote, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 6;
    }

    HANDLE hThread = CreateRemoteThread(pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibrary, pRemote, 0, NULL);
    if (!hThread) {
        std::cerr << "CreateRemoteThread failed: " << GetLastError() << "\n";
        VirtualFreeEx(pi.hProcess, pRemote, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 7;
    }

    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    VirtualFreeEx(pi.hProcess, pRemote, 0, MEM_RELEASE);

    // Resume target and exit
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
