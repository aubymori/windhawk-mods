// ==WindhawkMod==
// @id              transparent-screensaver-fix
// @name            Transparent Screensaver Fix
// @description     Allows transparent screensavers to see your desktop again, live
// @version         1.1.0
// @author          aubymori
// @github          https://github.com/aubymori
// @include         winlogon.exe
// @include         Bubbles.scr
// @architecture    x86-64
// @compilerOptions -lgdi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Transparent Screensaver Fix
This mod allows transparent screensavers, such as the Bubbles screensaver, to see
your desktop again, rather than displaying a solid color.

With **Live desktop feed** enabled, the Bubbles screensaver keeps refreshing the
desktop behind the bubbles instead of showing a snapshot taken when it started.
This requires Windows 10 version 2004 or later. The small preview in the Screen
Saver Settings dialog stays static.

# IMPORTANT: READ!
Windhawk needs to hook into `winlogon.exe` for this mod to work. Please navigate
to Windhawk's Settings, Advanced settings, More advanced settings, and make sure
that `winlogon.exe` is in the Process inclusion list.

**Before**:

![Before](https://raw.githubusercontent.com/aubymori/images/main/transparent-screensaver-fix-before.png)

**After**:

![After](https://raw.githubusercontent.com/aubymori/images/main/transparent-screensaver-fix-after.png)
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- liveFeed: true
  $name: Live desktop feed
  $description: Continuously refresh the desktop behind the Bubbles screensaver instead of using a static snapshot
- fps: 24
  $name: Live feed frame rate
  $description: How many times per second the desktop is captured (1-120)
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

/*
 * winlogon.exe: run the screensaver on the user desktop
 */

bool g_bStartingScreenSaver = false;

/* Only prevent desktop switch when screensaver is being started */
DWORD (*RunScreenSaver_orig)(struct _WLSM_GLOBAL_CONTEXT *, struct SCREEN_SAVER_DATA *, struct _WINLOGON_JOB **);
DWORD RunScreenSaver_hook(
    struct _WLSM_GLOBAL_CONTEXT *pContext,
    struct SCREEN_SAVER_DATA    *pData,
    struct _WINLOGON_JOB        **ppJob
)
{
    g_bStartingScreenSaver = true;
    DWORD result = RunScreenSaver_orig(
        pContext, pData, ppJob
    );
    g_bStartingScreenSaver = false;
    return result;
}

/* Prevent screensaver from switching desktop */
void (*CSession_SwitchDesktop_orig)(void *, UINT, int *, DWORD, DWORD);
void CSession_SwitchDesktop_hook(
    void  *pThis,
    UINT   a1,
    int   *a2,
    DWORD  a3,
    DWORD  a4
)
{
    if (!g_bStartingScreenSaver)
        return CSession_SwitchDesktop_orig(pThis, a1, a2, a3, a4);
}

/* Create screensaver on the user desktop */
DWORD (*CUser_CreateProcessW_orig)(void *, LPCWSTR, LPWSTR, int, STARTUPINFOW *, PROCESS_INFORMATION *, int);
DWORD CUser_CreateProcessW_hook(
    void                *pThis,
    LPCWSTR              a2,
    LPWSTR               a3,
    int                  a4,
    STARTUPINFOW        *psi,
    PROCESS_INFORMATION *a6,
    int                  a7
)
{
    if (psi && psi->lpDesktop && psi->lpDesktop[0]
    && 0 == wcsicmp(psi->lpDesktop, L"Winsta0\\Screen-saver"))
    {
        psi->lpDesktop = nullptr;
    }
    return CUser_CreateProcessW_orig(
        pThis, a2, a3, a4, psi, a6, a7
    );
}

const WindhawkUtils::SYMBOL_HOOK winlogonExeHooks[] = {
    {
        {
            L"unsigned long __cdecl RunScreenSaver(struct _WLSM_GLOBAL_CONTEXT *,struct SCREEN_SAVER_DATA *,struct _WINLOGON_JOB * *)"
        },
        &RunScreenSaver_orig,
        RunScreenSaver_hook,
        false
    },
    {
        {
            L"public: void __cdecl CSession::SwitchDesktop(enum _DESKTOPID,int *,unsigned long,unsigned long)"
        },
        &CSession_SwitchDesktop_orig,
        CSession_SwitchDesktop_hook,
        false
    },
    {
        {
            L"public: unsigned long __cdecl CUser::CreateProcessW(unsigned short const *,unsigned short *,unsigned long,struct _STARTUPINFOW *,struct _PROCESS_INFORMATION *,int)"
        },
        &CUser_CreateProcessW_orig,
        CUser_CreateProcessW_hook,
        false
    }
};

/*
 * Bubbles.scr: live desktop feed
 *
 * At startup, MonitorEnumProc BitBlts each monitor into an in-memory TGA, which
 * RestoreDeviceObjects turns into the texture drawn behind (and refracted by)
 * the bubbles. We remember which texture belongs to which monitor, capture the
 * desktop on a worker thread, and upload the newest frame before each render.
 * The screensaver windows are excluded from capture so they don't film themselves.
 */

struct MonitorSnapshot
{
    RECT        rc;
    const BYTE *tgaData;                      // Bubbles' own snapshot of this monitor
};

/*
 * Screen BitBlts mostly wait on DWM, so all monitors are captured with a single
 * BitBlt of their bounding rect instead of one per monitor.
 */
struct DesktopCapture
{
    RECT               rc   = {};             // Bounding rect of all monitors, guarded by lock
    HDC                hdc  = nullptr;        // Top-down 32bpp DIB, owned by the capture thread
    HBITMAP            hbm  = nullptr;
    const BYTE        *bits = nullptr;
    SRWLOCK            lock = SRWLOCK_INIT;   // Shared while reading, exclusive to (re)allocate

    // Single-buffer handoff: a frame is only captured once the previous one has
    // been uploaded, so the two threads never touch the pixels at the same time,
    // and nothing is captured while nobody renders.
    std::atomic<ULONG> seq{0};                // Last frame captured
    std::atomic<ULONG> consumed{0};           // Last frame uploaded
};

struct LiveTexture
{
    UINT               renderUnit;
    RECT               rc;                    // Monitor shown by this texture
    IDirect3DDevice9  *pDevice;
    IDirect3DTexture9 *pTexture;              // Owned by Bubbles
    IDirect3DTexture9 *pStaging;              // Ours, D3DPOOL_SYSTEMMEM
};

bool g_bLiveFeed;
int  g_fps;

// Appended on the render thread, read by the capture thread
SRWLOCK                      g_monitorsLock = SRWLOCK_INIT;
std::vector<MonitorSnapshot> g_monitors;

DesktopCapture g_capture;

// Render thread only
std::vector<LiveTexture> g_liveTextures;
UINT                     g_currentRenderUnit = 0;
const RECT              *g_pEnumMonitorRect = nullptr;
bool                     g_bExcludedFromCapture = false;

HANDLE g_hCaptureThread = nullptr;
HANDLE g_hStopEvent = nullptr;
HANDLE g_hUploadedEvent = nullptr;  // Signaled by the render thread, kept for the process lifetime

LONG Width(const RECT &rc)  { return rc.right - rc.left; }
LONG Height(const RECT &rc) { return rc.bottom - rc.top; }

/* Caller holds g_capture.lock exclusively */
void FreeCaptureBuffer(void)
{
    if (g_capture.hdc)
        DeleteDC(g_capture.hdc);  // Deselects the bitmap so it can be deleted
    if (g_capture.hbm)
        DeleteObject(g_capture.hbm);
    g_capture.hdc = nullptr;
    g_capture.hbm = nullptr;
    g_capture.bits = nullptr;
    g_capture.rc = {};
}

/* (Re)allocates the capture buffer when the monitor layout changed */
bool EnsureCaptureBuffer(void)
{
    RECT rc = {};
    AcquireSRWLockShared(&g_monitorsLock);
    for (const auto &monitor : g_monitors)
        UnionRect(&rc, &rc, &monitor.rc);
    ReleaseSRWLockShared(&g_monitorsLock);

    if (g_capture.hdc && EqualRect(&rc, &g_capture.rc))
        return true;
    if (IsRectEmpty(&rc))
        return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = Width(rc);
    bmi.bmiHeader.biHeight = -Height(rc);  // Top-down, like the textures
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    AcquireSRWLockExclusive(&g_capture.lock);
    FreeCaptureBuffer();
    void *pBits;
    g_capture.hdc = CreateCompatibleDC(nullptr);
    g_capture.hbm = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    bool ok = g_capture.hdc && g_capture.hbm;
    if (ok)
    {
        SelectObject(g_capture.hdc, g_capture.hbm);
        g_capture.bits = (const BYTE *)pBits;
        g_capture.rc = rc;
    }
    else
    {
        FreeCaptureBuffer();
    }
    ReleaseSRWLockExclusive(&g_capture.lock);
    return ok;
}

/* Periodic timer; high resolution avoids the ~16 ms jitter of the default timer */
HANDLE CreateFrameTimer(LONG periodMs)
{
    LARGE_INTEGER due;
    due.QuadPart = -1;
    for (DWORD flags : { (DWORD)CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, 0UL })
    {
        HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr, flags, TIMER_ALL_ACCESS);
        if (hTimer && SetWaitableTimer(hTimer, &due, periodMs, nullptr, nullptr, FALSE))
            return hTimer;
        if (hTimer)
            CloseHandle(hTimer);
    }
    return nullptr;
}

DWORD WINAPI CaptureThreadProc(LPVOID)
{
    HANDLE hTimer = CreateFrameTimer(1000 / g_fps);
    if (!hTimer)
        return 1;

    HANDLE tickHandles[] = { g_hStopEvent, hTimer };
    HANDLE uploadHandles[] = { g_hStopEvent, g_hUploadedEvent };
    HDC hdcScreen = nullptr;
    while (WaitForMultipleObjects(ARRAYSIZE(tickHandles), tickHandles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        // Wait for the previous frame to be uploaded instead of dropping the tick. Looping
        // because the event can still be signaled from an earlier upload.
        bool stop = false;
        while (!stop && g_capture.seq != g_capture.consumed)
            stop = WaitForMultipleObjects(ARRAYSIZE(uploadHandles), uploadHandles, FALSE, INFINITE) != WAIT_OBJECT_0 + 1;
        if (stop)
            break;

        if (!hdcScreen && !(hdcScreen = GetDC(nullptr)))
            continue;
        if (!EnsureCaptureBuffer())
            continue;

        // Only this thread changes the buffer, so it can be written without the lock
        const RECT &rc = g_capture.rc;
        if (!BitBlt(
            g_capture.hdc, 0, 0, Width(rc), Height(rc),
            hdcScreen, rc.left, rc.top, SRCCOPY | CAPTUREBLT))
        {
            // The screen DC may have gone stale, get a fresh one next tick
            ReleaseDC(nullptr, hdcScreen);
            hdcScreen = nullptr;
            continue;
        }
        GdiFlush();
        g_capture.seq++;
    }

    if (hdcScreen)
        ReleaseDC(nullptr, hdcScreen);
    CloseHandle(hTimer);

    AcquireSRWLockExclusive(&g_capture.lock);
    FreeCaptureBuffer();
    ReleaseSRWLockExclusive(&g_capture.lock);
    return 0;
}

void StartCaptureThread(void)
{
    if (g_hCaptureThread)
        return;
    if (!g_hStopEvent)
        g_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hUploadedEvent)
        g_hUploadedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_hStopEvent && g_hUploadedEvent)
        g_hCaptureThread = CreateThread(nullptr, 0, CaptureThreadProc, nullptr, 0, nullptr);
}

void StopCaptureThread(void)
{
    if (g_hCaptureThread)
    {
        SetEvent(g_hStopEvent);
        WaitForSingleObject(g_hCaptureThread, INFINITE);
        CloseHandle(g_hCaptureThread);
        g_hCaptureThread = nullptr;
    }
    if (g_hStopEvent)
    {
        CloseHandle(g_hStopEvent);
        g_hStopEvent = nullptr;
    }
}

/* Remove tracked textures and free their staging copies */
template <typename Pred>
void DropLiveTextures(Pred pred)
{
    std::erase_if(g_liveTextures, [&](LiveTexture &lt)
    {
        if (!pred(lt))
            return false;
        if (lt.pStaging)
            lt.pStaging->Release();
        return true;
    });
}

/* Copy one monitor's part of the captured frame into its Bubbles texture; caller holds g_capture.lock shared */
void UploadLiveTexture(LiveTexture &lt)
{
    RECT rcInCapture;
    D3DSURFACE_DESC desc;
    if (!g_capture.bits
    || !IntersectRect(&rcInCapture, &lt.rc, &g_capture.rc) || !EqualRect(&rcInCapture, &lt.rc)
    || FAILED(lt.pTexture->GetLevelDesc(0, &desc)))
        return;

    if (!lt.pStaging)
    {
        IDirect3DTexture9 *pStaging;
        if (FAILED(lt.pDevice->CreateTexture(
            desc.Width, desc.Height, 1, 0, desc.Format, D3DPOOL_SYSTEMMEM, &pStaging, nullptr)))
            return;
        lt.pStaging = pStaging;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(lt.pStaging->LockRect(0, &lr, nullptr, D3DLOCK_NOSYSLOCK)))
        return;

    // Same BGRA layout as the TGA Bubbles builds, straight from BitBlt
    SIZE_T srcPitch = (SIZE_T)Width(g_capture.rc) * 4;
    SIZE_T rowBytes = (std::min)((SIZE_T)Width(lt.rc), (SIZE_T)desc.Width) * 4;
    UINT   rows     = (std::min)((UINT)Height(lt.rc), desc.Height);
    const BYTE *src = g_capture.bits
        + (SIZE_T)(lt.rc.top - g_capture.rc.top) * srcPitch
        + (SIZE_T)(lt.rc.left - g_capture.rc.left) * 4;

    if ((SIZE_T)lr.Pitch == srcPitch && rowBytes == srcPitch)
    {
        memcpy(lr.pBits, src, srcPitch * rows);
    }
    else
    {
        for (UINT y = 0; y < rows; y++)
            memcpy((BYTE *)lr.pBits + (SIZE_T)y * lr.Pitch, src + y * srcPitch, rowBytes);
    }
    lt.pStaging->UnlockRect(0);

    IDirect3DSurface9 *pSrc, *pDst;
    if (SUCCEEDED(lt.pStaging->GetSurfaceLevel(0, &pSrc)))
    {
        if (SUCCEEDED(lt.pTexture->GetSurfaceLevel(0, &pDst)))
        {
            if (SUCCEEDED(lt.pDevice->UpdateSurface(pSrc, nullptr, pDst, nullptr)))
                lt.pTexture->GenerateMipSubLevels();
            pDst->Release();
        }
        pSrc->Release();
    }
}

/* Upload the newest captured frame to every monitor's texture */
void UpdateLiveTextures(void)
{
    ULONG seq = g_capture.seq;
    if (seq == g_capture.consumed || g_liveTextures.empty())
        return;

    AcquireSRWLockShared(&g_capture.lock);
    for (auto &lt : g_liveTextures)
        UploadLiveTexture(lt);
    ReleaseSRWLockShared(&g_capture.lock);

    // Hand the buffer back to the capture thread
    g_capture.consumed = seq;
    SetEvent(g_hUploadedEvent);
}

/* Only the fullscreen saver windows; the Settings preview is a child window and never listed here */
BOOL CALLBACK ExcludeSaverWindowFromCapture(HWND hwnd, LPARAM)
{
    WCHAR szClass[32];
    if (GetClassNameW(hwnd, szClass, ARRAYSIZE(szClass))
    && 0 == wcscmp(szClass, L"D3DSaverWndClass"))
    {
        if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE))
            g_bExcludedFromCapture = true;
        else
            Wh_Log(L"SetWindowDisplayAffinity failed (%u), live feed disabled", GetLastError());
    }
    return TRUE;
}

HRESULT (*CD3DScreensaver_CreateSaverWindow_orig)(void *);
HRESULT CD3DScreensaver_CreateSaverWindow_hook(void *pThis)
{
    HRESULT hr = CD3DScreensaver_CreateSaverWindow_orig(pThis);
    if (g_bLiveFeed)
        EnumThreadWindows(GetCurrentThreadId(), ExcludeSaverWindowFromCapture, 0);
    return hr;
}

/* Remember which monitor is being snapshotted */
BOOL (*MonitorEnumProc_orig)(HMONITOR, HDC, LPRECT, LPARAM);
BOOL MonitorEnumProc_hook(HMONITOR hMonitor, HDC hdc, LPRECT lprcMonitor, LPARAM dwData)
{
    g_pEnumMonitorRect = lprcMonitor;
    BOOL result = MonitorEnumProc_orig(hMonitor, hdc, lprcMonitor, dwData);
    g_pEnumMonitorRect = nullptr;
    return result;
}

/* MonitorEnumProc reads the snapshot pixels right after the 18-byte TGA header */
decltype(&GetDIBits) GetDIBits_orig;
int WINAPI GetDIBits_hook(HDC hdc, HBITMAP hbm, UINT start, UINT cLines, LPVOID lpvBits, LPBITMAPINFO lpbmi, UINT usage)
{
    int result = GetDIBits_orig(hdc, hbm, start, cLines, lpvBits, lpbmi, usage);
    if (g_pEnumMonitorRect && lpvBits && result)
    {
        const BYTE *tgaData = (const BYTE *)lpvBits - 18;

        // A new snapshot of a known monitor only moves its TGA
        for (auto &monitor : g_monitors)
        {
            if (EqualRect(&monitor.rc, g_pEnumMonitorRect))
            {
                monitor.tgaData = tgaData;
                return result;
            }
        }

        AcquireSRWLockExclusive(&g_monitorsLock);
        g_monitors.push_back({ *g_pEnumMonitorRect, tgaData });
        ReleaseSRWLockExclusive(&g_monitorsLock);
    }
    return result;
}

/* Track the textures created from the monitor snapshots */
HRESULT (*Texture_CreateTextureFromTGAFileInMemory_orig)(IDirect3DDevice9 *, const BYTE *, UINT, UINT, DWORD, D3DFORMAT, IDirect3DTexture9 **);
HRESULT Texture_CreateTextureFromTGAFileInMemory_hook(
    IDirect3DDevice9   *pDevice,
    const BYTE         *pData,
    UINT                a3,
    UINT                a4,
    DWORD               a5,
    D3DFORMAT           a6,
    IDirect3DTexture9 **ppTexture
)
{
    HRESULT hr = Texture_CreateTextureFromTGAFileInMemory_orig(
        pDevice, pData, a3, a4, a5, a6, ppTexture
    );
    if (FAILED(hr) || !*ppTexture || !g_bLiveFeed || !g_bExcludedFromCapture)
        return hr;

    for (const auto &monitor : g_monitors)
    {
        if (monitor.tgaData == pData)
        {
            // Replaces any entry this render unit left behind without an Invalidate
            DropLiveTextures([&](const LiveTexture &lt)
            {
                return lt.renderUnit == g_currentRenderUnit && EqualRect(&lt.rc, &monitor.rc);
            });
            g_liveTextures.push_back({ g_currentRenderUnit, monitor.rc, pDevice, *ppTexture, nullptr });
            StartCaptureThread();
            break;
        }
    }
    return hr;
}

/* Called by SwitchToRenderUnit with the render unit becoming current */
void (*BubblesScreensaver_SetDevice_orig)(void *, UINT);
void BubblesScreensaver_SetDevice_hook(void *pThis, UINT iDevice)
{
    g_currentRenderUnit = iDevice;
    BubblesScreensaver_SetDevice_orig(pThis, iDevice);
}

/* The current render unit's textures are about to be released */
HRESULT (*BubblesScreensaver_InvalidateDeviceObjects_orig)(void *);
HRESULT BubblesScreensaver_InvalidateDeviceObjects_hook(void *pThis)
{
    DropLiveTextures([](const LiveTexture &lt)
    {
        return lt.renderUnit == g_currentRenderUnit;
    });
    return BubblesScreensaver_InvalidateDeviceObjects_orig(pThis);
}

/* Called once per frame, before every render unit is drawn */
HRESULT (*CD3DScreensaver_Render3DEnvironment_orig)(void *);
HRESULT CD3DScreensaver_Render3DEnvironment_hook(void *pThis)
{
    UpdateLiveTextures();
    return CD3DScreensaver_Render3DEnvironment_orig(pThis);
}

const WindhawkUtils::SYMBOL_HOOK bubblesScrHooks[] = {
    {
        {
            L"protected: long __cdecl CD3DScreensaver::CreateSaverWindow(void)"
        },
        &CD3DScreensaver_CreateSaverWindow_orig,
        CD3DScreensaver_CreateSaverWindow_hook,
        false
    },
    {
        {
            L"int __cdecl MonitorEnumProc(struct HMONITOR__ *,struct HDC__ *,struct tagRECT *,__int64)"
        },
        &MonitorEnumProc_orig,
        MonitorEnumProc_hook,
        false
    },
    {
        {
            L"long __cdecl Texture::CreateTextureFromTGAFileInMemory(struct IDirect3DDevice9 *,unsigned char const *,unsigned int,unsigned int,unsigned long,enum _D3DFORMAT,struct IDirect3DTexture9 * *)"
        },
        &Texture_CreateTextureFromTGAFileInMemory_orig,
        Texture_CreateTextureFromTGAFileInMemory_hook,
        false
    },
    {
        {
            L"public: virtual void __cdecl BubblesScreensaver::SetDevice(unsigned int)"
        },
        &BubblesScreensaver_SetDevice_orig,
        BubblesScreensaver_SetDevice_hook,
        false
    },
    {
        {
            L"public: virtual long __cdecl BubblesScreensaver::InvalidateDeviceObjects(void)"
        },
        &BubblesScreensaver_InvalidateDeviceObjects_orig,
        BubblesScreensaver_InvalidateDeviceObjects_hook,
        false
    },
    {
        {
            L"protected: long __cdecl CD3DScreensaver::Render3DEnvironment(void)"
        },
        &CD3DScreensaver_Render3DEnvironment_orig,
        CD3DScreensaver_Render3DEnvironment_hook,
        false
    }
};

bool g_bIsWinlogon = false;

void LoadSettings(void)
{
    g_bLiveFeed = Wh_GetIntSetting(L"liveFeed");
    g_fps = std::clamp(Wh_GetIntSetting(L"fps"), 1, 120);
}

BOOL Wh_ModInit(void)
{
    g_bIsWinlogon = GetModuleHandleW(L"winlogon.exe") != nullptr;
    if (g_bIsWinlogon)
    {
        if (!WindhawkUtils::HookSymbols(GetModuleHandleW(NULL), winlogonExeHooks, ARRAYSIZE(winlogonExeHooks)))
        {
            Wh_Log(L"Failed to hook one or more symbol functions in winlogon.exe");
            return FALSE;
        }
        return TRUE;
    }

    LoadSettings();

    if (!WindhawkUtils::HookSymbols(GetModuleHandleW(NULL), bubblesScrHooks, ARRAYSIZE(bubblesScrHooks)))
    {
        Wh_Log(L"Failed to hook one or more symbol functions in Bubbles.scr");
        return FALSE;
    }
    if (!WindhawkUtils::SetFunctionHook(GetDIBits, GetDIBits_hook, &GetDIBits_orig))
    {
        Wh_Log(L"Failed to hook GetDIBits");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModUninit(void)
{
    // Staging textures belong to the render thread's devices and are left to process exit
    if (!g_bIsWinlogon)
        StopCaptureThread();
}

void Wh_ModSettingsChanged(void)
{
    if (!g_bIsWinlogon)
        LoadSettings();
}
