#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <wininet.h>
#include <string>
#include <vector>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <fstream>
#include "json.hpp"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wininet.lib")

using namespace Gdiplus;
using json = nlohmann::json;

#define WM_TRAYICON (WM_USER + 1)
#define WM_DOWNLOAD_COMPLETE (WM_USER + 2)
#define IDM_EXIT 1001
#define IDM_RELOAD 1002
#define IDM_CLICKTHROUGH 1003
#define IDM_ENABLE_E621 1004
#define IDM_DOWNLOAD_NOW 1005
#define IDM_OPACITY_START 3000
#define IDM_SCALE_START 4000
#define IDM_INTERVAL_START 5000
#define HOTKEY_TOGGLE_CLICK 1
#define HOTKEY_RELOAD 2
#define TIMER_DOWNLOAD 1
#define IDI_APPICON 101

ULONG_PTR gdiplusToken;
HWND g_hwnd = nullptr;
Image* g_image = nullptr;
NOTIFYICONDATA nid = {};
HICON g_hIcon = nullptr;
std::atomic<bool> g_downloading(false);

// Буфер для хранения изображения в памяти
std::vector<BYTE> g_imageBuffer;
std::mutex g_bufferMutex;

struct Config {
    int x = 100, y = 100;
    float scale = 1.0f;
    int opacity = 255;
    bool click_through = true;
    bool e621_enabled = false;
    std::string e621_tags = "rating:safe";
    int download_interval = 300;
} cfg;

// --- HTTP: Скачивание в память ---

std::vector<BYTE> DownloadToMemory(const std::string& url) {
    std::vector<BYTE> result;

    HINTERNET hInternet = InternetOpenA("E621Downloader/1.0",
        INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!hInternet) return result;

    HINTERNET hConnect = InternetOpenUrlA(hInternet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);

    if (hConnect) {
        BYTE buffer[8192];
        DWORD bytesRead;
        while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            result.insert(result.end(), buffer, buffer + bytesRead);
        }
        InternetCloseHandle(hConnect);
    }

    InternetCloseHandle(hInternet);
    return result;
}

std::string DownloadString(const std::string& url) {
    auto data = DownloadToMemory(url);
    return std::string(data.begin(), data.end());
}

// --- Создание Image из буфера в памяти ---

Image* CreateImageFromMemory(const std::vector<BYTE>& buffer) {
    if (buffer.empty()) return nullptr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, buffer.size());
    if (!hMem) return nullptr;

    void* pMem = GlobalLock(hMem);
    if (!pMem) {
        GlobalFree(hMem);
        return nullptr;
    }

    memcpy(pMem, buffer.data(), buffer.size());
    GlobalUnlock(hMem);

    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) != S_OK) {
        GlobalFree(hMem);
        return nullptr;
    }

    Image* img = new Image(pStream);
    pStream->Release();

    if (!img || img->GetLastStatus() != Ok) {
        delete img;
        return nullptr;
    }

    return img;
}

// --- E621 ---

std::string UrlEncode(const std::string& str) {
    std::ostringstream escaped;
    escaped << std::uppercase << std::hex << std::setfill('0');

    for (unsigned char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') {
            escaped << static_cast<char>(c);
        }
        else if (c == ' ') {
            escaped << '+';
        }
        else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

// Скачивает изображение с E621 напрямую в память
bool DownloadRandomE621ImageToMemory(std::vector<BYTE>& outBuffer) {
    try {
        std::string tags = cfg.e621_tags;

        if (tags.find("order:") == std::string::npos) {
            tags += " order:random";
        }
        if (tags.find("filetype:") == std::string::npos && tags.find("type:") == std::string::npos) {
            tags += " filetype:png";
        }

        std::string encodedTags = UrlEncode(tags);
        std::stringstream ss;
        ss << "https://e621.net/posts.json?tags=" << encodedTags << "&limit=1";

        std::string jsonData = DownloadString(ss.str());
        if (jsonData.empty()) return false;

        json response = json::parse(jsonData);

        if (!response.contains("posts") || !response["posts"].is_array() || response["posts"].empty()) {
            return false;
        }

        auto& post = response["posts"][0];
        if (!post.contains("file") || !post["file"].is_object()) return false;

        auto& file = post["file"];
        if (!file.contains("url") || file["url"].is_null()) return false;
        if (!file.contains("ext") || file["ext"].is_null()) return false;

        std::string imageUrl = file["url"].get<std::string>();
        std::string ext = file["ext"].get<std::string>();

        if (imageUrl.empty()) return false;
        if (ext != "png" && ext != "jpg" && ext != "jpeg") return false;

        // Скачиваем изображение напрямую в память
        outBuffer = DownloadToMemory(imageUrl);
        return !outBuffer.empty();

    }
    catch (...) {
        return false;
    }
}

// --- Конфиг ---

void SaveConfig() {
    json j = {
        {"x", cfg.x}, {"y", cfg.y},
        {"scale", cfg.scale}, {"opacity", cfg.opacity},
        {"click_through", cfg.click_through},
        {"e621_enabled", cfg.e621_enabled},
        {"e621_tags", cfg.e621_tags},
        {"download_interval", cfg.download_interval}
    };
    std::ofstream f("config.json");
    if (f.is_open()) { f << j.dump(4); f.close(); }
}

void LoadConfig() {
    std::ifstream f("config.json");
    if (!f.is_open()) return;

    try {
        json j; f >> j; f.close();
        cfg.x = j.value("x", 100);
        cfg.y = j.value("y", 100);
        cfg.scale = j.value("scale", 1.0f);
        cfg.opacity = j.value("opacity", 255);
        cfg.click_through = j.value("click_through", true);
        cfg.e621_enabled = j.value("e621_enabled", false);
        cfg.e621_tags = j.value("e621_tags", "rating:safe");
        cfg.download_interval = j.value("download_interval", 300);

        cfg.x = std::clamp(cfg.x, -10000, 10000);
        cfg.y = std::clamp(cfg.y, -10000, 10000);
        cfg.scale = std::clamp(cfg.scale, 0.1f, 10.0f);
        cfg.opacity = std::clamp(cfg.opacity, 0, 255);
        cfg.download_interval = std::max(cfg.download_interval, 10);
        if (cfg.e621_tags.empty()) cfg.e621_tags = "rating:safe";
    }
    catch (...) { f.close(); }
}

void ApplyClickThrough() {
    if (!g_hwnd) return;
    LONG_PTR ex = GetWindowLongPtr(g_hwnd, GWL_EXSTYLE);
    if (cfg.click_through)
        SetWindowLongPtr(g_hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    else
        SetWindowLongPtr(g_hwnd, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
}

void UpdateOverlay() {
    if (!g_hwnd || !g_image || g_image->GetLastStatus() != Ok) return;

    int w = static_cast<int>(g_image->GetWidth() * cfg.scale);
    int h = static_cast<int>(g_image->GetHeight() * cfg.scale);
    if (w <= 0 || h <= 0) return;

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return;

    HDC memDC = CreateCompatibleDC(screenDC);
    if (!memDC) { ReleaseDC(nullptr, screenDC); return; }

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

// Загрузка изображения из внутреннего буфера
bool LoadImageFromBuffer() {
    std::lock_guard<std::mutex> lock(g_bufferMutex);

    if (g_imageBuffer.empty()) return false;

    Image* newImage = CreateImageFromMemory(g_imageBuffer);
    if (!newImage) return false;

    if (g_image) delete g_image;
    g_image = newImage;

    return true;
}

// Загрузка изображения из файла (для начального запуска)
bool LoadImageFromFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    std::streamsize size = file.tellg();
    if (size <= 0) { file.close(); return false; }

    file.seekg(0, std::ios::beg);

    std::vector<BYTE> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        file.close();
        return false;
    }
    file.close();

    Image* newImage = CreateImageFromMemory(buffer);
    if (!newImage) return false;

    if (g_image) delete g_image;
    g_image = newImage;

    // Сохраняем в глобальный буфер
    std::lock_guard<std::mutex> lock(g_bufferMutex);
    g_imageBuffer = std::move(buffer);

    return true;
}

void StartDownloadTimer() {
    if (cfg.e621_enabled && cfg.download_interval > 0) {
        SetTimer(g_hwnd, TIMER_DOWNLOAD, cfg.download_interval * 1000, nullptr);
    }
    else {
        KillTimer(g_hwnd, TIMER_DOWNLOAD);
    }
}

void DownloadInBackground() {
    if (g_downloading.exchange(true)) return;

    std::thread([]() {
        std::vector<BYTE> newBuffer;
        bool success = DownloadRandomE621ImageToMemory(newBuffer);

        if (success && !newBuffer.empty()) {
            std::lock_guard<std::mutex> lock(g_bufferMutex);
            g_imageBuffer = std::move(newBuffer);
        }

        g_downloading = false;

        if (success && g_hwnd) {
            PostMessage(g_hwnd, WM_DOWNLOAD_COMPLETE, 1, 0);
        }
        }).detach();
}

void ShowTrayMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING | (cfg.e621_enabled ? MF_CHECKED : 0), IDM_ENABLE_E621, L"Enable E621 Auto-Download");
    AppendMenuW(hMenu, MF_STRING | (!cfg.e621_enabled ? MF_GRAYED : 0), IDM_DOWNLOAD_NOW, L"Download Now");

    HMENU hInterval = CreatePopupMenu();
    int intervals[] = { 30, 60, 300, 600, 1800, 3600 };
    const wchar_t* intervalLabels[] = { L"30 sec", L"1 min", L"5 min", L"10 min", L"30 min", L"1 hour" };
    for (int i = 0; i < 6; i++) {
        UINT flags = MF_STRING | ((cfg.download_interval == intervals[i]) ? MF_CHECKED : 0);
        AppendMenuW(hInterval, flags, IDM_INTERVAL_START + i, intervalLabels[i]);
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hInterval), L"Download Interval");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    HMENU hScale = CreatePopupMenu();
    float scales[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
    for (int i = 0; i < 6; i++) {
        std::wstring lbl = std::to_wstring(static_cast<int>(scales[i] * 100)) + L"%";
        UINT flags = MF_STRING | ((std::abs(cfg.scale - scales[i]) < 0.01f) ? MF_CHECKED : 0);
        AppendMenuW(hScale, flags, IDM_SCALE_START + i, lbl.c_str());
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hScale), L"Scale");

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
    AppendMenuW(hMenu, MF_STRING, IDM_RELOAD, L"Reload Config \tAlt+R");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}

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

    case WM_TIMER:
        if (wParam == TIMER_DOWNLOAD && cfg.e621_enabled) {
            DownloadInBackground();
        }
        break;

    case WM_DOWNLOAD_COMPLETE:
        if (wParam == 1) {
            if (LoadImageFromBuffer()) {
                UpdateOverlay();
            }
        }
        break;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) ShowTrayMenu(hWnd);
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
            ApplyClickThrough();
            StartDownloadTimer();
            UpdateOverlay();
        }
        break;

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDM_EXIT) { DestroyWindow(hWnd); return 0; }

        if (id == IDM_ENABLE_E621) {
            cfg.e621_enabled = !cfg.e621_enabled;
            StartDownloadTimer();
        }
        else if (id == IDM_DOWNLOAD_NOW) {
            if (cfg.e621_enabled) DownloadInBackground();
        }
        else if (id >= IDM_INTERVAL_START && id < IDM_INTERVAL_START + 6) {
            int intervals[] = { 30, 60, 300, 600, 1800, 3600 };
            cfg.download_interval = intervals[id - IDM_INTERVAL_START];
            StartDownloadTimer();
        }
        else if (id >= IDM_SCALE_START && id < IDM_SCALE_START + 6) {
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
            ApplyClickThrough();
            StartDownloadTimer();
        }

        SaveConfig();
        UpdateOverlay();
        break;
    }

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_DOWNLOAD);
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
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&gdiplusToken, &gsi, nullptr) != Ok) {
        MessageBoxW(nullptr, L"GDI+ init failed", L"Error", MB_ICONERROR);
        return 1;
    }

    LoadConfig();

    // Попытка загрузить начальное изображение из файла
    if (!LoadImageFromFile(L"image.png")) {
        // Если нет файла - не критично, подождём скачивания
    }

    g_hIcon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!g_hIcon) g_hIcon = LoadIcon(nullptr, IDI_APPLICATION);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"OverlayWindowClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_hIcon;

    if (!RegisterClassW(&wc)) {
        delete g_image;
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (cfg.click_through) exStyle |= WS_EX_TRANSPARENT;

    g_hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"Overlay", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);

    if (!g_hwnd) {
        delete g_image;
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_hIcon;
    wcscpy_s(nid.szTip, L"E621 Overlay");
    Shell_NotifyIcon(NIM_ADD, &nid);

    RegisterHotKey(g_hwnd, HOTKEY_TOGGLE_CLICK, MOD_ALT, 'T');
    RegisterHotKey(g_hwnd, HOTKEY_RELOAD, MOD_ALT, 'R');

    StartDownloadTimer();
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateOverlay();

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    delete g_image;
    GdiplusShutdown(gdiplusToken);
    return static_cast<int>(msg.wParam);
}