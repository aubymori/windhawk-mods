// ==WindhawkMod==
// @id              win32-tray-clock-experience
// @name            Win32 Tray Clock Experience
// @description     Use the Win32 clock flyout instead of the XAML one
// @version         1.1.0
// @author          aubymori
// @github          https://github.com/aubymori
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -lole32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Win32 Tray Clock Experience
This mod makes the clock button on the Windows 10 taskbar open the
Win32 tray clock from Windows 8.1 and before instead of the XAML one
from Windows 10 and up. This *should* work on Windows 11 with the legacy
taskbar, but I have not tested it.

Based on the ExplorerPatcher implementation.

**Preview**:

![Preview](https://raw.githubusercontent.com/aubymori/images/main/win32-tray-clock-experience.png)
*/
// ==/WindhawkModReadme==

#include <Windows.h>
#include <CommCtrl.h>
#include <initguid.h>   
#include <Unknwn.h>    
#include <objbase.h> 

DEFINE_GUID(GUID_Win32Clock,
    0x0A323554A,
    0x0FE1, 0x4E49, 0xae, 0xe1,
    0x67, 0x22, 0x46, 0x5d, 0x79, 0x9f
);
DEFINE_GUID(IID_Win32Clock,
    0x7A5FCA8A,
    0x76B1, 0x44C8, 0xa9, 0x7c,
    0xe7, 0x17, 0x3c, 0xca, 0x5f, 0x4f
);
typedef interface Win32Clock Win32Clock;

typedef struct Win32ClockVtbl
{
    BEGIN_INTERFACE

        HRESULT(STDMETHODCALLTYPE* QueryInterface)(
            Win32Clock* This,
            /* [in] */ REFIID riid,
            /* [annotation][iid_is][out] */
            _COM_Outptr_  void** ppvObject);

    ULONG(STDMETHODCALLTYPE* AddRef)(
        Win32Clock* This);

    ULONG(STDMETHODCALLTYPE* Release)(
        Win32Clock* This);

    HRESULT(STDMETHODCALLTYPE* ShowWin32Clock)(
        Win32Clock* This,
        /* [in] */ HWND hWnd,
        /* [in] */ LPRECT lpRect);

    END_INTERFACE
} Win32ClockVtbl;

interface Win32Clock
{
    CONST_VTBL struct Win32ClockVtbl* lpVtbl;
};

BOOL ShowLegacyClockExperience(HWND hWnd)
{
    if (!hWnd)
    {
        return FALSE;
    }
    HRESULT hr = S_OK;
    Win32Clock* pWin32Clock = NULL;
    hr = CoCreateInstance(
        GUID_Win32Clock,
        NULL,
        CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER,
        IID_Win32Clock,
        (void**)&pWin32Clock
    );
    if (SUCCEEDED(hr))
    {
        RECT rc;
        GetWindowRect(hWnd, &rc);
        pWin32Clock->lpVtbl->ShowWin32Clock(pWin32Clock, hWnd, &rc);
        pWin32Clock->lpVtbl->Release(pWin32Clock);
    }
    return TRUE;
}

LRESULT CALLBACK ClockButtonSubclassProc(
    HWND hWnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR uIdSubclass,
    DWORD_PTR dwRefData
)
{
    if (uMsg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hWnd, ClockButtonSubclassProc, 1);
    }
    else if (uMsg == WM_LBUTTONDOWN || (uMsg == WM_KEYDOWN && wParam == VK_RETURN))
    {
        if (!FindWindowW(L"ClockFlyoutWindow", NULL))
        {
            return ShowLegacyClockExperience(hWnd);
        }
        else
        {
            return 1;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Orig;
HWND WINAPI CreateWindowExW_Hook(DWORD dwExStyle,LPCWSTR lpClassName,LPCWSTR lpWindowName,
DWORD dwStyle,int X,int Y,int nWidth,int nHeight,HWND hWndParent,HMENU hMenu,HINSTANCE hInstance,LPVOID lpParam) {

    HWND hWnd = CreateWindowExW_Orig(dwExStyle,lpClassName,lpWindowName,dwStyle,X,Y,nWidth,nHeight,hWndParent,hMenu,hInstance,lpParam);

    if ((((ULONG_PTR)lpClassName & ~(ULONG_PTR)0xffff) != 0) && ((!wcscmp(lpClassName, L"TrayClockWClass") || !wcscmp(lpClassName, L"ClockButton")))) {
		SetWindowSubclass(hWnd, ClockButtonSubclassProc, 1, 0);
    }

    return hWnd;
}

BOOL Wh_ModInit(void)
{
        Wh_SetFunctionHook((void*)CreateWindowExW, (void*)CreateWindowExW_Hook, (void**)&CreateWindowExW_Orig);

    return TRUE;
}
