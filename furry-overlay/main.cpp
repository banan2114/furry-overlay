#include <windows.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include "json.hpp"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;
using json = nlohmann::json;

// Идентификаторы
#define WM_TRAYICON (WM_USER + 1)
#define IDM_EXIT 1001
#define IDM_RELOAD 1002
#define IDM_CLICKTHROUGH 1003
#define IDM_OPACITY_START 3000
#define IDM_SCALE_START 4000
#define HOTKEY_TOGGLE_CLICK 1
#define HOTKEY_RELOAD 2

// ID иконки в ресурсах (должен совпадать с .rc файлом)
#define IDI_APPICON 101

// Глобальные переменные
ULONG_PTR gdiplusToken;
HWND g_hwnd = nullptr;
Image* g_image = nullptr;
NOTIFYICONDATA nid = {};
HICON g_hIcon = nullptr;  // Глобальная иконка
std::wstring g_imagePath = L"image.png";

struct Config {
    int x = 100, y = 100;
    float scale = 1.0f;
    int opacity = 255;
    bool click_through = true;
} cfg;

// --- ФУНКЦИИ ЛОГИКИ ---

void SaveConfig() {
    json j = {
        {"x", cfg.x},
        {"y", cfg.y},
        {"scale", cfg.scale},
        {"opacity", cfg.opacity},
        {"click_through", cfg.click_through}
    };
    std::ofstream f("config.json");
    if (f.is_open()) {
        f << j.dump(4);
        f.close();
    }
}

void LoadConfig() {
    std::ifstream f("config.json");
    if (!f.good()) return;
    try {
        json j;
        f >> j;
        cfg.x = j.value("x", 100);
        cfg.y = j.value("y", 100);
        cfg.scale = j.value("scale", 1.0f);
        cfg.opacity = j.value("opacity", 255);
        cfg.click_through = j.value("click_through", true);
    }
    catch (...) {}
    f.close();
}

void ApplyClickThrough() {
    if (!g_hwnd) return;
    LONG ex = GetWindowLong(g_hwnd, GWL_EXSTYLE);
    if (cfg.click_through)
        SetWindowLong(g_hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    else
        SetWindowLong(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
}

void UpdateOverlay() {
    if (!g_hwnd) return;
    if (!g_image || g_image->GetLastStatus() != Ok) return;

    int w = static_cast<int>(g_image->GetWidth() * cfg.scale);
    int h = static_cast<int>(g_image->GetHeight() * cfg.scale);
    if (w <= 0 || h <= 0) return;

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return;

    HDC memDC = CreateCompatibleDC(screenDC);
    if (!memDC) {
        ReleaseDC(nullptr, screenDC);
        return;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);

    if (!hBitmap) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }

    HBITMAP hOldBitmap = static_cast<HBITMAP>(SelectObject(memDC, hBitmap));

    Graphics g(memDC);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.Clear(Color(0, 0, 0, 0));
    g.DrawImage(g_image, 0, 0, w, h);

    POINT pos = { cfg.x, cfg.y };
    SIZE sizeWnd = { w, h };
    POINT src = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, static_cast<BYTE>(cfg.opacity), AC_SRC_ALPHA };

    UpdateLayeredWindow(g_hwnd, screenDC, &pos, &sizeWnd, memDC, &src, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

bool LoadImage() {
    if (g_image) {
        delete g_image;
        g_image = nullptr;
    }

    g_image = new Image(g_imagePath.c_str());

    if (!g_image || g_image->GetLastStatus() != Ok) {
        if (g_image) {
            delete g_image;
            g_image = nullptr;
        }
        return false;
    }
    return true;
}

// --- МЕНЮ ТРЕЯ ---

void ShowTrayMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();

    // Подменю: Scale
    HMENU hScale = CreatePopupMenu();
    float scales[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
    for (int i = 0; i < 6; i++) {
        std::wstring lbl = std::to_wstring(static_cast<int>(scales[i] * 100)) + L"%";
        UINT flags = MF_STRING | ((std::abs(cfg.scale - scales[i]) < 0.01f) ? MF_CHECKED : 0);
        AppendMenuW(hScale, flags, IDM_SCALE_START + i, lbl.c_str());
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hScale), L"Scale");

    // Подменю: Opacity
    HMENU hOpacity = CreatePopupMenu();
    int ops[] = { 50, 125, 200, 255 };
    const wchar_t* opLabels[] = { L"20%", L"50%", L"75%", L"100%" };
    for (int i = 0; i < 4; i++) {
        UINT flags = MF_STRING | ((cfg.opacity == ops[i]) ? MF_CHECKED : 0);
        AppendMenuW(hOpacity, flags, IDM_OPACITY_START + i, opLabels[i]);
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hOpacity), L"Opacity");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (cfg.click_through ? MF_CHECKED : 0), IDM_CLICKTHROUGH, L"Lock (Click-Through) \tAlt+T");
    AppendMenuW(hMenu, MF_STRING, IDM_RELOAD, L"Reload Image && Config \tAlt+R");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}

// --- ОБРАБОТКА ---

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (!cfg.click_through) {
            ReleaseCapture();
            SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        break;

    case WM_EXITSIZEMOVE: {
        RECT r;
        GetWindowRect(hWnd, &r);
        cfg.x = r.left;
        cfg.y = r.top;
        SaveConfig();
        break;
    }

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hWnd);
        }
        break;

    case WM_HOTKEY:
        if (wParam == HOTKEY_TOGGLE_CLICK) {
            cfg.click_through = !cfg.click_through;
            ApplyClickThrough();
            SaveConfig();
            UpdateOverlay();
        }
        else if (wParam == HOTKEY_RELOAD) {
            LoadConfig();
            if (!LoadImage()) {
                MessageBoxW(hWnd, L"Не удалось загрузить image.png", L"Ошибка", MB_ICONWARNING);
            }
            ApplyClickThrough();
            UpdateOverlay();
        }
        break;

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDM_EXIT) {
            DestroyWindow(hWnd);
            return 0;
        }

        if (id >= IDM_SCALE_START && id < IDM_SCALE_START + 6) {
            float scales[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
            cfg.scale = scales[id - IDM_SCALE_START];
        }
        else if (id >= IDM_OPACITY_START && id < IDM_OPACITY_START + 4) {
            int ops[] = { 50, 125, 200, 255 };
            cfg.opacity = ops[id - IDM_OPACITY_START];
        }
        else if (id == IDM_CLICKTHROUGH) {
            cfg.click_through = !cfg.click_through;
            ApplyClickThrough();
        }
        else if (id == IDM_RELOAD) {
            LoadConfig();
            if (!LoadImage()) {
                MessageBoxW(hWnd, L"Не удалось загрузить image.png", L"Ошибка", MB_ICONWARNING);
            }
            ApplyClickThrough();
        }

        SaveConfig();
        UpdateOverlay();
        break;
    }

    case WM_DESTROY:
        UnregisterHotKey(hWnd, HOTKEY_TOGGLE_CLICK);
        UnregisterHotKey(hWnd, HOTKEY_RELOAD);
        Shell_NotifyIcon(NIM_DELETE, &nid);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Инициализация GDI+
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&gdiplusToken, &gsi, nullptr) != Ok) {
        MessageBoxW(nullptr, L"Не удалось инициализировать GDI+", L"Ошибка", MB_ICONERROR);
        return 1;
    }

    LoadConfig();

    if (!LoadImage()) {
        MessageBoxW(nullptr, L"Не удалось загрузить image.png", L"Ошибка", MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    // Загрузка иконки
    g_hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APPICON));

    // Регистрация класса окна
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"OverlayWindowClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_hIcon;  // Иконка для процесса/диспетчера задач

    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Не удалось зарегистрировать класс окна", L"Ошибка", MB_ICONERROR);
        delete g_image;
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (cfg.click_through) {
        exStyle |= WS_EX_TRANSPARENT;
    }

    g_hwnd = CreateWindowExW(
        exStyle,
        wc.lpszClassName,
        L"Overlay",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInst, nullptr
    );

    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Не удалось создать окно", L"Ошибка", MB_ICONERROR);
        delete g_image;
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    // Настройка иконки в трее
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_hIcon;  // Используем нашу иконку

    wcscpy_s(nid.szTip, _countof(nid.szTip), L"Overlay Settings");
    Shell_NotifyIcon(NIM_ADD, &nid);

    // Регистрация горячих клавиш
    RegisterHotKey(g_hwnd, HOTKEY_TOGGLE_CLICK, MOD_ALT, 'T');
    RegisterHotKey(g_hwnd, HOTKEY_RELOAD, MOD_ALT, 'R');

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateOverlay();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_image) {
        delete g_image;
        g_image = nullptr;
    }

    // Освобождаем иконку если загружена из файла
    if (g_hIcon) {
        DestroyIcon(g_hIcon);
    }

    GdiplusShutdown(gdiplusToken);

    return static_cast<int>(msg.wParam);
}