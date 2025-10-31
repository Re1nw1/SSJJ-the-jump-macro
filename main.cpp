#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <wchar.h>
#include <shlobj.h>
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

#ifndef VK_XBUTTON1
#define VK_XBUTTON1 0x05
#endif
#ifndef VK_XBUTTON2
#define VK_XBUTTON2 0x06
#endif

// 全局
HINSTANCE g_hInst;
HWND g_hWnd;
volatile BOOL running = TRUE;

// 热键默认
volatile UINT vkBhop   = VK_F6; // 连跳
volatile UINT vkCrouch = VK_F7; // 双蹲
volatile UINT vkHasten = VK_F8; // 空速脚本
volatile UINT vkAutoAS = VK_F9; // 自动空速

// 捕获模式状态（等待用户输入）
volatile BOOL capBhop   = FALSE;
volatile BOOL capCrouch = FALSE;
volatile BOOL capHasten = FALSE;
volatile BOOL capAutoAS = FALSE;

// 状态
volatile BOOL bhopActive = FALSE;
volatile BOOL crouchHeld = FALSE;
volatile BOOL hastenHeld = FALSE;
volatile BOOL autoASHeld = FALSE;

// 事件与钩子
HANDLE evtBhopStart, evtCrouchStart, evtHastenStart, evtAutoASStart;
HHOOK hKbdHook, hMouseHook;

// 参数
int   bhopDelay       = 3;
int   crouchDown      = 29;
int   crouchUp        = 4;
int   hastenAmplitude = 150;
int   hastenDelay     = 4;
DWORD fpsLast         = 120;

// 自动空速相关
DWORD lastMoveTime  = 0;
BOOL  keyDownA      = FALSE;
BOOL  keyDownD      = FALSE;
DWORD lastWheelTime = 0;
BOOL  wheelGate     = FALSE;
const DWORD WHEEL_GATE_WINDOW_MS = 200;
const DWORD STOP_THRESHOLD_MS     = 10;

// 新增：忽略滚轮检测选项（默认不忽略）
volatile BOOL autoASIgnoreWheel = FALSE;

// 配置文件路径
wchar_t g_configPath[MAX_PATH] = L"";

// 自动空速按压/切换逻辑
volatile DWORD autoASKeyDownTime = 0;
const DWORD AUTOAS_LONGPRESS_MS = 300; // 长按阈值
volatile BOOL autoASLongPressMode = FALSE;
volatile BOOL autoASPrevState = FALSE; // 记录按下前状态用于短按切换

// 鼠标侧键统一作为热键
volatile BOOL autoASMouseHeld = FALSE;
volatile DWORD autoASMouseDownTime = 0;
volatile BOOL autoASMousePrevState = FALSE; // 记录鼠标侧键按下前状态

// 前置声明
INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelKbdProc(int, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelMouseProc(int, WPARAM, LPARAM);

// 工具函数
void UpdateStatus(const wchar_t* text) {
    if (g_hWnd) SetDlgItemTextW(g_hWnd, IDC_STATIC_STATUS, text);
}
void PressKey(BYTE vk, BOOL isDown) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}
void SendWheelDown() {
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = -WHEEL_DELTA;
    SendInput(1, &in, sizeof(in));
}
void SendShift(BOOL down) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_SHIFT;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}
void MoveMouseRelative(int dx, int dy) {
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}
void PressReleaseKey(WORD vk, int delay) {
    INPUT in[2] = {0};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(delay);
    SendInput(1, &in[1], sizeof(INPUT));
}

// 键名
void VkName(UINT vk, wchar_t* buf, size_t cch) {
    // 鼠标侧键友好名称
    if (vk == VK_XBUTTON1) { wcsncpy_s(buf, cch, L"鼠标侧键1", _TRUNCATE); return; }
    if (vk == VK_XBUTTON2) { wcsncpy_s(buf, cch, L"鼠标侧键2", _TRUNCATE); return; }

    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) scan |= 0x01000000;
    if (GetKeyNameTextW((LONG)scan, buf, (int)cch) <= 0) wsprintfW(buf, L"VK 0x%02X", vk);
}
void RefreshHotkeyEdits() {
    wchar_t buf[64];
    if (!g_hWnd) return;
    VkName(vkBhop,   buf, 64); SetDlgItemTextW(g_hWnd, IDC_EDIT_BHOP,   buf);
    VkName(vkCrouch, buf, 64); SetDlgItemTextW(g_hWnd, IDC_EDIT_CROUCH, buf);
    VkName(vkHasten, buf, 64); SetDlgItemTextW(g_hWnd, IDC_EDIT_HASTEN, buf);
    VkName(vkAutoAS, buf, 64); SetDlgItemTextW(g_hWnd, IDC_EDIT_AUTOAS, buf);
}

// RawInput
void RegisterRawInput(HWND hWnd) {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x02;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hWnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

// 配置路径
void InitConfigPath() {
    wchar_t docPath[MAX_PATH] = L"";
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docPath))) {
        wsprintfW(g_configPath, L"%s\\HotkeyToolConfig.ini", docPath);
    } else {
        GetModuleFileNameW(NULL, g_configPath, MAX_PATH);
        wchar_t* p = wcsrchr(g_configPath, L'\\');
        if (p) *(p + 1) = L'\0';
        wcscat_s(g_configPath, MAX_PATH, L"HotkeyToolConfig.ini");
    }
}

// INI
void SaveConfig() {
    wchar_t buf[32];
    wsprintfW(buf, L"%u", vkBhop);   WritePrivateProfileStringW(L"Hotkeys", L"Bhop",   buf, g_configPath);
    wsprintfW(buf, L"%u", vkCrouch); WritePrivateProfileStringW(L"Hotkeys", L"Crouch", buf, g_configPath);
    wsprintfW(buf, L"%u", vkHasten); WritePrivateProfileStringW(L"Hotkeys", L"Hasten", buf, g_configPath);
    wsprintfW(buf, L"%u", vkAutoAS); WritePrivateProfileStringW(L"Hotkeys", L"AutoAS", buf, g_configPath);

    wsprintfW(buf, L"%d", bhopDelay);       WritePrivateProfileStringW(L"Params", L"BhopDelay",       buf, g_configPath);
    wsprintfW(buf, L"%d", crouchDown);      WritePrivateProfileStringW(L"Params", L"CrouchDown",      buf, g_configPath);
    wsprintfW(buf, L"%d", crouchUp);        WritePrivateProfileStringW(L"Params", L"CrouchUp",        buf, g_configPath);
    wsprintfW(buf, L"%d", hastenAmplitude); WritePrivateProfileStringW(L"Params", L"HastenAmplitude", buf, g_configPath);
    wsprintfW(buf, L"%d", hastenDelay);     WritePrivateProfileStringW(L"Params", L"HastenDelay",     buf, g_configPath);

    // 新增：忽略滚轮检测
    wsprintfW(buf, L"%d", autoASIgnoreWheel ? 1 : 0);
    WritePrivateProfileStringW(L"Params", L"AutoASIgnoreWheel", buf, g_configPath);
}
void LoadConfig() {
    vkBhop   = GetPrivateProfileIntW(L"Hotkeys", L"Bhop",   VK_F6, g_configPath);
    vkCrouch = GetPrivateProfileIntW(L"Hotkeys", L"Crouch", VK_F7, g_configPath);
    vkHasten = GetPrivateProfileIntW(L"Hotkeys", L"Hasten", VK_F8, g_configPath);
    vkAutoAS = GetPrivateProfileIntW(L"Hotkeys", L"AutoAS", VK_F9, g_configPath);

    bhopDelay       = GetPrivateProfileIntW(L"Params", L"BhopDelay",       3,   g_configPath);
    crouchDown      = GetPrivateProfileIntW(L"Params", L"CrouchDown",      29,  g_configPath);
    crouchUp        = GetPrivateProfileIntW(L"Params", L"CrouchUp",        4,   g_configPath);
    hastenAmplitude = GetPrivateProfileIntW(L"Params", L"HastenAmplitude", 150, g_configPath);
    hastenDelay     = GetPrivateProfileIntW(L"Params", L"HastenDelay",     4,   g_configPath);

    // 新增：忽略滚轮检测
    autoASIgnoreWheel = GetPrivateProfileIntW(L"Params", L"AutoASIgnoreWheel", 0, g_configPath) ? TRUE : FALSE;
}

// 注：此处你之前的字符串是乱码，我用中文名示例；你可以按需改回你原来的注册表路径
DWORD ReadCurrentFPSFromRegistry() {
    HKEY hKey;
    DWORD fps = 120, type = REG_DWORD, size = sizeof(DWORD);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Wooduan\\生死狙击微操战术", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"FrameLimit_h731164557", NULL, &type, (LPBYTE)&fps, &size);
        RegCloseKey(hKey);
    }
    return fps;
}

// 线程
DWORD WINAPI ThreadBhop(LPVOID) {
    timeBeginPeriod(1);
    while (running) {
        if (WaitForSingleObject(evtBhopStart, INFINITE) == WAIT_OBJECT_0) {
            UpdateStatus(L"连跳运行中...");
            while (bhopActive && running) { SendWheelDown(); Sleep(bhopDelay); }
            UpdateStatus(L"连跳停止");
        }
    }
    timeEndPeriod(1);
    return 0;
}
DWORD WINAPI ThreadCrouch(LPVOID) {
    timeBeginPeriod(1);
    while (running) {
        if (WaitForSingleObject(evtCrouchStart, INFINITE) == WAIT_OBJECT_0) {
            UpdateStatus(L"双蹲运行中...");
            while (crouchHeld && running) { SendShift(TRUE); Sleep(crouchDown); SendShift(FALSE); Sleep(crouchUp); }
            UpdateStatus(L"双蹲停止");
        }
    }
    timeEndPeriod(1);
    return 0;
}
DWORD WINAPI ThreadHasten(LPVOID) {
    timeBeginPeriod(1);
    while (running) {
        if (WaitForSingleObject(evtHastenStart, INFINITE) == WAIT_OBJECT_0) {
            UpdateStatus(L"空速脚本运行中...");
            while (hastenHeld && running) {
                MoveMouseRelative(hastenAmplitude, 0);
                PressReleaseKey('A', hastenDelay);
                MoveMouseRelative(-hastenAmplitude, 0);
                PressReleaseKey('D', hastenDelay);
            }
            UpdateStatus(L"空速脚本停止");
        }
    }
    timeEndPeriod(1);
    return 0;
}
DWORD WINAPI ThreadAutoAS(LPVOID) {
    timeBeginPeriod(1);
    while (running) {
        if (WaitForSingleObject(evtAutoASStart, INFINITE) == WAIT_OBJECT_0) {
            UpdateStatus(L"自动空速运行中...");
            while (autoASHeld && running) {
                DWORD now = GetTickCount();

                // 忽略滚轮检测时：仅依据移动停止来松开按键
                if (autoASIgnoreWheel) {
                    if (now - lastMoveTime > STOP_THRESHOLD_MS) {
                        if (keyDownA) { PressKey('A', FALSE); keyDownA = FALSE; }
                        if (keyDownD) { PressKey('D', FALSE); keyDownD = FALSE; }
                    }
                } else {
                    wheelGate = (now - lastWheelTime <= WHEEL_GATE_WINDOW_MS);
                    if (!wheelGate || (now - lastMoveTime > STOP_THRESHOLD_MS)) {
                        if (keyDownA) { PressKey('A', FALSE); keyDownA = FALSE; }
                        if (keyDownD) { PressKey('D', FALSE); keyDownD = FALSE; }
                    }
                }
                Sleep(5);
            }
            if (keyDownA) { PressKey('A', FALSE); keyDownA = FALSE; }
            if (keyDownD) { PressKey('D', FALSE); keyDownD = FALSE; }
            UpdateStatus(L"自动空速停止");
        }
    }
    timeEndPeriod(1);
    return 0;
}

// 键盘钩子
LRESULT CALLBACK LowLevelKbdProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        UINT vk = p->vkCode;
        if (wParam == WM_KEYDOWN) {
            if (capBhop)   { vkBhop   = vk; capBhop   = FALSE; RefreshHotkeyEdits(); UpdateStatus(L"已设置连跳热键"); return 1; }
            if (capCrouch) { vkCrouch = vk; capCrouch = FALSE; RefreshHotkeyEdits(); UpdateStatus(L"已设置双蹲热键"); return 1; }
            if (capHasten) { vkHasten = vk; capHasten = FALSE; RefreshHotkeyEdits(); UpdateStatus(L"已设置空速脚本热键"); return 1; }
            if (capAutoAS) { vkAutoAS = vk; capAutoAS = FALSE; RefreshHotkeyEdits(); UpdateStatus(L"已设置自动空速热键"); return 1; }
        }
        if (vk == vkBhop && wParam == WM_KEYDOWN) { bhopActive = !bhopActive; if (bhopActive) SetEvent(evtBhopStart); }

        if (vk == vkCrouch) {
            if (wParam == WM_KEYDOWN && !crouchHeld) { crouchHeld = TRUE; SetEvent(evtCrouchStart); }
            else if (wParam == WM_KEYUP) { crouchHeld = FALSE; }
        }
        if (vk == vkHasten) {
            if (wParam == WM_KEYDOWN && !hastenHeld) { hastenHeld = TRUE; SetEvent(evtHastenStart); }
            else if (wParam == WM_KEYUP) { hastenHeld = FALSE; }
        }

        // 自动空速：短按切换、长按维持
        if (vk == vkAutoAS) {
            if (wParam == WM_KEYDOWN) {
                if (autoASKeyDownTime == 0) autoASKeyDownTime = GetTickCount();
                autoASPrevState = autoASHeld;
                if (!autoASHeld) {
                    autoASHeld = TRUE;
                    autoASLongPressMode = TRUE;
                    SetEvent(evtAutoASStart);
                } else {
                    autoASLongPressMode = TRUE;
                }
            } else if (wParam == WM_KEYUP) {
                DWORD held = GetTickCount() - autoASKeyDownTime;
                autoASKeyDownTime = 0;
                if (held < AUTOAS_LONGPRESS_MS) {
                    BOOL newState = !autoASPrevState;
                    autoASHeld = newState;
                    if (autoASHeld && !autoASPrevState) {
                        SetEvent(evtAutoASStart);
                    }
                } else {
                    autoASHeld = FALSE;
                }
                autoASLongPressMode = FALSE;
            }
        }
    }
    return CallNextHookEx(hKbdHook, nCode, wParam, lParam);
}

// 鼠标钩子
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;

        // 鼠标侧键作为热键
        if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
            UINT btn = HIWORD(p->mouseData); // XBUTTON1=1, XBUTTON2=2
            UINT vk = (btn == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;

            // 捕获模式：用侧键设置热键
            if (wParam == WM_XBUTTONDOWN) {
                if (capBhop)   { vkBhop   = vk; capBhop   = FALSE; RefreshHotkeyEdits(); UpdateStatus(L"已设置连跳热键"); return 1; }
                if (capCrouch) { vkCrouch = vk; capCrouch = FALSE; RefreshHotkeyEdits(); UpdateStatus(L"已设置双蹲热键"); return 1; }
                if (capHasten) { vkHasten = vk; capHasten = FALSE; RefreshHotkeyEdits(); UpdateStatus(L"已设置空速脚本热键"); return 1; }
                if (capAutoAS) { vkAutoAS = vk; capAutoAS = FALSE; RefreshHotkeyEdits(); UpdateStatus(L"已设置自动空速热键"); return 1; }
            }

            // 将侧键作为功能触发键
            if (vk == vkBhop && wParam == WM_XBUTTONDOWN) {
                bhopActive = !bhopActive; if (bhopActive) SetEvent(evtBhopStart);
                return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
            }
            if (vk == vkCrouch) {
                if (wParam == WM_XBUTTONDOWN && !crouchHeld) { crouchHeld = TRUE; SetEvent(evtCrouchStart); }
                else if (wParam == WM_XBUTTONUP) { crouchHeld = FALSE; }
                return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
            }
            if (vk == vkHasten) {
                if (wParam == WM_XBUTTONDOWN && !hastenHeld) { hastenHeld = TRUE; SetEvent(evtHastenStart); }
                else if (wParam == WM_XBUTTONUP) { hastenHeld = FALSE; }
                return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
            }

            // 自动空速：侧键支持短按切换、长按维持
            if (vk == vkAutoAS) {
                if (wParam == WM_XBUTTONDOWN) {
                    if (autoASMouseDownTime == 0) autoASMouseDownTime = GetTickCount();
                    autoASMousePrevState = autoASHeld;
                    if (!autoASHeld) {
                        autoASHeld = TRUE;
                        autoASLongPressMode = TRUE;
                        autoASMouseHeld = TRUE;
                        SetEvent(evtAutoASStart);
                    } else {
                        autoASLongPressMode = TRUE;
                        autoASMouseHeld = TRUE;
                    }
                } else if (wParam == WM_XBUTTONUP) {
                    DWORD held = GetTickCount() - autoASMouseDownTime;
                    autoASMouseDownTime = 0;
                    autoASMouseHeld = FALSE;
                    if (held < AUTOAS_LONGPRESS_MS) {
                        BOOL newState = !autoASMousePrevState;
                        autoASHeld = newState;
                        if (autoASHeld && !autoASMousePrevState) {
                            SetEvent(evtAutoASStart);
                        }
                    } else {
                        autoASHeld = FALSE;
                    }
                    autoASLongPressMode = FALSE;
                }
                return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
            }
        }

        if (wParam == WM_MOUSEWHEEL) {
            SHORT zDelta = (SHORT)HIWORD(p->mouseData);
            if (zDelta < 0) { lastWheelTime = GetTickCount(); wheelGate = TRUE; }
        }
    }
    return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
}

// 对话框过程
INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        g_hWnd = hDlg;

        // 公共控件初始化
        INITCOMMONCONTROLSEX icex; icex.dwSize = sizeof(icex); icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
        InitCommonControlsEx(&icex);

        // RawInput 注册
        RegisterRawInput(hDlg);

        // 标题
        SetWindowTextW(hDlg, L"生死狙击连跳宏  By : Reinwi_Ternence");

        // 标签与按钮文本
        SetDlgItemTextW(hDlg, IDC_STATIC_BHOP_LABEL,         L"连跳热键:");
        SetDlgItemTextW(hDlg, IDC_STATIC_BHOP_DELAY_LABEL,   L"延迟(ms):");
        SetDlgItemTextW(hDlg, IDC_STATIC_CROUCH_LABEL,       L"双蹲热键:");
        SetDlgItemTextW(hDlg, IDC_STATIC_CROUCH_DOWN_LABEL,  L"按下(ms):");
        SetDlgItemTextW(hDlg, IDC_STATIC_CROUCH_UP_LABEL,    L"松开(ms):");
        SetDlgItemTextW(hDlg, IDC_STATIC_HASTEN_LABEL,       L"空速脚本热键:");
        SetDlgItemTextW(hDlg, IDC_STATIC_HASTEN_AMP_LABEL,   L"幅度:");
        SetDlgItemTextW(hDlg, IDC_STATIC_HASTEN_DELAY_LABEL, L"延迟(ms):");
        SetDlgItemTextW(hDlg, IDC_STATIC_AUTOAS_LABEL,       L"自动空速热键:");
        SetDlgItemTextW(hDlg, IDC_STATIC_FPS_LABEL,          L"锁帧:");
        SetDlgItemTextW(hDlg, IDC_STATIC_STATUS,             L"状态: 就绪");

        SetDlgItemTextW(hDlg, IDC_BTN_FPS,   L"锁帧");
        SetDlgItemTextW(hDlg, IDC_BTN_APPLY, L"应用");

        // 复选框文本与状态
        SetDlgItemTextW(hDlg, IDC_CHK_AUTOAS_IGNOREWHEEL, L"忽略滚轮检测");
        CheckDlgButton(hDlg, IDC_CHK_AUTOAS_IGNOREWHEEL, autoASIgnoreWheel ? BST_CHECKED : BST_UNCHECKED);

        // 初始化热键显示与参数
        RefreshHotkeyEdits();
        wchar_t buf[32];
        wsprintfW(buf, L"%d", bhopDelay);        SetDlgItemTextW(hDlg, IDC_EDIT_BHOP_DELAY, buf);
        wsprintfW(buf, L"%d", crouchDown);       SetDlgItemTextW(hDlg, IDC_EDIT_CROUCH_DOWN, buf);
        wsprintfW(buf, L"%d", crouchUp);         SetDlgItemTextW(hDlg, IDC_EDIT_CROUCH_UP, buf);
        wsprintfW(buf, L"%d", hastenAmplitude);  SetDlgItemTextW(hDlg, IDC_EDIT_HASTEN_AMP, buf);
        wsprintfW(buf, L"%d", hastenDelay);      SetDlgItemTextW(hDlg, IDC_EDIT_HASTEN_DELAY, buf);

        // 锁帧值
        fpsLast = ReadCurrentFPSFromRegistry();
        wsprintfW(buf, L"%u", fpsLast);
        SetDlgItemTextW(hDlg, IDC_EDIT_FPS, buf);

        // 应用图标
        HICON hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCE(IDI_APPICON),
                                        IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
        if (hIcon) {
            SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessageW(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
        }

        // 默认焦点：应用按钮
        SetFocus(GetDlgItem(hDlg, IDC_BTN_APPLY));
        return FALSE;
    }
    case WM_INPUT: {
        UINT dwSize = sizeof(RAWINPUT);
        static BYTE lpb[sizeof(RAWINPUT)];
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) == sizeof(RAWINPUT)) {
            RAWINPUT* raw = (RAWINPUT*)lpb;
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                int dx = raw->data.mouse.lLastX;
                DWORD now = GetTickCount();
                if (raw->data.mouse.usButtonFlags & RI_MOUSE_WHEEL) {
                    SHORT zDelta = (SHORT)raw->data.mouse.usButtonData;
                    if (zDelta < 0) { lastWheelTime = now; wheelGate = TRUE; }
                }
                if (autoASHeld) {
                    lastMoveTime = now;
                    // 勾选忽略滚轮时，直接按水平移动触发；否则按滚轮门控
                    if (autoASIgnoreWheel || (wheelGate && (now - lastWheelTime <= WHEEL_GATE_WINDOW_MS))) {
                        if (dx < 0) {
                            if (!keyDownA) { PressKey('A', TRUE); keyDownA = TRUE; }
                            if (keyDownD) { PressKey('D', FALSE); keyDownD = FALSE; }
                        } else if (dx > 0) {
                            if (!keyDownD) { PressKey('D', TRUE); keyDownD = TRUE; }
                            if (keyDownA) { PressKey('A', FALSE); keyDownA = FALSE; }
                        }
                    }
                }
            }
        }
        return TRUE;
    }
    case WM_COMMAND: {
        if (HIWORD(wParam) == EN_SETFOCUS) {
            switch (LOWORD(wParam)) {
            case IDC_EDIT_BHOP:   capBhop   = TRUE; UpdateStatus(L"请按下一个按键作为连跳热键"); break;
            case IDC_EDIT_CROUCH: capCrouch = TRUE; UpdateStatus(L"请按下一个按键作为双蹲热键"); break;
            case IDC_EDIT_HASTEN: capHasten = TRUE; UpdateStatus(L"请按下一个按键作为空速脚本热键"); break;
            case IDC_EDIT_AUTOAS: capAutoAS = TRUE; UpdateStatus(L"请按下一个按键作为自动空速热键"); break;
            }
        }
        switch (LOWORD(wParam)) {
        case IDC_BTN_APPLY: {
            wchar_t buf[32];
            GetDlgItemTextW(hDlg, IDC_EDIT_BHOP_DELAY, buf, 32);   bhopDelay       = _wtoi(buf);
            GetDlgItemTextW(hDlg, IDC_EDIT_CROUCH_DOWN, buf, 32);  crouchDown      = _wtoi(buf);
            GetDlgItemTextW(hDlg, IDC_EDIT_CROUCH_UP, buf, 32);    crouchUp        = _wtoi(buf);
            GetDlgItemTextW(hDlg, IDC_EDIT_HASTEN_AMP, buf, 32);   hastenAmplitude = _wtoi(buf);
            GetDlgItemTextW(hDlg, IDC_EDIT_HASTEN_DELAY, buf, 32); hastenDelay     = _wtoi(buf);

            // 读取复选框状态
            autoASIgnoreWheel = (IsDlgButtonChecked(hDlg, IDC_CHK_AUTOAS_IGNOREWHEEL) == BST_CHECKED);

            SaveConfig();
            UpdateStatus(L"参数与热键已更新并保存");
            return TRUE;
        }
        case IDC_BTN_FPS: {
            wchar_t buf[32];
            GetDlgItemTextW(hDlg, IDC_EDIT_FPS, buf, 32);
            DWORD fps = _wtoi(buf);
            fpsLast = fps;

            HKEY hKey;
            LONG res = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Wooduan\\生死狙击微操战术",
                                       0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL);
            if (res == ERROR_SUCCESS) {
                RegSetValueExW(hKey, L"FrameLimit_h731164557", 0, REG_DWORD,
                               (const BYTE*)&fps, sizeof(DWORD));
                RegCloseKey(hKey);
                UpdateStatus(L"锁帧成功");
            } else {
                UpdateStatus(L"锁帧失败");
            }
            return TRUE;
        }
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(hDlg, 0);
        return TRUE;
    }
    return FALSE;
}

// 程序入口
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hInst = hInstance;

    InitConfigPath();
    LoadConfig();

    hKbdHook   = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKbdProc, hInstance, 0);
    hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);

    evtBhopStart   = CreateEventW(NULL, FALSE, FALSE, NULL);
    evtCrouchStart = CreateEventW(NULL, FALSE, FALSE, NULL);
    evtHastenStart = CreateEventW(NULL, FALSE, FALSE, NULL);
    evtAutoASStart = CreateEventW(NULL, FALSE, FALSE, NULL);

    CreateThread(NULL, 0, ThreadBhop,   NULL, 0, NULL);
    CreateThread(NULL, 0, ThreadCrouch, NULL, 0, NULL);
    CreateThread(NULL, 0, ThreadHasten, NULL, 0, NULL);
    CreateThread(NULL, 0, ThreadAutoAS, NULL, 0, NULL);

    DialogBoxParamW(hInstance, MAKEINTRESOURCE(IDD_MAIN), NULL, DlgProc, 0);

    running = FALSE;
    if (hKbdHook)   UnhookWindowsHookEx(hKbdHook);
    if (hMouseHook) UnhookWindowsHookEx(hMouseHook);
    if (evtBhopStart)   CloseHandle(evtBhopStart);
    if (evtCrouchStart) CloseHandle(evtCrouchStart);
    if (evtHastenStart) CloseHandle(evtHastenStart);
    if (evtAutoASStart) CloseHandle(evtAutoASStart);

    return 0;
}
