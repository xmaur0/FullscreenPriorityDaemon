#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <iostream>
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>

const DWORD CHECK_INTERVAL_MS = 2000;         
const DWORD RESTORE_DELAY_MS = 15000;         // 15 segundos de espera
const DWORD TARGET_PRIORITY = ABOVE_NORMAL_PRIORITY_CLASS;
const DWORD BACKGROUND_PRIORITY = IDLE_PRIORITY_CLASS; 

struct ModifiedProcess {
    DWORD processId;
    HANDLE hProcess;
};
std::vector<ModifiedProcess> modifiedBackgrounds;

void KillPreviousInstances() {
    DWORD currentPid = GetCurrentProcessId();
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"game_debug.exe") == 0 && pe.th32ProcessID != currentPid) {
                HANDLE hOldProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hOldProc) {
                    TerminateProcess(hOldProc, 0);
                    CloseHandle(hOldProc);
                }
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
}

std::wstring GetProcessName(DWORD processId) {
    std::wstring processName = L"Desconhecido";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (hProcess) {
        wchar_t imagePath[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageName(hProcess, 0, imagePath, &size)) {
            wchar_t* fileName = wcsrchr(imagePath, L'\\');
            if (fileName) {
                processName = fileName + 1;
            } else {
                processName = imagePath;
            }
        }
        CloseHandle(hProcess);
    }
    return processName;
}

bool IsFullscreenWindow(HWND hwnd) {
    if (!hwnd) return false;

    HWND hwndDesktop = GetDesktopWindow();
    HWND hwndShell = GetShellWindow();
    if (hwnd == hwndDesktop || hwnd == hwndShell) return false;

    wchar_t className[256];
    if (GetClassName(hwnd, className, 256)) {
        if (lstrcmp(className, L"Progman") == 0 || 
            lstrcmp(className, L"WorkerW") == 0 || 
            lstrcmp(className, L"Shell_TrayWnd") == 0) {
            return false;
        }
    }

    RECT rcApp;
    if (!GetWindowRect(hwnd, &rcApp)) return false;

    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfo(hMonitor, &mi)) return false;

    RECT rcScreen = mi.rcMonitor;

    return (rcApp.left <= rcScreen.left &&
            rcApp.top <= rcScreen.top &&
            rcApp.right >= rcScreen.right &&
            rcApp.bottom >= rcScreen.bottom);
}

// Modo Agressivo: Tenta rebaixar absolutamente tudo o que o Windows permitir
void LowerAllProcessesAggressive(DWORD gamePid) {
    for (auto& mp : modifiedBackgrounds) {
        if (mp.hProcess) CloseHandle(mp.hProcess);
    }
    modifiedBackgrounds.clear();

    DWORD currentPid = GetCurrentProcessId();
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe)) {
        do {
            // Removemos as travas do Explorer e afins. Deixamos apenas o jogo, nosso app e o kernel base (PID <= 4) de fora.
            if (pe.th32ProcessID == gamePid || 
                pe.th32ProcessID == currentPid || 
                pe.th32ProcessID <= 4) {
                continue;
            }

            HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
            if (hProc) {
                // Tenta forçar IDLE. O Windows vai barrar os protegidos por segurança (Erro 5), mas aceitará os demais.
                if (SetPriorityClass(hProc, BACKGROUND_PRIORITY)) {
                    modifiedBackgrounds.push_back({pe.th32ProcessID, hProc});
                } else {
                    CloseHandle(hProc); // Descarta se o Windows recusar por proteção interna
                }
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    std::cout << "[+] Varredura agressiva aplicada. (Processos protegidos foram ignorados pelo Kernel)." << std::endl;
}

void RestoreAllProcesses() {
    for (auto& mp : modifiedBackgrounds) {
        if (mp.hProcess != NULL) {
            SetPriorityClass(mp.hProcess, NORMAL_PRIORITY_CLASS);
            CloseHandle(mp.hProcess);
        }
    }
    modifiedBackgrounds.clear();
    std::cout << "[*] Todos os processos alterados foram restaurados para NORMAL." << std::endl;
}

int main() {
    KillPreviousInstances();
    SetConsoleOutputCP(65001);

    std::cout << "====================================================" << std::endl;
    std::cout << "    GAME BOOSTER - MODO AGRESSIVO TOTAL (TESTE)     " << std::endl;
    std::cout << "====================================================" << std::endl;
    std::cout << "Execute como ADMINISTRADOR!" << std::endl;
    std::cout << "====================================================" << std::endl << std::endl;

    DWORD lastProcessId = 0;
    HANDLE hLastProcess = NULL;
    ULONGLONG unfocusTimestamp = 0;
    bool waitingToRestore = false;

    while (true) {
        HWND hwndForeground = GetForegroundWindow();

        if (hwndForeground && IsFullscreenWindow(hwndForeground)) {
            DWORD currentProcessId = 0;
            GetWindowThreadProcessId(hwndForeground, &currentProcessId);

            HWND hwndOwner = GetWindow(hwndForeground, GW_OWNER);
            if (hwndOwner) {
                DWORD ownerPid = 0;
                GetWindowThreadProcessId(hwndOwner, &ownerPid);
                if (ownerPid != 0) currentProcessId = ownerPid;
            }

            std::wstring procName = GetProcessName(currentProcessId);

            if (procName != L"explorer.exe" && currentProcessId != 0) {
                
                if (currentProcessId != lastProcessId) {
                    if (hLastProcess != NULL && waitingToRestore) {
                        SetPriorityClass(hLastProcess, NORMAL_PRIORITY_CLASS);
                        CloseHandle(hLastProcess);
                        hLastProcess = NULL;
                    }

                    if (hLastProcess != NULL) {
                        CloseHandle(hLastProcess);
                        hLastProcess = NULL;
                    }

                    hLastProcess = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, currentProcessId);
                    lastProcessId = currentProcessId;

                    // Executa a varredura agressiva
                    LowerAllProcessesAggressive(currentProcessId);
                }

                if (hLastProcess != NULL) {
                    SetPriorityClass(hLastProcess, TARGET_PRIORITY);
                }

                waitingToRestore = false;
            }
        } 
        else {
            if (hLastProcess != NULL && !waitingToRestore) {
                waitingToRestore = true;
                unfocusTimestamp = GetTickCount64();
                std::wcout << L"[-] Jogo saiu da tela cheia. Aguardando 15 segundos..." << std::endl;
            }
        }

        if (waitingToRestore && hLastProcess != NULL) {
            ULONGLONG elapsed = GetTickCount64() - unfocusTimestamp;
            if (elapsed >= RESTORE_DELAY_MS) {
                SetPriorityClass(hLastProcess, NORMAL_PRIORITY_CLASS);
                std::wcout << L"[*] 15 segundos esgotados. Jogo restaurado." << std::endl;
                
                CloseHandle(hLastProcess);
                hLastProcess = NULL;
                lastProcessId = 0;
                waitingToRestore = false;

                RestoreAllProcesses();
            }
        }

        Sleep(CHECK_INTERVAL_MS);
    }

    return 0;
}