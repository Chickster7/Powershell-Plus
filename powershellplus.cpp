// PowerShell+ GUI - Pure Win32, no external dependencies
// Compile (MinGW): g++ -o PowerShellPlus.exe powershell_plus_gui.cpp -lgdi32 -mwindows
// Compile (MSVC):  cl /EHsc powershell_plus_gui.cpp /link gdi32.lib user32.lib

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>
#include <vector>

// ─── Constants ────────────────────────────────────────────────────────────────
#define WND_WIDTH     900
#define WND_HEIGHT    580
#define TITLEBAR_H    40
#define CLOSE_BTN_W   52
#define INPUTBAR_H    44
#define FONT_SIZE     13
#define CLEAR_EVERY   7

#define IDC_OUTPUT    101
#define IDC_INPUT     102
#define IDC_CLOSEBTN  103

#define WM_APPEND_TEXT (WM_USER + 1)
#define WM_CMD_DONE    (WM_USER + 2)

#define COL_BG         RGB(10,  14,  26)
#define COL_TITLEBAR   RGB(8,   12,  22)
#define COL_ACCENT     RGB(0,   210, 220)
#define COL_ACCENT2    RGB(100, 60,  220)
#define COL_TEXT       RGB(200, 230, 255)
#define COL_DIM        RGB(80,  100, 140)
#define COL_SUCCESS    RGB(60,  220, 120)
#define COL_ERROR      RGB(255, 80,  80)
#define COL_PROMPT     RGB(0,   210, 220)
#define COL_INPUT_BG   RGB(14,  20,  36)
#define COL_CLOSE_HOV  RGB(220, 50,  50)

static inline int GET_X(LPARAM lp) { return (int)(short)LOWORD(lp); }
static inline int GET_Y(LPARAM lp) { return (int)(short)HIWORD(lp); }

static HWND  g_hWnd        = NULL;
static HWND  g_hOutput     = NULL;
static HWND  g_hInput      = NULL;
static HFONT g_hFont       = NULL;
static HFONT g_hFontBold   = NULL;
static HFONT g_hFontTitle  = NULL;
static WNDPROC g_OrigInput = NULL;
static WNDPROC g_OrigClose = NULL;

static std::vector<std::wstring> g_history;
static int  g_histIdx    = -1;
static int  g_cmdCount   = 0;
static bool g_closeHover = false;
static bool g_dragging   = false;
static POINT g_dragPt    = {0,0};
static RECT  g_dragWnd   = {0,0,0,0};
static HBRUSH g_inputBrush = NULL;

struct AppendData {
    std::wstring text;
    COLORREF     color;
    bool         bold;
};

static std::wstring CurDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetCurrentDirectoryW(MAX_PATH, buf);
    return n ? std::wstring(buf, n) : L"?";
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static void AppendText(const std::wstring& text) {
    int len = GetWindowTextLengthW(g_hOutput);
    SendMessageW(g_hOutput, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hOutput, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    int lines = (int)SendMessageW(g_hOutput, EM_GETLINECOUNT, 0, 0);
    SendMessageW(g_hOutput, EM_LINESCROLL, 0, (LPARAM)lines);
}

static void PostText(const std::wstring& t, COLORREF c = COL_TEXT, bool b = false) {
    AppendData* d = new AppendData{t, c, b};
    PostMessageW(g_hWnd, WM_APPEND_TEXT, 0, (LPARAM)d);
}

static void PrintPrompt() {
    int rem = CLEAR_EVERY - (g_cmdCount % CLEAR_EVERY);
    std::wstring s = L"\r\n[PS+] " + CurDir()
                   + L" [" + std::to_wstring(rem) + L"] > ";
    AppendText(s);
}

static void PrintBanner() {
    AppendText(L"╔══════════════════════════════════════════╗\r\n");
    AppendText(L"║      PowerShell+  v1.0  --  Pure Win32  ║\r\n");
    AppendText(L"╚══════════════════════════════════════════╝\r\n");
    AppendText(L"  Type 'help' for commands. "
               L"Auto-clears every 7 commands.\r\n");
}

static void ClearOutput() {
    SetWindowTextW(g_hOutput, L"");
    PrintBanner();
}

static void PrintHelp() {
    AppendText(L"\r\n  Built-in Commands\r\n");
    AppendText(L"  ────────────────────────────────\r\n");
    AppendText(L"  help            Show this help\r\n");
    AppendText(L"  cls / clear     Clear terminal\r\n");
    AppendText(L"  history         Show history\r\n");
    AppendText(L"  cd <path>       Change directory\r\n");
    AppendText(L"  exit / quit     Close app\r\n");
    AppendText(L"  <anything>      Passed to PowerShell\r\n\r\n");
}

struct CmdParam { std::wstring cmd; };

static DWORD WINAPI RunPS(LPVOID lpParam) {
    CmdParam* p = (CmdParam*)lpParam;
    std::wstring fullCmd = L"powershell.exe -NoLogo -NonInteractive -Command \""
                         + p->cmd + L"\"";
    delete p;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hROut=NULL, hWOut=NULL, hRErr=NULL, hWErr=NULL;
    CreatePipe(&hROut, &hWOut, &sa, 0);
    SetHandleInformation(hROut, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&hRErr, &hWErr, &sa, 0);
    SetHandleInformation(hRErr, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb          = sizeof(si);
    si.hStdOutput  = hWOut;
    si.hStdError   = hWErr;
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(fullCmd.begin(), fullCmd.end());
    buf.push_back(L'\0');

    if (!CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        PostText(L"  [PS+] Failed to launch PowerShell.\r\n");
        CloseHandle(hROut); CloseHandle(hRErr);
        CloseHandle(hWOut); CloseHandle(hWErr);
        PostMessageW(g_hWnd, WM_CMD_DONE, 0, 0);
        return 0;
    }
    CloseHandle(hWOut);
    CloseHandle(hWErr);

    char cbuf[2048];
    DWORD nRead;

    auto readPipe = [&](HANDLE h) {
        while (ReadFile(h, cbuf, sizeof(cbuf)-1, &nRead, NULL) && nRead > 0) {
            cbuf[nRead] = '\0';
            int wlen = MultiByteToWideChar(CP_ACP, 0, cbuf, -1, NULL, 0);
            std::wstring ws(wlen, L'\0');
            MultiByteToWideChar(CP_ACP, 0, cbuf, -1, &ws[0], wlen);
            ws.resize(wlen > 0 ? wlen - 1 : 0);
            std::wstring out;
            for (size_t i = 0; i < ws.size(); i++) {
                if (ws[i] == L'\n' && (i == 0 || ws[i-1] != L'\r'))
                    out += L"\r\n";
                else
                    out += ws[i];
            }
            PostText(out);
        }
    };

    readPipe(hROut);
    readPipe(hRErr);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(hROut); CloseHandle(hRErr);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    PostMessageW(g_hWnd, WM_CMD_DONE, 0, 0);
    return 0;
}

static void ExecuteCmd(const std::wstring& raw) {
    std::wstring cmd = Trim(raw);
    if (cmd.empty()) { PrintPrompt(); return; }

    if (g_history.empty() || g_history.back() != cmd)
        g_history.push_back(cmd);
    g_histIdx = -1;

    AppendText(cmd + L"\r\n");
    g_cmdCount++;

    if (cmd == L"exit" || cmd == L"quit") {
        PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
        return;
    }
    if (cmd == L"help" || cmd == L"?") {
        PrintHelp();
    }
    else if (cmd == L"cls" || cmd == L"clear") {
        ClearOutput(); g_cmdCount = 0;
    }
    else if (cmd == L"history") {
        AppendText(L"\r\n  Command History\r\n  ──────────────────\r\n");
        for (int i = 0; i < (int)g_history.size(); i++) {
            AppendText(L"  " + std::to_wstring(i+1) + L".  " + g_history[i] + L"\r\n");
        }
        AppendText(L"\r\n");
    }
    else if (cmd.size() >= 2 && cmd.substr(0,2) == L"cd") {
        std::wstring path = cmd.size() > 3 ? Trim(cmd.substr(3)) : L"";
        if (!path.empty() && path.front() == L'"') path = path.substr(1, path.size()-2);
        if (path.empty()) {
            wchar_t* home = NULL; size_t sz = 0;
            if (_wdupenv_s(&home, &sz, L"USERPROFILE") == 0 && home) {
                SetCurrentDirectoryW(home); free(home);
            }
        } else {
            if (!SetCurrentDirectoryW(path.c_str()))
                AppendText(L"  Cannot find path: " + path + L"\r\n");
        }
    }
    else {
        CmdParam* p = new CmdParam{cmd};
        HANDLE hT = CreateThread(NULL, 0, RunPS, p, 0, NULL);
        if (hT) CloseHandle(hT);
        return;
    }

    if (g_cmdCount > 0 && g_cmdCount % CLEAR_EVERY == 0) {
        AppendText(L"\r\n  [PS+] Auto-clearing...\r\n");
        Sleep(600); ClearOutput(); g_cmdCount = 0;
    }
    PrintPrompt();
}

static void DrawCloseBtn(HDC hdc, RECT rc, bool hover) {
    HBRUSH bg = CreateSolidBrush(hover ? COL_CLOSE_HOV : COL_TITLEBAR);
    FillRect(hdc, &rc, bg); DeleteObject(bg);

    if (hover) {
        HPEN glow = CreatePen(PS_SOLID, 2, RGB(255,120,120));
        HPEN old  = (HPEN)SelectObject(hdc, glow);
        MoveToEx(hdc, rc.left, rc.bottom-1, NULL);
        LineTo(hdc, rc.right, rc.bottom-1);
        SelectObject(hdc, old); DeleteObject(glow);
    }

    int cx = (rc.left+rc.right)/2, cy = (rc.top+rc.bottom)/2, s = 7;
    COLORREF xc = hover ? RGB(255,255,255) : RGB(160,180,210);
    HPEN xp = CreatePen(PS_SOLID, 2, xc);
    HPEN op = (HPEN)SelectObject(hdc, xp);
    MoveToEx(hdc, cx-s, cy-s, NULL); LineTo(hdc, cx+s, cy+s);
    MoveToEx(hdc, cx+s, cy-s, NULL); LineTo(hdc, cx-s, cy+s);
    SelectObject(hdc, op); DeleteObject(xp);
}

static void DrawTitlebar(HDC hdc, RECT rc) {
    HBRUSH bg = CreateSolidBrush(COL_TITLEBAR);
    FillRect(hdc, &rc, bg); DeleteObject(bg);

    HPEN ln = CreatePen(PS_SOLID, 2, COL_ACCENT);
    HPEN ol = (HPEN)SelectObject(hdc, ln);
    MoveToEx(hdc, 0, rc.bottom-1, NULL);
    LineTo(hdc, rc.right-CLOSE_BTN_W, rc.bottom-1);
    SelectObject(hdc, ol); DeleteObject(ln);

    int dy = TITLEBAR_H/2;
    COLORREF dots[3] = {RGB(255,80,80), RGB(255,190,50), RGB(60,200,100)};
    for (int i = 0; i < 3; i++) {
        int x = 16 + i*16;
        HBRUSH db = CreateSolidBrush(dots[i]);
        SelectObject(hdc, db);
        SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, x-4, dy-4, x+4, dy+4);
        DeleteObject(db);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COL_ACCENT);
    SelectObject(hdc, g_hFontTitle);
    RECT tr = rc; tr.left = 68; tr.right -= CLOSE_BTN_W+10;
    DrawTextW(hdc, L"PowerShell+", -1, &tr, DT_SINGLELINE|DT_VCENTER|DT_LEFT);

    SetTextColor(hdc, COL_DIM);
    SelectObject(hdc, g_hFont);
    RECT sr = tr; sr.left += 108;
    DrawTextW(hdc, L"Pure Win32 Terminal  v1.0", -1, &sr,
              DT_SINGLELINE|DT_VCENTER|DT_LEFT);
}

static LRESULT CALLBACK CloseBtnProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        DrawCloseBtn(hdc, rc, g_closeHover);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!g_closeHover) {
            g_closeHover = true;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    case WM_MOUSELEAVE:
        g_closeHover = false;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        PostMessageW(GetParent(hwnd), WM_CLOSE, 0, 0);
        return 0;
    }
    return CallWindowProcW(g_OrigClose, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK InputProc(HWND hwnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        switch (wParam) {
        case VK_RETURN: {
            wchar_t buf[4096] = {};
            GetWindowTextW(hwnd, buf, 4096);
            SetWindowTextW(hwnd, L"");
            ExecuteCmd(buf);
            return 0;
        }
        case VK_UP:
            if (!g_history.empty()) {
                if (g_histIdx < 0) g_histIdx = (int)g_history.size()-1;
                else if (g_histIdx > 0) g_histIdx--;
                SetWindowTextW(hwnd, g_history[g_histIdx].c_str());
                int n = GetWindowTextLengthW(hwnd);
                SendMessageW(hwnd, EM_SETSEL, n, n);
            }
            return 0;
        case VK_DOWN:
            if (g_histIdx >= 0) {
                if (g_histIdx < (int)g_history.size()-1) {
                    g_histIdx++;
                    SetWindowTextW(hwnd, g_history[g_histIdx].c_str());
                } else {
                    g_histIdx = -1;
                    SetWindowTextW(hwnd, L"");
                }
                int n = GetWindowTextLengthW(hwnd);
                SendMessageW(hwnd, EM_SETSEL, n, n);
            }
            return 0;
        }
    }
    return CallWindowProcW(g_OrigInput, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        RECT cr; GetClientRect(hwnd, &cr);
        int outH = cr.bottom - TITLEBAR_H - INPUTBAR_H;
        HDC hdc  = GetDC(NULL);
        int dpi  = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);

        g_hFont = CreateFontW(-MulDiv(FONT_SIZE,dpi,72),
            0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas");

        g_hFontBold = CreateFontW(-MulDiv(FONT_SIZE,dpi,72),
            0,0,0,FW_BOLD,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas");

        g_hFontTitle = CreateFontW(-MulDiv(11,dpi,72),
            0,0,0,FW_BOLD,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");

        g_hOutput = CreateWindowExW(
            WS_EX_STATICEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|
            ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            0, TITLEBAR_H, cr.right, outH,
            hwnd, (HMENU)IDC_OUTPUT,
            ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        SendMessageW(g_hOutput, WM_SETFONT, (WPARAM)g_hFont, FALSE);
        SendMessageW(g_hOutput, EM_SETMARGINS,
            EC_LEFTMARGIN|EC_RIGHTMARGIN, MAKELONG(10,10));

        int iy = TITLEBAR_H + outH;
        g_hInput = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            22, iy+8, cr.right-36, INPUTBAR_H-16,
            hwnd, (HMENU)IDC_INPUT,
            ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        SendMessageW(g_hInput, WM_SETFONT, (WPARAM)g_hFont, FALSE);
        g_OrigInput = (WNDPROC)SetWindowLongPtrW(
            g_hInput, GWLP_WNDPROC, (LONG_PTR)InputProc);

        HWND hClose = CreateWindowExW(
            0, L"BUTTON", L"",
            WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            cr.right-CLOSE_BTN_W, 0, CLOSE_BTN_W, TITLEBAR_H,
            hwnd, (HMENU)IDC_CLOSEBTN,
            ((LPCREATESTRUCT)lParam)->hInstance, NULL);
        g_OrigClose = (WNDPROC)SetWindowLongPtrW(
            hClose, GWLP_WNDPROC, (LONG_PTR)CloseBtnProc);

        g_inputBrush = CreateSolidBrush(COL_INPUT_BG);
        PrintBanner();
        PrintPrompt();
        SetFocus(g_hInput);
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        int outH = h - TITLEBAR_H - INPUTBAR_H;
        if (outH < 1) outH = 1;
        SetWindowPos(g_hOutput, NULL, 0, TITLEBAR_H, w, outH, SWP_NOZORDER);
        SetWindowPos(g_hInput, NULL, 22, TITLEBAR_H+outH+8,
                     w-36, INPUTBAR_H-16, SWP_NOZORDER);
        HWND hC = GetDlgItem(hwnd, IDC_CLOSEBTN);
        SetWindowPos(hC, NULL, w-CLOSE_BTN_W, 0,
                     CLOSE_BTN_W, TITLEBAR_H, SWP_NOZORDER);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);

        HBRUSH bgBr = CreateSolidBrush(COL_BG);
        FillRect(hdc, &cr, bgBr); DeleteObject(bgBr);

        RECT tb = {0, 0, cr.right, TITLEBAR_H};
        DrawTitlebar(hdc, tb);

        int iy = cr.bottom - INPUTBAR_H;
        HPEN sp = CreatePen(PS_SOLID, 1, COL_ACCENT2);
        HPEN op = (HPEN)SelectObject(hdc, sp);
        MoveToEx(hdc, 0, iy, NULL); LineTo(hdc, cr.right, iy);
        SelectObject(hdc, op); DeleteObject(sp);

        RECT ib = {0, iy, cr.right, cr.bottom};
        HBRUSH ibBr = CreateSolidBrush(COL_INPUT_BG);
        FillRect(hdc, &ib, ibBr); DeleteObject(ibBr);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_PROMPT);
        SelectObject(hdc, g_hFontBold ? g_hFontBold : g_hFont);
        RECT pl = {6, iy+2, 20, iy+INPUTBAR_H-2};
        DrawTextW(hdc, L">", -1, &pl, DT_SINGLELINE|DT_VCENTER|DT_LEFT);

        HPEN gl = CreatePen(PS_SOLID, 2, COL_ACCENT);
        HPEN og = (HPEN)SelectObject(hdc, gl);
        MoveToEx(hdc, 0, cr.bottom-1, NULL); LineTo(hdc, cr.right, cr.bottom-1);
        SelectObject(hdc, og); DeleteObject(gl);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == g_hOutput) {
            SetBkColor(hdc, COL_BG);
            SetTextColor(hdc, COL_TEXT);
            static HBRUSH outBr = NULL;
            if (!outBr) outBr = CreateSolidBrush(COL_BG);
            return (LRESULT)outBr;
        }
        if (hCtrl == g_hInput) {
            SetBkColor(hdc, COL_INPUT_BG);
            SetTextColor(hdc, COL_ACCENT);
            return (LRESULT)g_inputBrush;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int mx = GET_X(lParam), my = GET_Y(lParam);
        if (my < TITLEBAR_H && mx < WND_WIDTH - CLOSE_BTN_W) {
            g_dragging = true;
            // Store cursor in SCREEN coords and window origin at drag start
            GetCursorPos(&g_dragPt);
            GetWindowRect(hwnd, &g_dragWnd);
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT cur; GetCursorPos(&cur);
            // Move window by how far cursor has moved from drag-start
            SetWindowPos(hwnd, NULL,
                g_dragWnd.left + (cur.x - g_dragPt.x),
                g_dragWnd.top  + (cur.y - g_dragPt.y),
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        g_dragging = false;
        ReleaseCapture();
        return 0;

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT pt = {GET_X(lParam), GET_Y(lParam)};
            ScreenToClient(hwnd, &pt);
            RECT cr2; GetClientRect(hwnd, &cr2);
            const int B = 6;
            bool L2 = pt.x < B, R2 = pt.x > cr2.right-B;
            bool T2 = pt.y < B, B2 = pt.y > cr2.bottom-B;
            if (L2 && T2) return HTTOPLEFT;
            if (R2 && T2) return HTTOPRIGHT;
            if (L2 && B2) return HTBOTTOMLEFT;
            if (R2 && B2) return HTBOTTOMRIGHT;
            if (L2) return HTLEFT;  if (R2) return HTRIGHT;
            if (T2) return HTTOP;   if (B2) return HTBOTTOM;
        }
        return hit;
    }

    case WM_APPEND_TEXT: {
        AppendData* d = (AppendData*)lParam;
        if (d) { AppendText(d->text); delete d; }
        return 0;
    }

    case WM_CMD_DONE:
        if (g_cmdCount > 0 && g_cmdCount % CLEAR_EVERY == 0) {
            AppendText(L"\r\n  [PS+] Auto-clearing...\r\n");
            Sleep(600); ClearOutput(); g_cmdCount = 0;
        }
        AppendText(L"\r\n");
        PrintPrompt();
        SetFocus(g_hInput);
        return 0;

    case WM_DESTROY:
        if (g_inputBrush) DeleteObject(g_inputBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"PSPlusGUI";
    wc.style         = CS_HREDRAW|CS_VREDRAW;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_APPWINDOW,
        L"PSPlusGUI", L"PowerShell+",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, WND_WIDTH, WND_HEIGHT,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    g_hWnd = hwnd;

    // 50% transparency (alpha 128 / 255)
    SetLayeredWindowAttributes(hwnd, 0, 215, LWA_ALPHA);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}