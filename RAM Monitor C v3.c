// ram_monitor.c
#define _WIN32_WINNT 0x0601
#define NTDDI_VERSION 0x06010000
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")

#define ID_TIMER 201
#define IDM_FILL_RAM 301
#define IDM_STAY_ON_TOP 302
#define IDM_EXIT 303

#define IDI_ICON1 101
#define INI_FILE "settings.ini"

#define COLOR_BG RGB(30, 30, 30)
#define COLOR_TEXT RGB(255, 255, 255)
#define COLOR_RAM_LABEL RGB(128, 212, 255)
#define COLOR_PAGE_LABEL RGB(255, 204, 128)
#define COLOR_PROGRESS_BG RGB(60, 60, 60)
#define COLOR_PROGRESS_RAM RGB(50, 255, 50)
#define COLOR_PROGRESS_PAGE RGB(50, 255, 50)

typedef struct {
    ULONGLONG totalPhys;
    ULONGLONG availPhys;
    ULONGLONG totalPageFile;
    ULONGLONG availPageFile;
    double physUsedPercent;
    double pageUsedPercent;
} MemInfo;

typedef struct {
    void** allocations;
    int count;
    int capacity;
    size_t totalAllocated;
    BOOL isFilling;
    int releaseCountdownTicks;
    int animFrame;
    volatile BOOL stopRequested; // NEW: stop signal for FillThreadProc
} RamFiller;

char g_iniPath[MAX_PATH] = {0};


HWND g_hwnd = NULL;
HBRUSH g_bgBrush = NULL;
HBRUSH g_progressBgBrush = NULL;
HFONT g_font = NULL;
RamFiller g_filler = {0};
CRITICAL_SECTION g_cs;
BOOL g_stayOnTop = TRUE; // default: on top

COLORREF ColorForPercent(double pct) {
    if (pct < 25) return RGB(0x00,0xff,0x66);
    else if (pct < 30) return RGB(0x33,0xff,0x33);
    else if (pct < 35) return RGB(0x66,0xff,0x00);
    else if (pct < 40) return RGB(0x99,0xff,0x00);
    else if (pct < 45) return RGB(0xcc,0xff,0x00);
    else if (pct < 50) return RGB(0xff,0xff,0x00);
    else if (pct < 55) return RGB(0xff,0xcc,0x00);
    else if (pct < 60) return RGB(0xff,0x99,0x00);
    else if (pct < 65) return RGB(0xff,0x66,0x00);
    else if (pct < 70) return RGB(0xff,0x33,0x00);
    else if (pct < 75) return RGB(0xff,0x00,0x33);
    else if (pct < 80) return RGB(0xff,0x00,0x66);
    else if (pct < 85) return RGB(0xff,0x00,0x99);
    else if (pct < 90) return RGB(0xff,0x00,0xcc);
    else if (pct < 95) return RGB(0xff,0x00,0xff);
    else return RGB(0xff,0x33,0x99);
}


// Forward declarations
void GetMemoryInfo(MemInfo* info);
void FormatBytes(ULONGLONG bytes, char* buffer, size_t bufSize);
void DrawProgressBar(HDC hdc, int x, int y, int width, int height, double percent, COLORREF barColor);
void UpdateDisplay(void);
void ReleaseAllMemory(void);
DWORD WINAPI FillThreadProc(LPVOID lpParam);

// --- Implementation ----------------------------------------------------------------

void GetMemoryInfo(MemInfo* info) {
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    GlobalMemoryStatusEx(&statex);

    PERFORMANCE_INFORMATION perfInfo;
    perfInfo.cb = sizeof(PERFORMANCE_INFORMATION);
    GetPerformanceInfo(&perfInfo, sizeof(PERFORMANCE_INFORMATION));

    info->totalPhys = statex.ullTotalPhys;
    info->availPhys = statex.ullAvailPhys;

    // pagefile total (CommitLimit - PhysicalTotal) * PageSize
    info->totalPageFile = (ULONGLONG)(perfInfo.CommitLimit - perfInfo.PhysicalTotal) * perfInfo.PageSize;

    ULONGLONG usedPhys = info->totalPhys - info->availPhys;
    info->physUsedPercent = (double)usedPhys / (double)info->totalPhys * 100.0;

    ULONGLONG totalVirtualUsed = statex.ullTotalPageFile - statex.ullAvailPageFile;
    // available in pagefile = totalPageFile - (virtualUsed - physUsed)
    info->availPageFile = info->totalPageFile;
    if (totalVirtualUsed >= usedPhys) {
        ULONGLONG pageUsed = totalVirtualUsed - usedPhys;
        if (info->totalPageFile > pageUsed) {
            info->availPageFile = info->totalPageFile - pageUsed;
        } else {
            info->availPageFile = 0;
        }
        info->pageUsedPercent = (double)pageUsed / (double)info->totalPageFile * 100.0;
    } else {
        // odd case; no page used
        info->pageUsedPercent = 0.0;
    }
}

void FormatBytes(ULONGLONG bytes, char* buffer, size_t bufSize) {
    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    snprintf(buffer, bufSize, "%.2f", gb);
}

void DrawProgressBar(HDC hdc, int x, int y, int width, int height, double percent, COLORREF barColor) {
    // Clamp percentage to [0, 100]
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;

    HBRUSH borderBrush = CreateSolidBrush(RGB(80,80,80));
    RECT border = { x - 1, y - 1, x + width + 1, y + height + 1 };
    FrameRect(hdc, &border, borderBrush);
    DeleteObject(borderBrush);

    RECT bgRect = { x, y, x + width, y + height };
    FillRect(hdc, &bgRect, g_progressBgBrush);

    int fillWidth = (int)(width * (percent / 100.0));
    if (fillWidth > 0) {
        // Ensure fill never exceeds the bar width (in case rounding pushes it past)
        if (fillWidth > width) fillWidth = width;

        RECT fillRect = { x, y, x + fillWidth, y + height };
        HBRUSH fillBrush = CreateSolidBrush(barColor);
        FillRect(hdc, &fillRect, fillBrush);
        DeleteObject(fillBrush);
    }
}


void UpdateDisplay(void) {
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

void ReleaseAllMemory(void) {
    EnterCriticalSection(&g_cs);

    if (g_filler.allocations) {
        for (int i = 0; i < g_filler.count; ++i) {
            if (g_filler.allocations[i]) {
                VirtualFree(g_filler.allocations[i], 0, MEM_RELEASE);  // full release
                g_filler.allocations[i] = NULL;
            }
        }
        free(g_filler.allocations);
        g_filler.allocations = NULL;
    }
    g_filler.count = 0;
    g_filler.capacity = 0;
    g_filler.totalAllocated = 0;
    g_filler.isFilling = FALSE;
    g_filler.releaseCountdownTicks = 0;
    g_filler.animFrame = 0;

    LeaveCriticalSection(&g_cs);
}

DWORD WINAPI FillThreadProc(LPVOID lpParam) {
    EnterCriticalSection(&g_cs);
    g_filler.totalAllocated = 0;
    g_filler.count = 0;
    g_filler.capacity = 0;
    g_filler.allocations = NULL;
    g_filler.isFilling = TRUE;
    g_filler.releaseCountdownTicks = 0;
    g_filler.animFrame = 0;
    g_filler.stopRequested = FALSE;
    LeaveCriticalSection(&g_cs);

    MemInfo info;
    GetMemoryInfo(&info);

    ULONGLONG pagefile_stop_threshold = 1ULL * 1024 * 1024 * 1024; // 1 GB free
    ULONGLONG availableToFill = info.availPhys;
    //ULONGLONG targetToFill = (ULONGLONG)(availableToFill * 0.85);
    ULONGLONG targetToFill = (ULONGLONG)(availableToFill * 2);

    size_t chunkSize;
    int delayMs;

    if (info.totalPhys < 4ULL * 1024 * 1024 * 1024ULL) {
        chunkSize = 50 * 1024 * 1024;
        delayMs = 200;
    } else if (info.totalPhys < 8ULL * 1024 * 1024 * 1024ULL) {
        chunkSize = 100 * 1024 * 1024;
        delayMs = 100;
    } else {
        chunkSize = 200 * 1024 * 1024;
        delayMs = 50;
    }

    //int cap = (int)(targetToFill / chunkSize) + 8;
    //if (cap < 32) cap = 32;
	int cap = 128;

    EnterCriticalSection(&g_cs);
    g_filler.allocations = (void**)malloc(cap * sizeof(void*));
    if (!g_filler.allocations) {
        g_filler.capacity = 0;
        g_filler.count = 0;
        g_filler.isFilling = FALSE;
        LeaveCriticalSection(&g_cs);
        return 1;
    }
    g_filler.capacity = cap;
    g_filler.count = 0;
    LeaveCriticalSection(&g_cs);

    while (1) {
        EnterCriticalSection(&g_cs);
        if (g_filler.stopRequested) {
            LeaveCriticalSection(&g_cs);
            break;
        }
        LeaveCriticalSection(&g_cs);

        GetMemoryInfo(&info);
        if (info.availPageFile <= pagefile_stop_threshold) {
            EnterCriticalSection(&g_cs);
            g_filler.releaseCountdownTicks = 10;
            LeaveCriticalSection(&g_cs);
            break;
        }

        void* ptr = VirtualAlloc(NULL, chunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!ptr) {
            EnterCriticalSection(&g_cs);
            g_filler.releaseCountdownTicks = 5;
            LeaveCriticalSection(&g_cs);
            break;
        }

        memset(ptr, 0xFF, chunkSize);

        EnterCriticalSection(&g_cs);
        if (g_filler.count >= g_filler.capacity) {
            int newcap = g_filler.capacity * 2;
            if (newcap < 64) newcap = 64;
            void** tmp = (void**)realloc(g_filler.allocations, newcap * sizeof(void*));
            if (!tmp) {
                LeaveCriticalSection(&g_cs);
                VirtualFree(ptr, 0, MEM_RELEASE);
                EnterCriticalSection(&g_cs);
                g_filler.releaseCountdownTicks = 5;
                LeaveCriticalSection(&g_cs);
                break;
            }
            g_filler.allocations = tmp;
            g_filler.capacity = newcap;
        }
        g_filler.allocations[g_filler.count++] = ptr;
        g_filler.totalAllocated += chunkSize;
        LeaveCriticalSection(&g_cs);

        UpdateDisplay();

        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        Sleep(delayMs);
    }

    // Thread exits; timer code will release allocations when countdown hits zero
    return 0;
}


// --- Window proc ------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            InitializeCriticalSection(&g_cs);
            g_bgBrush = CreateSolidBrush(COLOR_BG);
            g_progressBgBrush = CreateSolidBrush(COLOR_PROGRESS_BG);
            g_font = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

            // ensure initially on top if requested
            if (g_stayOnTop) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
            }

            SetTimer(hwnd, ID_TIMER, 500, NULL); // 500 ms
            break;
        }

        case WM_TIMER:
            if (wParam == ID_TIMER) {
                // animate title when filling or when countdown running
                EnterCriticalSection(&g_cs);
                BOOL filling = g_filler.isFilling;
                int countdown = g_filler.releaseCountdownTicks;
                if (filling || countdown > 0) {
                    g_filler.animFrame = (g_filler.animFrame + 1) % 8;
                    char title[256];
                    int exclamationCount = (g_filler.animFrame / 2) + 1; // 1..4
                    if (exclamationCount > 4) exclamationCount = 4;
                    char excls[8] = {0};
                    for (int i=0;i<exclamationCount;i++) excls[i] = '!';
                    snprintf(title, sizeof(title), "RAM & Pagefile Monitor - RAM STRESS TEST ONGOING%s", excls);
                    SetWindowTextA(hwnd, title);
                } else {
                    SetWindowTextA(hwnd, "RAM & Pagefile Monitor");
                }

                if (countdown > 0) {
                    g_filler.releaseCountdownTicks--;
                    if (g_filler.releaseCountdownTicks <= 0) {
                        // release memory after countdown
                        ReleaseAllMemory();
                        SetWindowTextA(hwnd, "RAM & Pagefile Monitor");
                        UpdateDisplay();
                    }
                }
                LeaveCriticalSection(&g_cs);

                UpdateDisplay();
            }
            break;

        case WM_CONTEXTMENU: {
            // Build popup
            HMENU hMenu = CreatePopupMenu();
            // Start/Stop label
            EnterCriticalSection(&g_cs);
            BOOL filling = g_filler.isFilling;
            LeaveCriticalSection(&g_cs);

            if (!filling) AppendMenuA(hMenu, MF_STRING, IDM_FILL_RAM, "Start RAM Fill Test");
            else AppendMenuA(hMenu, MF_STRING, IDM_FILL_RAM, "Stop & Release RAM");

            // Stay on top toggle (checked if on)
            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(hMenu, MF_STRING | (g_stayOnTop ? MF_CHECKED : MF_UNCHECKED), IDM_STAY_ON_TOP, "Stay on Top");

            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(hMenu, MF_STRING, IDM_EXIT, "Exit");

            POINT pt;
            pt.x = LOWORD(lParam);
            pt.y = HIWORD(lParam);
            // if lParam == -1 (keyboard), show at window center
            if (pt.x == -1 && pt.y == -1) {
                RECT rc; GetWindowRect(hwnd, &rc);
                pt.x = (rc.left + rc.right) / 2;
                pt.y = (rc.top + rc.bottom) / 2;
            }
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
            break;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDM_FILL_RAM: {
					EnterCriticalSection(&g_cs);
					BOOL wasFilling = g_filler.isFilling;
					if (!wasFilling) {
						LeaveCriticalSection(&g_cs);
						// Start thread
						DWORD tid;
						CreateThread(NULL, 0, FillThreadProc, NULL, 0, &tid);
					} else {
						// Stop immediately
						g_filler.stopRequested = TRUE;   // signal thread to stop
						ReleaseAllMemory();              // free all allocations immediately
						g_filler.isFilling = FALSE;      // reset filling flag so menu shows "Start"
						SetWindowTextA(hwnd, "RAM & Pagefile Monitor");
						UpdateDisplay();
						LeaveCriticalSection(&g_cs);
					}
					break;
				}



                case IDM_STAY_ON_TOP: {
                    g_stayOnTop = !g_stayOnTop;
                    if (g_stayOnTop) {
                        SetWindowPos(hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
                    } else {
                        SetWindowPos(hwnd, HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
                    }
                    break;
                }

                case IDM_EXIT:
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                    break;
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
            HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

            FillRect(hdcMem, &clientRect, g_bgBrush);

            SetBkMode(hdcMem, TRANSPARENT);
            SelectObject(hdcMem, g_font);

            MemInfo info;
            GetMemoryInfo(&info);

            char physUsed[64], physTotal[64];
            char pageUsed[64], pageTotal[64];

            FormatBytes(info.totalPhys - info.availPhys, physUsed, sizeof(physUsed));
            FormatBytes(info.totalPhys, physTotal, sizeof(physTotal));
            FormatBytes(info.totalPageFile - info.availPageFile, pageUsed, sizeof(pageUsed));
            FormatBytes(info.totalPageFile, pageTotal, sizeof(pageTotal));

            // Layout constants (relative distances requested)
            int labelX = 10 - 7; // move left labels -7 px as requested
            int ramLabelY = 8;
            int pageLabelY = 40;
            int progressOffset = 103; // progress bar offset from label left
            int progressHeight = 24;
            int textPadding = 13; // number text offset after bar

            int ramBarX = labelX + progressOffset;
            int ramBarY = ramLabelY;
            int ramBarWidth = 300; // keep original width
            int pageBarX = labelX + progressOffset;
            int pageBarY = pageLabelY;
            int pageBarWidth = 300;

			COLORREF ramColor  = ColorForPercent(info.physUsedPercent);
			COLORREF pageColor = ColorForPercent(info.pageUsedPercent);


            // Physical RAM
            SetTextColor(hdcMem, COLOR_RAM_LABEL);
            TextOutA(hdcMem, labelX, ramLabelY, "Physical RAM", 12);
            DrawProgressBar(hdcMem, ramBarX, ramBarY, ramBarWidth, progressHeight, info.physUsedPercent, ramColor);

            char ramText[128];
            snprintf(ramText, sizeof(ramText), "%s/%s GB (%.0f%%)", physUsed, physTotal, info.physUsedPercent);
            SetTextColor(hdcMem, COLOR_TEXT);
            TextOutA(hdcMem, ramBarX + ramBarWidth + textPadding, ramLabelY, ramText, lstrlenA(ramText));

            // Pagefile
            SetTextColor(hdcMem, COLOR_PAGE_LABEL);
            TextOutA(hdcMem, labelX, pageLabelY, "Pagefile", 8);
            DrawProgressBar(hdcMem, pageBarX, pageBarY, pageBarWidth, progressHeight, info.pageUsedPercent, pageColor);

            char pageText[128];
            snprintf(pageText, sizeof(pageText), "%s/%s GB (%.0f%%)", pageUsed, pageTotal, info.pageUsedPercent);
            SetTextColor(hdcMem, COLOR_TEXT);
            TextOutA(hdcMem, pageBarX + pageBarWidth + textPadding, pageLabelY, pageText, lstrlenA(pageText));

            BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, hdcMem, 0, 0, SRCCOPY);

            SelectObject(hdcMem, hbmOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hwnd, &ps);
            break;
        }

        case WM_CLOSE:
            // ensure memory released and exit
            ReleaseAllMemory();
            DestroyWindow(hwnd);
            break;

        case WM_DESTROY: 
			KillTimer(hwnd, ID_TIMER);
			if (g_bgBrush) DeleteObject(g_bgBrush);
			if (g_progressBgBrush) DeleteObject(g_progressBgBrush);
			if (g_font) DeleteObject(g_font);
			DeleteCriticalSection(&g_cs);

			if (g_hwnd) {
				RECT rc;
				GetWindowRect(g_hwnd, &rc);
				char buf[32];

				sprintf(buf, "%d", rc.left);
				WritePrivateProfileStringA("Settings", "WindowLeft", buf, g_iniPath);
				sprintf(buf, "%d", rc.top);
				WritePrivateProfileStringA("Settings", "WindowTop", buf, g_iniPath);

				WritePrivateProfileStringA("Settings", "StayOnTop", g_stayOnTop ? "1" : "0", g_iniPath);
			}

			PostQuitMessage(0);
			break;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// --- WinMain ---------------------------------------------------------------------


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEX wc = {0};
    MSG msg;

    INITCOMMONCONTROLSEX icc = {0};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COLOR_BG);
    wc.lpszClassName = "RAMMonitorClass";
    wc.lpszMenuName = NULL;
    
	wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
	wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));

    if (!RegisterClassEx(&wc)) {
        MessageBoxA(NULL, "Window Registration Failed!", "Error", MB_ICONERROR);
        return 0;
    }

    // keep your working dimensions (unchanged)
    int newWidth = 607;        // 627 total width
    int newHeight = 106;        // original chosen height
	
	// --- Prepare absolute path for INI ---
	GetModuleFileNameA(NULL, g_iniPath, MAX_PATH);
	char* p = strrchr(g_iniPath, '\\');
	if (p) *(p + 1) = 0;  // keep folder only
	strcat_s(g_iniPath, MAX_PATH, "settings.ini");

	// --- Default values ---
	g_stayOnTop = TRUE;   // default = 1
	int winLeft = 0;
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);
	//int winTop  = (screenHeight / 2); // default vertical center
	int winTop  = (screenHeight / 10); // default vertical center

	// --- Load from INI if exists ---
	g_stayOnTop = GetPrivateProfileIntA("Settings", "StayOnTop", g_stayOnTop, g_iniPath);
	winLeft = GetPrivateProfileIntA("Settings", "WindowLeft", winLeft, g_iniPath);
	winTop  = GetPrivateProfileIntA("Settings", "WindowTop", winTop, g_iniPath);



	
    g_hwnd = CreateWindowExA(WS_EX_CLIENTEDGE, "RAMMonitorClass",
        "RAM & Pagefile Monitor",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        winLeft, winTop, newWidth, newHeight,
        NULL, NULL, hInstance, NULL);


    if (g_hwnd == NULL) {
        MessageBoxA(NULL, "Window Creation Failed!", "Error", MB_ICONERROR);
        return 0;
    }
	
	SendMessage(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)));
	SendMessage(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1)));

	ShowWindow(g_hwnd, nCmdShow);
	UpdateWindow(g_hwnd);

    // ensure topmost at start if requested
    if (g_stayOnTop) {
    SetWindowPos(g_hwnd, HWND_TOPMOST, winLeft, winTop, 0, 0, SWP_NOSIZE);
	}


    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int) msg.wParam;
}
