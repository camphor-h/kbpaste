/*
 * kbpaste - Keyboard Simulated Paste Tool
 * Copyright (C) 2026 FogRain
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <string>
#include <stdlib.h>
#include <stdio.h>

// 全局变量
HWND g_hMainWnd = NULL;
HWND g_hEdit = NULL;
HWND g_hWaitBox = NULL;
HWND g_hIntervalBox = NULL;
HWND g_hBtnApply = NULL;
HWND g_hBtnStop = NULL;
HWND g_hStatusText = NULL;

HHOOK g_hKeyboardHook = NULL;
std::wstring g_strTextToType;
int g_nOutputIndex = 0;
int g_nWaitSeconds = 0;
int g_nIntervalMs = 5;
bool g_bStopping = false;

// 控件ID
#define IDC_EDIT_TEXT 1001
#define IDC_WAIT_BOX 1002
#define IDC_INTERVAL_BOX 1003
#define IDC_BTN_APPLY 1004
#define IDC_STATUS_TEXT 1005
#define IDC_BTN_STOP 1006

// 全局热键ID
#define HK_PASTE_V 0x0001
#define HK_COPY_C 0x0002
#define HK_STOP_G 0x0003

// 规范化文本：过滤掉\r，保留\n和\t和可打印字符
std::wstring NormalizeText(const std::wstring& src)
{
    std::wstring result;
    size_t len = src.length();
    size_t i = 0;

    for (i = 0; i < len; i++) {
        WCHAR ch = src[i];

        if (ch == L'\r') {
            // 跳过\r，只保留\n
            continue;
        } else if (ch == L'\n') {
            result += L'\n';
        } else if (ch == L'\t') {
            result += L'\t';
        } else if (ch >= 0x20) {
            result += ch;
        }
    }

    return result;
}

// 模拟Shift+Enter（软回车，适用于Monaco/大多数网页编辑器）
void SimulateShiftEnter(int intervalMs)
{
    INPUT inputs[4];
    memset(inputs, 0, sizeof(inputs));

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_SHIFT;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_RETURN;

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = VK_RETURN;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_SHIFT;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(4, inputs, sizeof(INPUT));
    if (intervalMs > 0) Sleep((DWORD)intervalMs);
    Sleep(50);
}

// 模拟单个字符击键
void SimulateKeyStroke(WCHAR ch, int intervalMs)
{
    // 换行符改用 Shift+Enter
    if (ch == L'\n') {
        SimulateShiftEnter(intervalMs);
        return;
    }
    if (ch == L'\r') return; // 跳过\r

    // 制表符
    if (ch == L'\t') {
        INPUT inputs[2];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_TAB;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_TAB;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
        if (intervalMs > 0) Sleep((DWORD)intervalMs);
        return;
    }

    INPUT inputs[2];
    memset(inputs, 0, sizeof(inputs));

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = ch;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wScan = ch;
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
    if (intervalMs > 0) Sleep((DWORD)intervalMs);
}

// 从剪贴板获取文本
std::wstring GetClipboardText()
{
    std::wstring result;
    if (!OpenClipboard(NULL)) return result;

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        wchar_t* pText = (wchar_t*)GlobalLock(hData);
        if (pText) {
            result = pText;
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return result;
}

// 安全解析整数 (C++98兼容，使用wcstol代替stoi)
int SafeParseInt(const wchar_t* str, int defaultVal, int minVal, int maxVal)
{
    if (!str || str[0] == L'\0') return defaultVal;

    // 检查是否全为数字
    int i = 0;
    for (i = 0; str[i] != L'\0'; i++) {
        if (str[i] < L'0' || str[i] > L'9') {
            return defaultVal;
        }
    }

    long val = wcstol(str, NULL, 10);
    if (val < (long)minVal) return minVal;
    if (val > (long)maxVal) return maxVal;
    return (int)val;
}

// 更新状态文本
void UpdateStatus(const wchar_t* text)
{
    if (g_hStatusText) {
        SetWindowTextW(g_hStatusText, text);
    }
}

// 处理待处理的消息（防止Sleep阻塞热键）
void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// 键盘钩子回调
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* pKb = (KBDLLHOOKSTRUCT*)lParam;

        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        // 检测 Ctrl+Alt+V (模拟击键输出)
        if (pKb->vkCode == 'V' && ctrl && alt) {
            // 从编辑框获取文本
            wchar_t buf[32768];
            memset(buf, 0, sizeof(buf));
            GetWindowTextW(g_hEdit, buf, 32768);
            g_strTextToType = buf;
            g_nOutputIndex = 0;

            if (!g_strTextToType.empty()) {
                // 规范化文本（统一换行符）
                g_strTextToType = NormalizeText(g_strTextToType);
                g_bStopping = false;

                // 启用停止按钮
                EnableWindow(g_hBtnStop, TRUE);

                // 倒计时
                int i = 0;
                for (i = g_nWaitSeconds; i > 0; i--) {
                    if (g_bStopping) break;
                    wchar_t title[128];
                    swprintf(title, L"%d 秒后开始输出...", i);
                    UpdateStatus(title);
                    // 每秒处理消息
                    int j = 0;
                    for (j = 0; j < 100; j++) {
                        if (g_bStopping) break;
                        PumpMessages();
                        Sleep(10);
                    }
                }

                if (!g_bStopping) {
                    UpdateStatus(L"正在输出...");

                    // 模拟逐字符输出
                    size_t len = g_strTextToType.length();
                    size_t idx = 0;
                    for (idx = 0; idx < len; idx++) {
                        if (g_bStopping) {
                            UpdateStatus(L"已停止输出");
                            break;
                        }
                        SimulateKeyStroke(g_strTextToType[idx], g_nIntervalMs);
                        g_nOutputIndex = (int)(idx + 1);
                        // 每个字符后都处理消息，确保停止键即时响应
                        PumpMessages();
                    }

                    if (!g_bStopping) {
                        UpdateStatus(L"输出完成，等待下一次触发");
                    }
                }

                // 禁用停止按钮
                EnableWindow(g_hBtnStop, FALSE);
            } else {
                UpdateStatus(L"待输出文本为空");
            }
            return 1;
        }

        // 检测 Ctrl+Alt+G (停止输出)
        if (pKb->vkCode == 'G' && ctrl && alt) {
            if (g_bStopping == false) {
                g_bStopping = true;
                EnableWindow(g_hBtnStop, FALSE);
                UpdateStatus(L"已按Ctrl+Alt+G停止输出");
            }
            return 1;
        }

        // 检测 Ctrl+Alt+C (从剪贴板读取到编辑框)
        if (pKb->vkCode == 'C' && ctrl && alt) {
            std::wstring clipText = GetClipboardText();
            if (!clipText.empty()) {
                SetWindowTextW(g_hEdit, clipText.c_str());
                g_nOutputIndex = 0;
                UpdateStatus(L"已从剪贴板读取文本");
            } else {
                UpdateStatus(L"剪贴板为空");
            }
            return 1;
        }
    }

    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

// 窗口过程
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"待输出文本（支持多行和换行）：",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            10, 10, 400, 20,
            hWnd, (HMENU)0, NULL, NULL);

        g_hEdit = CreateWindowW(L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL,
            10, 30, 460, 120,
            hWnd, (HMENU)IDC_EDIT_TEXT, NULL, NULL);

        CreateWindowW(L"STATIC", L"开始前等待秒数：",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            10, 160, 120, 20,
            hWnd, (HMENU)0, NULL, NULL);

        g_hWaitBox = CreateWindowW(L"EDIT", L"0",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
            130, 158, 60, 22,
            hWnd, (HMENU)IDC_WAIT_BOX, NULL, NULL);

        CreateWindowW(L"STATIC", L"模拟击键间隔(ms)：",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            210, 160, 130, 20,
            hWnd, (HMENU)0, NULL, NULL);

        g_hIntervalBox = CreateWindowW(L"EDIT", L"5",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER,
            345, 158, 70, 22,
            hWnd, (HMENU)IDC_INTERVAL_BOX, NULL, NULL);

        g_hBtnApply = CreateWindowW(L"BUTTON", L"应用配置",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            425, 158, 70, 24,
            hWnd, (HMENU)IDC_BTN_APPLY, NULL, NULL);

        CreateWindowW(L"STATIC", L"快捷键：Ctrl+Alt+V 输出 | Ctrl+Alt+C 读取 | Ctrl+Alt+G 停止",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            10, 186, 420, 25,
            hWnd, (HMENU)0, NULL, NULL);

        g_hBtnStop = CreateWindowW(L"BUTTON", L"停止",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_DISABLED,
            430, 186, 65, 24,
            hWnd, (HMENU)IDC_BTN_STOP, NULL, NULL);

        g_hStatusText = CreateWindowW(L"STATIC", L"就绪",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            10, 214, 480, 22,
            hWnd, (HMENU)IDC_STATUS_TEXT, NULL, NULL);

        CreateWindowW(L"STATIC", L"说明：换行使用Shift+Enter（软回车）。间隔设为0可能卡顿，请谨慎设置！",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            10, 238, 480, 25,
            hWnd, (HMENU)0, NULL, NULL);

        CreateWindowW(L"STATIC", L"提示：内嵌VSCode/Monaco编辑器的网页，右键选择Format Document即可一键格式化代码",
            WS_VISIBLE | WS_CHILD | SS_LEFT,
            10, 262, 480, 35,
            hWnd, (HMENU)0, NULL, NULL);

        break;
    }

    case WM_COMMAND: {
        int cmd = LOWORD(wParam);

        if (cmd == IDC_BTN_STOP) {
            g_bStopping = true;
            EnableWindow(g_hBtnStop, FALSE);
            return 0;
        }

        if (cmd == IDC_BTN_APPLY) {
            // 读取等待秒数（防御性编程）
            wchar_t waitBuf[32];
            memset(waitBuf, 0, sizeof(waitBuf));
            GetWindowTextW(g_hWaitBox, waitBuf, 32);

            // 检查空输入
            if (waitBuf[0] == L'\0') {
                g_nWaitSeconds = 0;
            } else {
                // 验证是否全为数字
                bool allDigits = true;
                int i = 0;
                for (i = 0; waitBuf[i] != L'\0'; i++) {
                    if (waitBuf[i] < L'0' || waitBuf[i] > L'9') {
                        allDigits = false;
                        break;
                    }
                }

                if (allDigits) {
                    g_nWaitSeconds = SafeParseInt(waitBuf, 0, 0, 60);
                } else {
                    UpdateStatus(L"错误：等待秒数必须为纯数字");
                    return 0;
                }
            }

            // 读取击键间隔（防御性编程）
            wchar_t intervalBuf[32];
            memset(intervalBuf, 0, sizeof(intervalBuf));
            GetWindowTextW(g_hIntervalBox, intervalBuf, 32);

            // 检查空输入
            if (intervalBuf[0] == L'\0') {
                g_nIntervalMs = 0;
            } else {
                // 验证是否全为数字
                bool allDigits = true;
                int i = 0;
                for (i = 0; intervalBuf[i] != L'\0'; i++) {
                    if (intervalBuf[i] < L'0' || intervalBuf[i] > L'9') {
                        allDigits = false;
                        break;
                    }
                }

                if (allDigits) {
                    g_nIntervalMs = SafeParseInt(intervalBuf, 0, 0, 10000);
                } else {
                    UpdateStatus(L"错误：击键间隔必须为纯数字");
                    return 0;
                }
            }

            // 更新状态文本
            wchar_t status[128];
            swprintf(status, L"配置已更新 - 等待%d秒，间隔%d毫秒", g_nWaitSeconds, g_nIntervalMs);
            UpdateStatus(status);
        }

        break;
    }

    case WM_CLOSE: {
        if (g_hKeyboardHook) {
            UnhookWindowsHookEx(g_hKeyboardHook);
        }
        DestroyWindow(hWnd);
        break;
    }

    case WM_DESTROY: {
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(WNDCLASSEXW));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.hInstance = hInst;
    wc.lpszClassName = L"KbPasteWndClass";
    wc.lpfnWndProc = WndProc;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"注册窗口类失败！", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_hMainWnd = CreateWindowW(
        L"KbPasteWndClass",
        L"键盘模拟粘贴工具",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 510, 340,
        NULL, NULL, hInst, NULL
    );

    if (!g_hMainWnd) {
        MessageBoxW(NULL, L"创建窗口失败！", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    RegisterHotKey(g_hMainWnd, HK_PASTE_V, MOD_CONTROL | MOD_ALT, 'V');
    RegisterHotKey(g_hMainWnd, HK_COPY_C, MOD_CONTROL | MOD_ALT, 'C');
    RegisterHotKey(g_hMainWnd, HK_STOP_G, MOD_CONTROL | MOD_ALT, 'G');

    g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hInst, 0);

    MSG msg;
    memset(&msg, 0, sizeof(MSG));
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}