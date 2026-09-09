// ==WindhawkMod==
// @id              desktop-watermark-tweaks
// @name            Desktop Watermark Tweaks
// @description     Tweaks the desktop watermark and refreshes Explorer immediately after settings changes
// @version         1.1.0
// @author          aubymori
// @github          https://github.com/aubymori
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Desktop Watermark Tweaks
This mod lets you tweak certain things about the desktop watermark, like whether to display it
and what to display in the build section. Changes to the settings are applied to the existing
desktop immediately, without restarting Explorer.

The mod relies on private shell32.dll symbols. After a newly released Windows build, those
symbols may not be available immediately; Windhawk may therefore be unable to load the mod
until the symbols become available. This mod currently targets x86-64 Explorer processes.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- display: true
  $name: Display watermark
  $description: Whether or not to display the desktop watermark.
- build: ""
  $name: Build string
  $description: String to display in place of build section. Leave blank for default.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <windhawk_utils.h>

struct
{
    bool                         display;
    WindhawkUtils::StringSetting build;
} settings;

bool g_unloading = false;

//public: static bool __cdecl CDesktopWatermark::s_WantWatermark(void)
bool (*CDesktopWatermark_s_WantWatermark_orig)(void);
bool CDesktopWatermark_s_WantWatermark_hook(void)
{
    if (g_unloading)
    {
        return CDesktopWatermark_s_WantWatermark_orig();
    }

    return settings.display;
}

//private: static void __cdecl CDesktopWatermark::s_GetProductBuildString(unsigned short *,unsigned int)
void (*CDesktopWatermark_s_GetProductBuildString_orig)(LPWSTR, unsigned int);
void CDesktopWatermark_s_GetProductBuildString_hook(
    LPWSTR lpszOut,
    unsigned int cchOut
)
{
    if (g_unloading)
    {
        CDesktopWatermark_s_GetProductBuildString_orig(lpszOut, cchOut);
        return;
    }

    LPCWSTR build = settings.build.get();
    if (build && *build != L'\0' && cchOut != 0)
    {
        wcsncpy_s(lpszOut, cchOut, build, _TRUNCATE);
        return;
    }
    CDesktopWatermark_s_GetProductBuildString_orig(lpszOut, cchOut);
}

// Some newer Windows builds can reach the painter even when s_WantWatermark
// returns false. Keep this hook optional so that the mod still works on builds
// where this private symbol is absent or has a different signature.
void (*CDesktopWatermark_s_DesktopBuildPaint_orig)(
    HDC, LPCRECT, HFONT, bool);
void CDesktopWatermark_s_DesktopBuildPaint_hook(
    HDC hdc,
    LPCRECT lprc,
    HFONT hFont,
    bool fDrawVersionAlways
)
{
    if (g_unloading || settings.display)
    {
        CDesktopWatermark_s_DesktopBuildPaint_orig(
            hdc, lprc, hFont, fDrawVersionAlways);
    }
}

BOOL CALLBACK RefreshDesktopWindow(HWND hwnd, LPARAM)
{
    wchar_t className[64] = {};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) == 0)
    {
        return TRUE;
    }

    if (wcscmp(className, L"Progman") == 0 ||
        wcscmp(className, L"WorkerW") == 0 ||
        wcscmp(className, L"SHELLDLL_DefView") == 0)
    {
        RedrawWindow(
            hwnd,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    EnumChildWindows(hwnd, RefreshDesktopWindow, 0);
    return TRUE;
}

void RefreshDesktop(void)
{
    EnumWindows(RefreshDesktopWindow, 0);
}

#define LoadIntSetting(NAME)    settings.NAME = Wh_GetIntSetting(L ## #NAME)
#define LoadStringSetting(NAME) settings.NAME = WindhawkUtils::StringSetting::make(L ## #NAME)

void LoadSettings(void)
{
    LoadIntSetting(display);
    LoadStringSetting(build);
}

const WindhawkUtils::SYMBOL_HOOK hooks[] = {
    {
        {
            L"public: static bool __cdecl CDesktopWatermark::s_WantWatermark(void)"
        },
        &CDesktopWatermark_s_WantWatermark_orig,
        CDesktopWatermark_s_WantWatermark_hook,
        false
    },
    {
        {
            L"private: static void __cdecl CDesktopWatermark::s_GetProductBuildString(unsigned short *,unsigned int)"
        },
        &CDesktopWatermark_s_GetProductBuildString_orig,
        CDesktopWatermark_s_GetProductBuildString_hook,
        false
    },
    {
        {
            L"private: static void __cdecl CDesktopWatermark::s_DesktopBuildPaint(struct HDC__ *,struct tagRECT const *,struct HFONT__ *,bool)",
            L"private: static void __cdecl CDesktopWatermark::s_DesktopBuildPaint(struct HDC__ *,struct tagRECT const *,struct HFONT__ *)"
        },
        &CDesktopWatermark_s_DesktopBuildPaint_orig,
        CDesktopWatermark_s_DesktopBuildPaint_hook,
        true
    }
};

BOOL Wh_ModInit(void)
{
    LoadSettings();

    HMODULE hShell32 = LoadLibraryW(L"shell32.dll");
    if (!hShell32)
    {
        Wh_Log(L"Failed to load shell32.dll");
        return FALSE;
    }

    if (!WindhawkUtils::HookSymbols(
        hShell32,
        hooks,
        ARRAYSIZE(hooks)
    ))
    {
        Wh_Log(L"Failed to hook one or more symbol functions in shell32.dll");
        return FALSE;
    }

    RefreshDesktop();
    return TRUE;
}

void Wh_ModSettingsChanged(void)
{
    LoadSettings();
    RefreshDesktop();
}

void Wh_ModAfterInit(void)
{
    RefreshDesktop();
}

void Wh_ModBeforeUninit(void)
{
    g_unloading = true;
    RefreshDesktop();
}
