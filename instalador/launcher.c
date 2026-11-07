// Instalar-Ocarina.exe: opens instalador\Instalar-Ocarina.ps1, the script beside it in the ZIP, with PowerShell, and waits for it.
// It does nothing else, and the script is a plain text file anyone can read. Build with instalador/build_exe.sh.
#include <windows.h>

int WINAPI WinMain(HINSTANCE self, HINSTANCE prev, LPSTR line, int show) {
    (void)self, (void)prev, (void)line, (void)show;
    wchar_t dir[MAX_PATH], script[MAX_PATH + 64], sys[MAX_PATH], cmd[2 * MAX_PATH + 192];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\') n--;
    if (n) dir[n - 1] = 0;
    wsprintfW(script, L"%s\\instalador\\Instalar-Ocarina.ps1", dir);
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"Extraia o ZIP inteiro para uma pasta e abra o instalador de dentro dela.\n\nUnpack the whole ZIP to a folder and open the installer from inside it.",
                    L"Ocarina Recomp", MB_ICONWARNING);
        return 2;
    }
    SetEnvironmentVariableW(L"OCARINA_SETUP", dir);   // the installer finds the ZIP's other folders from here
    GetSystemDirectoryW(sys, MAX_PATH);   // by full path: a PATH that is too long or broken must not hide it
    wsprintfW(cmd, L"\"%s\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -Sta -WindowStyle Hidden -ExecutionPolicy Bypass -File \"%s\"", sys, script);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        MessageBoxW(NULL, L"PowerShell not found.", L"Ocarina Recomp", MB_ICONERROR);
        return 3;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    return 0;
}
