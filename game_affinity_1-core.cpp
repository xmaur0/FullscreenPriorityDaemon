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
const DWORD TARGET_PRIORITY = HIGH_PRIORITY_CLASS;
const DWORD BACKGROUND_PRIORITY = IDLE_PRIORITY_CLASS; 

struct ModifiedProcess {
    DWORD processId;
    DWORD_PTR originalAffinityMask; // Salvamos apenas o PID e a máscara original
};
std::vector<ModifiedProcess> modifiedBackgrounds;
DWORD_PTR lastCoreMask = 0;

HANDLE hSingleInstanceMutex = NULL;
const wchar_t* MUTEX_NAME = L"Local\\GameBooster_SingleInstance_Mutex_UniqueKey";

bool CheckSingleInstance() {
    hSingleInstanceMutex = CreateMutex(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hSingleInstanceMutex) {
            CloseHandle(hSingleInstanceMutex);
            hSingleInstanceMutex = NULL;
        }
        return false;
    }
    return true;
}

DWORD_PTR GetLastCoreMask() {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD totalCores = sysInfo.dwNumberOfProcessors;
    if (totalCores == 0) return 1;
    return (DWORD_PTR)1 << (totalCores - 1);
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

void LowerAllProcessesAggressive(DWORD gamePid) {
    modifiedBackgrounds.clear();

    DWORD currentPid = GetCurrentProcessId();
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe)) {
        do {
            if (pe.th32ProcessID == gamePid || 
                pe.th32ProcessID == currentPid || 
                pe.th32ProcessID <= 4) {
                continue;
            }

            HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
            if (hProc) {
                DWORD_PTR processAffinity = 0, systemAffinity = 0;
                if (GetProcessAffinityMask(hProc, &processAffinity, &systemAffinity)) {
                    // Aplica prioridade IDLE e restringe afinidade ao último núcleo
                    if (SetPriorityClass(hProc, BACKGROUND_PRIORITY) && SetProcessAffinityMask(hProc, lastCoreMask)) {
                        // Guardamos apenas o PID e a máscara original para reabrir o handle com segurança na hora de restaurar
                        modifiedBackgrounds.push_back({pe.th32ProcessID, processAffinity});
                    }
                }
                CloseHandle(hProc);
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
}

void RestoreAllProcesses() {
    for (auto& mp : modifiedBackgrounds) {
        // Reabre o processo ativamente pelo PID na hora de restaurar
        HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, mp.processId);
        if (hProc != NULL) {
            SetPriorityClass(hProc, NORMAL_PRIORITY_CLASS);
            SetProcessAffinityMask(hProc, mp.originalAffinityMask); // Devolve todos os núcleos originais
            CloseHandle(hProc);
        }
    }
    modifiedBackgrounds.clear();
    std::wcout << L"[*] Todos os processos de fundo foram restaurados para NORMAL e afinidade original." << std::endl;
}

int main() {
    SetConsoleOutputCP(65001);

    if (!CheckSingleInstance()) {
        std::wcout << L"[!] Ja existe uma instancia deste programa em execucao. Fechando..." << std::endl;
        Sleep(2000);
        return 0;
    }

    lastCoreMask = GetLastCoreMask();

    std::cout << "====================================================" << std::endl;
    std::cout << "    GAME BOOSTER - RESTAURACAO SEGURA ATIVADA       " << std::endl;
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
            }
        }

        if (waitingToRestore && hLastProcess != NULL) {
            ULONGLONG elapsed = GetTickCount64() - unfocusTimestamp;
            if (elapsed >= RESTORE_DELAY_MS) {
                SetPriorityClass(hLastProcess, NORMAL_PRIORITY_CLASS);
                
                CloseHandle(hLastProcess);
                hLastProcess = NULL;
                lastProcessId = 0;
                waitingToRestore = false;

                RestoreAllProcesses(); // Agora reabre cada processo de forma segura pelo PID e restaura os cores
            }
        }

        Sleep(CHECK_INTERVAL_MS);
    }

    if (hSingleInstanceMutex) {
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
    }

    return 0;
}