/* ============================================================================
 *  vitals.c  --  透明生命体征条 / Transparent Cyberpunk Vital-Sign Strip
 *
 *  纯 C99 + Win32 GDI, 无第三方依赖, 单文件.
 *
 *  特性:
 *    - 真·透明背景 (UpdateLayeredWindow + 预乘 alpha), 没有黑框
 *    - 一行文字, 模板里的 @1@ / @HR@ 是"数值槽", 每槽独立范围/漂移/颜色
 *    - 左键拖动移动(自动记忆位置), 滚轮调字号
 *    - 右键菜单: 字号 / 置顶 / 重载配置 / 回到默认位置 / 退出
 *    - 配置 vitals.ini (exe 同目录或当前目录), 支持热重载
 *    - 可调阈值报警色 + 红青重影(glitch)
 *
 *  编译: 双击 build.bat   (已在 VS2022 BuildTools + Win10 SDK 验证路径)
 * ==========================================================================*/

#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <shellapi.h>      /* 托盘图标 Shell_NotifyIcon */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")   /* 注册表: 开机自启动 */
#pragma comment(lib, "shell32.lib")    /* 托盘图标 */

/* ------------------------------------------------------------------ 常量 --*/
#define MAX_SLOTS   64
#define MAX_RUNS    512
#define MAX_TXT     256
#define MAX_LINE    4096
#define MAX_PATHW   1024

#define BUF_W       4096          /* 离屏缓冲上限 */
#define BUF_H       256

#define PADX        3
#define PADY        2

#define IDM_FONTUP  1001
#define IDM_FONTDN  1002
#define IDM_TOP     1003
#define IDM_RESET   1004
#define IDM_RELOAD  1005
#define IDM_EXIT    1006
#define IDM_AUTORUN 1007
#define IDM_DEVICE  1008
#define IDM_WAVE    1009
#define IDM_REFRESH 1010
#define IDM_SETTINGS 1011
#define IDM_TRAYSHOW 1012

#define WM_TRAYMSG  (WM_APP + 1)
#define WM_FRAME    (WM_APP + 2)   /* 看门线程投递出来的"该出下一帧了" */
#define IDI_APPICON 1
#define IDR_DEFINI  2         /* 内置的默认 vitals.ini (RCDATA 资源) */

#define SCROLL_TICK_MS  40        /* 退化模式下(没有高精度定时器)的滚动刷新间隔 */

#define WAVE_OS         4         /* 波形横向过采样倍数: 采样点比像素密 4 倍 */

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET    0x00000001
#endif

/* ------------------------------------------------------------------ 类型 --*/

/* 一个数值槽: @1@ 取的是它的 value */
typedef struct {
    wchar_t  key[48];             /* "1" / "HR" / ... */
    double   lo, hi;              /* 取值范围 */
    int      mode;                /* 0=fixed 1=random 2=walk */
    double   step;                /* walk 模式单次最大变化量 */
    int      decimals;            /* 小数位 */
    COLORREF color;               /* 正常颜色 */
    int      useAlert;            /* 是否启用报警阈值 */
    double   alertLo, alertHi;
    COLORREF alertColor;
    double   value;               /* 当前值 */
    int      rate;                /* 这个槽多少毫秒动一次 */
    ULONGLONG last;               /* 上次更新时间 */
    int      pad;                 /* 对齐宽度(字符数), 保证整行宽度恒定 */
    int      boxW;                /* 固定像素格子宽: 数字在这个格子里右对齐 */
    int      arrowW;              /* 箭头格子的固定像素宽 */
    int      pull;                /* 回归强度(%): 越大越贴着正常区间中心 */
} Slot;

/* 一"段": 要么是字面文本, 要么是一个槽的值, 要么是槽的箭头 */
typedef struct {
    int      isSlot;
    int      isArrow;
    int      isText;              /* 特殊文本段(比如 @DEVICE@ 显示设备名) */
    int      slot;
    int      cjk;                 /* 这段要用中文字体 */
    COLORREF color;
    wchar_t  text[MAX_TXT];
    int      w, x;
    int      tw;                  /* 文字本身的像素宽(用来在格子里右对齐) */
} Run;

typedef struct {
    wchar_t  font[64];
    wchar_t  fontCjk[64];         /* 留空 = 和主字体同一个(推荐, 中英才统一) */
    int      weight;              /* 字重 400=常规 700=粗 */
    int      align;               /* 1 = 数字等宽对齐, 整行宽度恒定 */
    int      size;                /* 96dpi 基准像素高 */
    int      letterSpace;         /* 段间额外间距(px) */
    COLORREF color;               /* 默认字色 */
    COLORREF bg;                  /* 背景色 */
    int      bgAlpha;             /* 0..255 */
    int      bgStyle;             /* 0=无 1=黑灰渐变 2=毛玻璃 3=通透玻璃 */
    int      bgRadius;            /* 圆角半径(px), 会被钳到 高/2 -> 就是四分之一个圆 */
    int      glitch;              /* 红青重影 */
    int      interval;            /* 刷新间隔 ms */
    int      grabFull;            /* 整条可拖动 */
    int      topmost;
    int      autofit;             /* 自动缩小字号以塞进屏幕(滚动模式自动关闭) */
    int      scroll;              /* 1 = 跑马灯滚动 */
    int      width;               /* 滚动时窗口宽度(px) */
    int      speed;               /* 滚动速度(px/秒) */
    int      gap;                 /* 每圈之间的空隙(px) */
    int      fps;                 /* 目标帧率 */
    int      opacity;             /* 文字整体透明度 0..255 */
    int      arrow;               /* 1 = 超范围时显示 ↑ / ↓ */
    int      pullDef;             /* 所有槽的默认回归强度(%) */
    int      bolden;              /* 加粗档位 0..2 (向右补画, 笔画变粗) */
    int      smooth;              /* 1 = 亚像素平滑滚动(消除整数像素的顿挫) */
    int      snap;                /* 1 = 每帧整数像素步进(最匀最脆, 推荐) */
    int      wave;                /* 1 = 显示实时波形小图 */
    int      waveH;               /* 波形带高度(px) */
    int      waveBold;            /* 波形描边: 上下各往外扩几像素 (0=最细) */
    int      waveType;            /* 0=ECG 1=PPG 2=RESP */
    COLORREF waveColor;
    wchar_t  line[MAX_LINE];      /* 模板 */
    int      posX, posY;
    Slot     slot[MAX_SLOTS];
    int      slotCount;
} Config;

/* ------------------------------------------------------------------ 全局 --*/
static Config      g_cfg;
static Run         g_run[MAX_RUNS];
static int         g_runCount   = 0;
static int         g_totalW     = 1;
static int         g_cellH     = 20;
static int         g_winW       = 1;
static int         g_winH       = 1;

static HDC         g_memDC      = NULL;   /* 背缓冲 */
static HDC         g_scrDC      = NULL;   /* 字形覆盖图(黑底白字) */
static HBITMAP     g_memBmp     = NULL;
static HBITMAP     g_scrBmp     = NULL;
static HBITMAP     g_lineBmp    = NULL;
static void       *g_memBits    = NULL;
static void       *g_scrBits    = NULL;
static void       *g_lineBits   = NULL;
static int         g_lineW      = 1;
static int         g_lineH      = 1;
static int         g_period     = 1;
static double      g_scroll     = 0.0;
static double      g_wavePhase  = 0.0;
static float       g_waveBuf[BUF_W];
static int         g_textH      = 1;
static int         g_waveOverride = -1;   /* 右键菜单切换过就覆盖 ini */

/* --- 性能相关 --- */
static HDC         g_screenDC   = NULL;   /* 缓存的屏幕 DC, 不再每帧 GetDC */
static int         g_lastSx     = -1;     /* 上次真正绘制用的整数偏移 */
static int         g_dirty      = 1;      /* 需要强制重画 */
static HANDLE      g_hTimer     = NULL;   /* 高精度等待定时器(帧同步) */
static LARGE_INTEGER g_qpcFreq;
static LARGE_INTEGER g_qpcStart;
static int         g_frameNo    = 0;
static ULONGLONG   g_lastTickMs = 0;      /* 上次帧回调时间, 看门狗用 */
static int         g_waveOS     = 1;      /* 波形横向过采样倍数 */
static int         g_waveN      = 0;      /* 波形缓冲有效采样数 = 窗口宽 x 过采样 */
static double      g_waveAcc    = 0.0;    /* 波形推进的亚像素累加器 */
static LARGE_INTEGER g_lastFrameQpc;
static int         g_lastFrameValid = 0;
static int         g_ftrace    = 0;       /* --ftrace: 记录每帧时间戳和滚动位置 */
static FILE       *g_ftraceF   = NULL;
static int         g_ftraceN   = 0;
static double      g_latEwma   = 0.0;     /* 定时器唤醒延迟的平滑估计(外生量, 补偿它才是稳定的) */
static ULONGLONG   g_lastMainFrameMs = 0; /* 主循环上次出帧的时刻(兜底判据用) */
static volatile LONG g_wndAlive = 0;      /* 看门线程用 */
static volatile LONG g_framePending = 0;  /* 已投递但还没被处理的 WM_FRAME */
static ULONGLONG   g_framePendingMs = 0;
static HANDLE      g_watchThread = NULL;
static double      g_lastWait  = 0.0;
static double      g_lastWork  = 0.0;
static LARGE_INTEGER g_prevFrameT;
static int         g_prevFrameTValid = 0;
static double      g_frameInterval = 1.0 / 60.0;

/* ---------------------------------------------------- 监测设备清单(演示) --*/
typedef struct {
    const wchar_t *vendor;
    const wchar_t *name;
    const wchar_t *kind;
    const wchar_t *link;
} Device;

static const Device g_dev[] = {
    { L"PHILIPS",     L"IntelliVue MX750",    L"BEDSIDE MONITOR",     L"LAN/USB"   },
    { L"GE",          L"CARESCAPE B650",      L"BEDSIDE MONITOR",     L"LAN"       },
    { L"MINDRAY",     L"BeneVision N22",      L"BEDSIDE MONITOR",     L"LAN/USB"   },
    { L"NIHON KOHDEN",L"Life Scope G9",       L"BEDSIDE MONITOR",     L"LAN"       },
    { L"DRAEGER",     L"Infinity Delta XL",   L"BEDSIDE MONITOR",     L"LAN"       },
    { L"SPACELABS",   L"XPREZZON",            L"BEDSIDE MONITOR",     L"LAN"       },
    { L"MASIMO",      L"Radical-7",           L"PULSE OXIMETER",      L"SERIAL"    },
    { L"NONIN",       L"WristOx 3150",        L"PULSE OXIMETER",      L"BLE 5.0"   },
    { L"APPLE",       L"Watch Ultra 2",       L"WEARABLE",            L"BLE 5.3"   },
    { L"HUAWEI",      L"Watch D2",            L"WEARABLE / BP",       L"BLE 5.2"   },
    { L"WITHINGS",    L"ScanWatch 2",         L"WEARABLE / ECG",      L"BLE 5.0"   },
    { L"OURA",        L"Ring Gen 4",          L"WEARABLE / HRV",      L"BLE 5.1"   },
    { L"WHOOP",       L"5.0",                 L"WEARABLE / HRV",      L"BLE 5.1"   },
    { L"GARMIN",      L"Index BPM",           L"BLOOD PRESSURE",      L"BLE 5.0"   },
    { L"OMRON",       L"HEM-7361T",           L"BLOOD PRESSURE",      L"USB"       },
    { L"DEXCOM",      L"G7",                  L"CGM",                 L"BLE 5.2"   },
    { L"ABBOTT",      L"FreeStyle Libre 3",   L"CGM",                 L"NFC/BLE"   },
    { L"MEDTRONIC",   L"Guardian 4",          L"CGM",                 L"BLE 5.0"   },
    { L"IRHYTHM",     L"Zio Patch XT",        L"ECG PATCH",           L"BLE"       },
    { L"EMOTIV",      L"EPOC X",              L"EEG / 14 CH",         L"USB DONGLE"},
    { L"MUSE",        L"S Athena",            L"EEG / 4 CH",          L"BLE 5.0"   },
    { L"ARTINIS",     L"Brite",               L"fNIRS / 8 CH",        L"BLE 5.0"   },
    { L"OPENBCI",     L"Cyton + Daisy",       L"EEG / 16 CH",         L"USB"       },
    { L"FLIR",        L"A700 Smart Sensor",   L"THERMAL IR CAMERA",   L"ETH/USB"   },
    { L"OPTRIS",      L"PI 640i",             L"THERMAL IR CAMERA",   L"USB"       },
    { L"SIEMENS",     L"Thermo-Scan SC",      L"IR SCREENING",        L"LAN"       },
    { L"BUTTERFLY",   L"iQ+",                 L"ULTRASOUND PROBE",    L"WLAN"      },
    { L"GE",          L"Vscan Air",           L"ULTRASOUND PROBE",    L"WLAN"      },
};
static const int   g_devCount = (int)(sizeof(g_dev) / sizeof(g_dev[0]));
static int         g_devSel   = 0;      /* 当前"连接"的设备 */
static int         g_devW     = 0;      /* 设备名渲染宽度(固定格子) */
static HWND        g_devWnd   = NULL;
static HICON       g_iconBig   = NULL;
static HICON       g_iconSmall = NULL;
static NOTIFYICONDATAW g_nid;
static int         g_trayOn    = 0;
static UINT        g_taskbarCreated = 0;
static int         g_hidden    = 0;
static int         g_showDev  = 0;      /* --devices: 启动就打开设备界面 */
static HFONT       g_devFont  = NULL;
static HFONT       g_devFontB = NULL;

static void DevDisplayName(int i, wchar_t *out, int cch)
{
    if (i < 0 || i >= g_devCount) { out[0] = 0; return; }
    _snwprintf(out, cch, L"%s %s", g_dev[i].vendor, g_dev[i].name);
    out[cch - 1] = 0;
}

static HFONT       g_font       = NULL;
static HDC     g_bdDC      = NULL;    /* 背景缓冲(毛玻璃要抓桌面再模糊) */
static HBITMAP g_bdBmp     = NULL;
static void   *g_bdBits    = NULL;
static int     g_bdW       = 0, g_bdH = 0;
static int     g_bdValid   = 0;
static ULONGLONG g_bdTime  = 0;
static BYTE   *g_mask      = NULL;    /* 圆角覆盖度图 (0..255) */
static int     g_maskW     = 0, g_maskH = 0, g_maskR = -1;
static HFONT       g_fontCJK    = NULL;
static HINSTANCE   g_inst       = NULL;
static HWND        g_hwnd       = NULL;
static int         g_drag       = 0;
static POINT       g_dragOff    = {0, 0};
static unsigned long long g_rng = 0x2545F4914F6CDD1DULL;

/* ============================================================== 小工具 ====*/

static void Trim(wchar_t *s)
{
    wchar_t *p = s;
    size_t n;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    n = wcslen(s);
    while (n > 0 && (s[n-1] == L' ' || s[n-1] == L'\t' ||
                     s[n-1] == L'\r' || s[n-1] == L'\n')) {
        s[--n] = 0;
    }
}

static void CopyStr(wchar_t *dst, const wchar_t *src, int cch)
{
    int i = 0;
    if (cch <= 0) return;
    while (i < cch - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int HexVal(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

static COLORREF ParseColor(const wchar_t *s, COLORREF def)
{
    int i, v = 0;
    while (*s == L' ' || *s == L'\t') s++;
    if (*s == L'#') s++;
    if (wcslen(s) >= 6) {
        for (i = 0; i < 6; i++) {
            int h = HexVal(s[i]);
            if (h < 0) return def;
            v = v * 16 + h;
        }
        return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }
    if (*s == L'g' || *s == L'G') return RGB(0x39, 0xFF, 0x14);
    if (*s == L'c' || *s == L'C') return RGB(0x00, 0xE5, 0xFF);
    if (*s == L'm' || *s == L'M') return RGB(0xFF, 0x2E, 0x88);
    if (*s == L'y' || *s == L'Y') return RGB(0xFF, 0xB3, 0x00);
    if (*s == L'r' || *s == L'R') return RGB(0xFF, 0x40, 0x40);
    if (*s == L'w' || *s == L'W') return RGB(0xFF, 0xFF, 0xFF);
    return def;
}

static double Rnd01(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (double)((g_rng >> 11) & 0x1FFFFFFFFFFFFULL) / (double)0x1FFFFFFFFFFFFULL;
}

/* =========================================================== 配置文件 ====*/

static Slot *FindSlot(const wchar_t *key)
{
    int i;
    for (i = 0; i < g_cfg.slotCount; i++) {
        if (_wcsicmp(g_cfg.slot[i].key, key) == 0) return &g_cfg.slot[i];
    }
    return NULL;
}

static Slot *AddSlot(const wchar_t *key)
{
    Slot *sl;
    if (g_cfg.slotCount >= MAX_SLOTS) return &g_cfg.slot[0];
    sl = &g_cfg.slot[g_cfg.slotCount++];
    memset(sl, 0, sizeof(*sl));
    CopyStr(sl->key, key, 48);
    sl->lo = 0; sl->hi = 100;
    sl->mode = 1; sl->step = 5; sl->decimals = 0;
    sl->color = g_cfg.color;
    sl->alertColor = RGB(0xFF, 0x2E, 0x88);
    sl->useAlert = 0;
    sl->value = 0;
    sl->rate = 1000;
    sl->last = 0;
    sl->pad = 1;
    sl->pull = g_cfg.pullDef;
    return sl;
}

/* 算出这个槽"最长可能写出来几个字符", 用来等宽对齐.
   这样数字位数变化时整行宽度不变, 滚动画面就不会抖 */
static void ComputePad(Slot *sl)
{
    wchar_t b[64];
    int w1, w2;
    if (sl->decimals > 0) swprintf(b, 64, L"%.*f", sl->decimals, sl->hi);
    else                  swprintf(b, 64, L"%.0f", sl->hi);
    w1 = (int)wcslen(b);
    if (sl->decimals > 0) swprintf(b, 64, L"%.*f", sl->decimals, sl->lo);
    else                  swprintf(b, 64, L"%.0f", sl->lo);
    w2 = (int)wcslen(b);
    sl->pad = (w1 > w2) ? w1 : w2;
    if (sl->pad < 1) sl->pad = 1;
    if (sl->pad > 20) sl->pad = 20;
}

/* 规格串语法:  lo~hi | 颜色 | 模式[:步长[:小数位]] | 报警低~报警高 | 报警色 */
static void ParseSpec(Slot *sl, const wchar_t *spec)
{
    wchar_t buf[1024];
    wchar_t *parts[8];
    int np = 0, i;

    CopyStr(buf, spec, 1024);
    {
        wchar_t *p = buf;
        while (np < 8) {
            wchar_t *bar = wcschr(p, L'|');
            parts[np++] = p;
            if (!bar) break;
            *bar = 0;
            p = bar + 1;
        }
    }
    for (i = 0; i < np; i++) Trim(parts[i]);

    /* [0] 范围 */
    if (np > 0 && parts[0][0]) {
        wchar_t *t = wcschr(parts[0], L'~');
        if (t) {
            wchar_t *a = parts[0], *b = t + 1;
            *t = 0;
            Trim(a); Trim(b);
            sl->lo = wcstod(a, NULL);
            sl->hi = wcstod(b, NULL);
        }
    }
    if (sl->hi < sl->lo) { double t = sl->lo; sl->lo = sl->hi; sl->hi = t; }
    if (sl->value < sl->lo) sl->value = sl->lo;
    if (sl->value > sl->hi) sl->value = sl->hi;

    /* [1] 颜色 */
    if (np > 1 && parts[1][0]) sl->color = ParseColor(parts[1], sl->color);

    /* [2] 模式[:步长[:小数位]] */
    if (np > 2 && parts[2][0]) {
        wchar_t *sp[3];
        int ns = 0;
        wchar_t *q = parts[2];
        while (ns < 3) {
            wchar_t *c = wcschr(q, L':');
            sp[ns++] = q;
            if (!c) break;
            *c = 0;
            q = c + 1;
        }
        for (i = 0; i < ns; i++) Trim(sp[i]);
        if (sp[0][0]) {
            if      (_wcsicmp(sp[0], L"fixed")  == 0) sl->mode = 0;
            else if (_wcsicmp(sp[0], L"random") == 0) sl->mode = 1;
            else if (_wcsicmp(sp[0], L"walk")   == 0) sl->mode = 2;
        }
        if (ns > 1 && sp[1][0]) sl->step     = wcstod(sp[1], NULL);
        if (ns > 2 && sp[2][0]) sl->decimals = (int)wcstol(sp[2], NULL, 10);
        if (sl->step < 0) sl->step = -sl->step;
        if (sl->decimals < 0) sl->decimals = 0;
        if (sl->decimals > 6) sl->decimals = 6;
    }

    /* [3] 报警区间 */
    if (np > 3 && parts[3][0]) {
        wchar_t *t = wcschr(parts[3], L'~');
        if (t) {
            wchar_t *a = parts[3], *b = t + 1;
            *t = 0;
            Trim(a); Trim(b);
            sl->alertLo = wcstod(a, NULL);
            sl->alertHi = wcstod(b, NULL);
            sl->useAlert = 1;
        }
    }
    /* [4] 报警色 */
    if (np > 4 && parts[4][0]) sl->alertColor = ParseColor(parts[4], sl->alertColor);

    ComputePad(sl);
}

static wchar_t *ReadTextFileW(const wchar_t *path)
{
    HANDLE h;
    DWORD  sz, rd = 0;
    char  *raw;
    wchar_t *out;
    int    n;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > (4u * 1024u * 1024u)) { CloseHandle(h); return NULL; }
    raw = (char *)malloc(sz + 1);
    if (!raw) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, raw, sz, &rd, NULL)) { free(raw); CloseHandle(h); return NULL; }
    CloseHandle(h);
    raw[rd] = 0;

    /* 跳过 UTF-8 BOM */
    {
        char *p = raw;
        DWORD  len = rd;
        if (len >= 3 && (unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) { p += 3; len -= 3; }
        n = MultiByteToWideChar(CP_UTF8, 0, p, (int)len, NULL, 0);
        out = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
        if (out) {
            MultiByteToWideChar(CP_UTF8, 0, p, (int)len, out, n);
            out[n] = 0;
        }
    }
    free(raw);
    return out;
}

static void Defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    CopyStr(g_cfg.font, L"DengXian", 64);      /* 等线: 中英 2:1 全等宽 */
    CopyStr(g_cfg.fontCjk, L"", 64);           /* 空 = 一个字体通吃 */
    g_cfg.weight      = 400;
    g_cfg.align       = 1;
    g_cfg.size        = 12;
    g_cfg.letterSpace = 3;
    g_cfg.color       = RGB(0x00, 0xE5, 0xFF);
    g_cfg.bg          = RGB(0x00, 0x00, 0x00);
    g_cfg.bgAlpha     = 0;
    g_cfg.bgStyle     = 0;
    g_cfg.bgRadius    = 40;
    g_cfg.glitch      = 0;
    g_cfg.interval    = 600;
    g_cfg.grabFull    = 1;
    g_cfg.topmost     = 1;
    g_cfg.autofit     = 0;
    g_cfg.scroll      = 1;
    g_cfg.width       = 560;
    g_cfg.speed       = 40;
    g_cfg.gap         = 110;
    g_cfg.fps         = 60;
    g_cfg.opacity     = 235;
    g_cfg.arrow       = 1;
    g_cfg.pullDef     = 2;
    g_cfg.bolden      = 1;
    g_cfg.smooth      = 1;
    g_cfg.snap        = 1;
    g_cfg.wave        = 1;
    g_cfg.waveH       = 26;
    g_cfg.waveBold    = 1;
    g_cfg.waveType    = 0;
    g_cfg.waveColor   = RGB(0x39, 0xFF, 0x14);
    g_cfg.posX        = INT_MIN;
    g_cfg.posY        = INT_MIN;
    g_cfg.slotCount   = 0;
    CopyStr(g_cfg.line,
            L"HR @HR@   SpO2 @9@   ALT @1@   BUN @5@", MAX_LINE);
}

static void ApplyKey(const wchar_t *k, const wchar_t *v)
{
    if      (_wcsicmp(k, L"font") == 0)         CopyStr(g_cfg.font, v, 64);
    else if (_wcsicmp(k, L"font_cjk") == 0)     CopyStr(g_cfg.fontCjk, v, 64);
    else if (_wcsicmp(k, L"weight") == 0)       g_cfg.weight      = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"align") == 0)        g_cfg.align       = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"size") == 0)         g_cfg.size        = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"letter_space") == 0) g_cfg.letterSpace = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"color") == 0)        g_cfg.color       = ParseColor(v, g_cfg.color);
    else if (_wcsicmp(k, L"bg") == 0)           g_cfg.bg          = ParseColor(v, g_cfg.bg);
    else if (_wcsicmp(k, L"bg_alpha") == 0)     g_cfg.bgAlpha     = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"bg_radius") == 0)    g_cfg.bgRadius    = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"bg_style") == 0) {
        if      (_wcsicmp(v, L"none")  == 0) g_cfg.bgStyle = 0;
        else if (_wcsicmp(v, L"grad")  == 0) g_cfg.bgStyle = 1;
        else if (_wcsicmp(v, L"glass") == 0) g_cfg.bgStyle = 3;
        else if (_wcsicmp(v, L"frost") == 0) g_cfg.bgStyle = 2;
        else                                 g_cfg.bgStyle = (int)wcstol(v, NULL, 10);
    }
    else if (_wcsicmp(k, L"glitch") == 0)       g_cfg.glitch      = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"interval") == 0)     g_cfg.interval    = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"grab_full") == 0)    g_cfg.grabFull    = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"topmost") == 0)      g_cfg.topmost     = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"autofit") == 0)      g_cfg.autofit     = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"scroll") == 0)       g_cfg.scroll      = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"width") == 0)        g_cfg.width       = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"speed") == 0)        g_cfg.speed       = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"gap") == 0)          g_cfg.gap         = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"fps") == 0)          g_cfg.fps         = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"opacity") == 0)      g_cfg.opacity     = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"arrow") == 0)        g_cfg.arrow       = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"bolden") == 0)       g_cfg.bolden      = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"smooth") == 0)       g_cfg.smooth      = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"snap") == 0)         g_cfg.snap        = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"wave") == 0)         g_cfg.wave        = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"wave_h") == 0)       g_cfg.waveH       = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"wave_bold") == 0)    g_cfg.waveBold    = (int)wcstol(v, NULL, 10);
    else if (_wcsicmp(k, L"wave_color") == 0)   g_cfg.waveColor   = ParseColor(v, g_cfg.waveColor);
    else if (_wcsicmp(k, L"wave_type") == 0) {
        if      (_wcsicmp(v, L"ecg")  == 0) g_cfg.waveType = 0;
        else if (_wcsicmp(v, L"ppg")  == 0) g_cfg.waveType = 1;
        else if (_wcsicmp(v, L"resp") == 0) g_cfg.waveType = 2;
        else                                g_cfg.waveType = 0;
    }
    else if (_wcsicmp(k, L"pull") == 0) {               /* 全局默认回归强度(%) */
        int i;
        g_cfg.pullDef = (int)wcstol(v, NULL, 10);
        if (g_cfg.pullDef < 0)  g_cfg.pullDef = 0;
        if (g_cfg.pullDef > 50) g_cfg.pullDef = 50;
        for (i = 0; i < g_cfg.slotCount; i++) g_cfg.slot[i].pull = g_cfg.pullDef;
    }
    else if (_wcsicmp(k, L"line") == 0)         CopyStr(g_cfg.line, v, MAX_LINE);
    else if (_wcsnicmp(k, L"line", 4) == 0 && k[4]) {
        /* line1 / line2 / ... : 分段写模板, 便于阅读; 会自动拼成一行 */
        int    n = (int)wcstol(k + 4, NULL, 10);
        size_t have = wcslen(g_cfg.line);
        size_t add  = wcslen(v);
        if (n <= 1) { g_cfg.line[0] = 0; have = 0; }
        else if (have > 0 && have + 1 < MAX_LINE) {
            g_cfg.line[have++] = L' ';
            g_cfg.line[have]   = 0;
        }
        if (have + add < MAX_LINE) wmemcpy(g_cfg.line + have, v, add + 1);
    }
    else if (_wcsnicmp(k, L"slot.", 5) == 0 && k[5]) {
        Slot *sl = FindSlot(k + 5);
        if (!sl) sl = AddSlot(k + 5);
        if (sl) ParseSpec(sl, v);
    }
    else if (_wcsnicmp(k, L"rate.", 5) == 0 && k[5]) {   /* 该槽多少毫秒动一次 */
        Slot *sl = FindSlot(k + 5);
        if (!sl) sl = AddSlot(k + 5);
        if (sl) {
            sl->rate = (int)wcstol(v, NULL, 10);
            if (sl->rate < 20)    sl->rate = 20;
            if (sl->rate > 600000) sl->rate = 600000;
        }
    }
    else if (_wcsnicmp(k, L"pull.", 5) == 0 && k[5]) {   /* 回归正常区间中心的强度(%) */
        Slot *sl = FindSlot(k + 5);
        if (!sl) sl = AddSlot(k + 5);
        if (sl) {
            sl->pull = (int)wcstol(v, NULL, 10);
            if (sl->pull < 0)  sl->pull = 0;
            if (sl->pull > 50) sl->pull = 50;
        }
    }

    if (g_cfg.size < 6)    g_cfg.size = 6;
    if (g_cfg.size > 200)  g_cfg.size = 200;
    if (g_cfg.bgAlpha < 0) g_cfg.bgAlpha = 0;
    if (g_cfg.bgAlpha > 255) g_cfg.bgAlpha = 255;
    if (g_cfg.bgStyle < 0) g_cfg.bgStyle = 0;
    if (g_cfg.bgStyle > 3) g_cfg.bgStyle = 3;
    if (g_cfg.bgRadius < 0)   g_cfg.bgRadius = 0;
    if (g_cfg.bgRadius > 500) g_cfg.bgRadius = 500;
    if (g_cfg.interval < 30) g_cfg.interval = 30;
    if (g_cfg.letterSpace < 0) g_cfg.letterSpace = 0;
    if (g_cfg.width   < 60)    g_cfg.width   = 60;
    if (g_cfg.width   > 2000)  g_cfg.width   = 2000;
    if (g_cfg.speed   < 0)     g_cfg.speed   = 0;
    if (g_cfg.speed   > 400)   g_cfg.speed   = 400;
    if (g_cfg.gap     < 0)     g_cfg.gap     = 0;
    if (g_cfg.fps     < 10)    g_cfg.fps     = 10;
    if (g_cfg.fps     > 240)   g_cfg.fps     = 240;
    if (g_cfg.opacity < 20)    g_cfg.opacity = 20;
    if (g_cfg.opacity > 255)   g_cfg.opacity = 255;
    if (g_cfg.bolden  < 0)     g_cfg.bolden  = 0;
    if (g_cfg.bolden  > 2)     g_cfg.bolden  = 2;
    if (g_cfg.waveH   < 8)     g_cfg.waveH   = 8;
    if (g_cfg.waveH   > 120)   g_cfg.waveH   = 120;
    if (g_cfg.waveBold < 0)    g_cfg.waveBold = 0;
    if (g_cfg.waveBold > 4)    g_cfg.waveBold = 4;
}

static void GetIniPath(wchar_t *out, int cch)
{
    wchar_t dir[MAX_PATHW];
    CopyStr(dir, L"", MAX_PATHW);
    GetModuleFileNameW(NULL, dir, MAX_PATHW);
    {
        wchar_t *p = wcsrchr(dir, L'\\');
        if (p) *p = 0;
    }
    _snwprintf(out, cch, L"%s\\vitals.ini", dir);
    out[cch - 1] = 0;
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) {
        wchar_t cwd[MAX_PATHW];
        cwd[0] = 0;
        GetCurrentDirectoryW(MAX_PATHW, cwd);
        _snwprintf(out, cch, L"%s\\vitals.ini", cwd);
        out[cch - 1] = 0;
    }
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) {
        /* 最后找 %APPDATA%\vitals.ini (exe 装在 Program Files 之类只读目录时会用到) */
        const wchar_t *ap = _wgetenv(L"APPDATA");
        if (ap && *ap) {
            _snwprintf(out, cch, L"%s\\vitals.ini", ap);
            out[cch - 1] = 0;
        }
    }
}

/* 第一次运行: 把 exe 里内置的默认配置释放成 vitals.ini.
   这样"只发一个 exe"也能用, 不用另外附带 ini.
   注意: 这里不能用 GetIniPath() —— 那是"查找"函数, 会兜底到 %APPDATA%;
        我们要的是"优先写在 exe 旁边" */
static void EnsureDefaultIni(void)
{
    wchar_t  path[MAX_PATHW], dir[MAX_PATHW];
    HRSRC    r;
    HGLOBAL  h;
    const BYTE *p;
    DWORD    sz;
    FILE    *f;

    GetModuleFileNameW(NULL, dir, MAX_PATHW);
    { wchar_t *s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
    _snwprintf(path, MAX_PATHW, L"%s\\vitals.ini", dir);
    path[MAX_PATHW - 1] = 0;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;   /* 已经有了 */

    r = FindResourceW(g_inst, MAKEINTRESOURCEW(IDR_DEFINI), RT_RCDATA);
    if (!r) return;
    h = LoadResource(g_inst, r);
    if (!h) return;
    p  = (const BYTE *)LockResource(h);
    sz = SizeofResource(g_inst, r);
    if (!p || !sz) return;

    f = _wfopen(path, L"wb");
    if (!f) {
        /* exe 目录不可写(比如装在 Program Files) -> 退到 %APPDATA% */
        const wchar_t *ap = _wgetenv(L"APPDATA");
        if (!ap || !*ap) return;
        _snwprintf(path, MAX_PATHW, L"%s\\vitals.ini", ap);
        path[MAX_PATHW - 1] = 0;
        f = _wfopen(path, L"wb");
        if (!f) return;
    }
    fwrite(p, 1, sz, f);
    fclose(f);
}

static void ResolveFonts(void);      /* 定义在字体那一段, 这里先用 */
static void ComputeBoxes(void);      /* 同上, BuildRuns 里要用 */
static int  IsAutoRun(void);         /* 定义在末尾, 菜单要用 */
static void SetAutoRun(int on);
static void ShowDeviceWindow(void);
static void ShowSettingsWindow(void);
static void ToggleHidden(void);
static void TrayAdd(void);
static void TrayDel(void);
static void TrayMenu(void);
static void LoadIcons(void);
static void SaveState(void);
static void IniSetValue(const wchar_t *key, const wchar_t *val);
static void PaintWave(double dt);

static void LoadConfig(void)
{
    wchar_t  path[MAX_PATHW];
    wchar_t *txt;
    wchar_t *s;

    Defaults();
    GetIniPath(path, MAX_PATHW);
    txt = ReadTextFileW(path);
    if (!txt) { ResolveFonts(); return; }

    s = txt;
    while (s && *s) {
        wchar_t *nl = wcschr(s, L'\n');
        wchar_t *line = s;
        wchar_t *eq;
        if (nl) { *nl = 0; s = nl + 1; } else { s = NULL; }

        Trim(line);
        {   /* ';' 起注释. 注意颜色用 #RRGGBB, 所以 # 不是注释符 */
            wchar_t *cm = wcschr(line, L';');
            if (cm) *cm = 0;
        }
        Trim(line);
        if (!line[0]) continue;

        eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        {
            wchar_t *v = eq + 1;
            Trim(line);
            Trim(v);
            ApplyKey(line, v);
        }
    }
    free(txt);
    ResolveFonts();
}

/* ============================================================== 状态记忆 =*/

static void StatePath(wchar_t *out, int cch)
{
    const wchar_t *ap = _wgetenv(L"APPDATA");
    if (ap && *ap) {
        _snwprintf(out, cch, L"%s\\vitals.state", ap);
    } else {
        wchar_t dir[MAX_PATHW];
        dir[0] = 0;
        GetModuleFileNameW(NULL, dir, MAX_PATHW);
        _snwprintf(out, cch, L"%s.state", dir);
    }
    out[cch - 1] = 0;
}

static void SaveState(void)
{
    wchar_t path[MAX_PATHW];
    FILE *f;
    StatePath(path, MAX_PATHW);
    f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"x=%d\ny=%d\nsize=%d\ndev=%d\n",
             g_cfg.posX, g_cfg.posY, g_cfg.size, g_devSel);
    fclose(f);
}

static void LoadState(void)
{
    wchar_t  path[MAX_PATHW];
    wchar_t *txt, *s;
    StatePath(path, MAX_PATHW);
    txt = ReadTextFileW(path);
    if (!txt) return;
    s = txt;
    while (s && *s) {
        wchar_t *nl = wcschr(s, L'\n');
        wchar_t *line = s;
        wchar_t *eq;
        if (nl) { *nl = 0; s = nl + 1; } else { s = NULL; }
        Trim(line);
        eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        {
            wchar_t *k = line, *v = eq + 1;
            Trim(k); Trim(v);
            if      (_wcsicmp(k, L"x") == 0)    g_cfg.posX = (int)wcstol(v, NULL, 10);
            else if (_wcsicmp(k, L"y") == 0)    g_cfg.posY = (int)wcstol(v, NULL, 10);
            else if (_wcsicmp(k, L"size") == 0) g_cfg.size = (int)wcstol(v, NULL, 10);
            else if (_wcsicmp(k, L"dev") == 0)  g_devSel   = (int)wcstol(v, NULL, 10);
        }
    }
    free(txt);
    if (g_cfg.size < 6)   g_cfg.size = 6;
    if (g_cfg.size > 200) g_cfg.size = 200;
}

/* ============================================================== 模板解析 =*/

/* 这一段里有没有中日韩字符 */
static int HasCJK(const wchar_t *s)
{
    for (; *s; s++) {
        if (*s >= 0x2E80) return 1;
    }
    return 0;
}

static void AddLiteral(const wchar_t *t)
{
    size_t n;
    if (g_runCount >= MAX_RUNS) return;
    n = wcslen(t);
    if (n == 0) return;
    if (g_runCount > 0 && !g_run[g_runCount-1].isSlot &&
        !g_run[g_runCount-1].isText &&                    /* @DEVICE@ 段不能被合并, 否则它的文字会被覆盖 */
        g_run[g_runCount-1].cjk == HasCJK(t)) {
        /* 合并到上一段 */
        size_t have = wcslen(g_run[g_runCount-1].text);
        if (have + n < MAX_TXT) {
            wmemcpy(g_run[g_runCount-1].text + have, t, n + 1);
            return;
        }
    }
    g_run[g_runCount].isSlot = 0;
    g_run[g_runCount].isArrow = 0;
    g_run[g_runCount].isText = 0;
    g_run[g_runCount].slot   = -1;
    g_run[g_runCount].cjk    = HasCJK(t);
    g_run[g_runCount].color  = g_cfg.color;
    CopyStr(g_run[g_runCount].text, t, MAX_TXT);
    g_runCount++;
}

static void AddSlotRun(const wchar_t *tok)
{
    wchar_t key[48];
    const wchar_t *bar = wcschr(tok, L'|');
    int n;
    Slot *sl;
    Run  *r;

    n = bar ? (int)(bar - tok) : (int)wcslen(tok);
    if (n > 47) n = 47;
    wcsncpy(key, tok, n);
    key[n] = 0;
    Trim(key);
    if (!key[0]) return;

    /* 特殊段: @DEVICE@ = 当前"连接"的监测设备名 */
    if (_wcsicmp(key, L"DEVICE") == 0 && g_runCount < MAX_RUNS) {
        r = &g_run[g_runCount++];
        r->isSlot  = 0;
        r->isArrow = 0;
        r->isText  = 1;
        r->slot    = -1;
        r->cjk     = 0;
        r->color   = RGB(0xFF, 0xB3, 0x00);
        r->text[0] = 0;
        return;
    }

    sl = FindSlot(key);
    if (!sl) {
        sl = AddSlot(key);
        if (bar) ParseSpec(sl, bar + 1);
    }
    if (g_runCount >= MAX_RUNS) return;

    r = &g_run[g_runCount++];
    r->isSlot = 1;
    r->isArrow = 0;
    r->isText  = 0;
    r->cjk    = 0;
    r->slot   = (int)(sl - g_cfg.slot);
    r->color  = sl->color;
    r->text[0] = 0;

    /* 紧跟一个"箭头格子": 不管有没有箭头, 宽度都固定, 所以整行不会忽宽忽窄 */
    if (g_cfg.arrow && g_runCount < MAX_RUNS) {
        r = &g_run[g_runCount++];
        r->isSlot  = 1;
        r->isArrow = 1;
        r->isText  = 0;
        r->cjk     = 0;
        r->slot    = (int)(sl - g_cfg.slot);
        r->color   = sl->color;
        r->text[0] = 0;
    }
}

static void BuildRuns(void)
{
    const wchar_t *p = g_cfg.line;
    g_runCount = 0;
    while (*p && g_runCount < MAX_RUNS) {
        if (p[0] == L'@' && p[1] == L'@') {           /* @@ = 字面 @ */
            AddLiteral(L"@");
            p += 2;
            continue;
        }
        if (*p == L'@') {
            const wchar_t *e = wcschr(p + 1, L'@');
            if (e) {
                wchar_t tok[MAX_TXT];
                int n = (int)(e - p - 1);
                if (n > MAX_TXT - 1) n = MAX_TXT - 1;
                wcsncpy(tok, p + 1, n);
                tok[n] = 0;
                AddSlotRun(tok);
                p = e + 1;
                continue;
            }
        }
        {   /* 一段字面文本 */
            const wchar_t *e = wcschr(p, L'@');
            wchar_t buf[MAX_TXT];
            int n;
            if (!e) e = p + wcslen(p);
            n = (int)(e - p);
            if (n > MAX_TXT - 1) n = MAX_TXT - 1;
            wcsncpy(buf, p, n);
            buf[n] = 0;
            AddLiteral(buf);
            p = e;
        }
    }
    ComputeBoxes();   /* 模板里可能有没定义过的槽, 刚被自动建出来, 这里补算格子宽 */
}

/* ================================================================ 数值 ====*/

/* 每个槽有自己的刷新节奏: 心率快, 肝功/甲功几乎不动.
   返回 1 表示这次真的有槽动过 (没动就不用重画) */
static int UpdateValues(void)
{
    ULONGLONG now = GetTickCount64();
    int i, changed = 0;

    for (i = 0; i < g_cfg.slotCount; i++) {
        Slot *sl = &g_cfg.slot[i];
        int   r  = (sl->rate > 0) ? sl->rate : 1000;

        if (sl->last != 0 && (now - sl->last) < (ULONGLONG)r) continue;
        sl->last = now ? now : 1;

        switch (sl->mode) {
        case 0:
            sl->value = sl->lo;
            break;
        case 1:
            sl->value = sl->lo + Rnd01() * (sl->hi - sl->lo);
            break;
        default: {
            double d = (Rnd01() * 2.0 - 1.0) * sl->step;
            double mid = sl->useAlert ? (sl->alertLo + sl->alertHi) * 0.5
                                      : (sl->lo + sl->hi) * 0.5;
            d += (mid - sl->value) * ((double)sl->pull / 100.0);   /* 往正常区中心拉一点, 更像真人 */
            sl->value += d;
            if (sl->value < sl->lo) sl->value = sl->lo + (sl->lo - sl->value);
            if (sl->value > sl->hi) sl->value = sl->hi - (sl->value - sl->hi);
            if (sl->value < sl->lo) sl->value = sl->lo;
            if (sl->value > sl->hi) sl->value = sl->hi;
            break;
        }
        }
        changed = 1;
    }
    return changed;
}

/* 字体建好后, 给每个槽算出"它可能写出的最宽像素数"当固定格子.
   这样管你什么字体(等宽不等宽), 整行宽度都恒定, 滚动永远不抖 */
static void ComputeBoxes(void)
{
    int i;
    if (!g_memDC) return;
    SelectObject(g_memDC, g_font);
    {   /* 设备名格子宽 = 所有设备名里最宽的那个 */
        int d;
        g_devW = 0;
        for (d = 0; d < g_devCount; d++) {
            wchar_t b[160];
            SIZE s;
            DevDisplayName(d, b, 160);
            GetTextExtentPoint32W(g_memDC, b, (int)wcslen(b), &s);
            if (s.cx > g_devW) g_devW = s.cx;
        }
    }
    for (i = 0; i < g_cfg.slotCount; i++) {
        Slot *sl = &g_cfg.slot[i];
        wchar_t b[64];
        SIZE s;
        int w1, w2;
        if (sl->decimals > 0) swprintf(b, 64, L"%.*f", sl->decimals, sl->hi);
        else                  swprintf(b, 64, L"%.0f", sl->hi);
        GetTextExtentPoint32W(g_memDC, b, (int)wcslen(b), &s);
        w1 = s.cx;
        if (sl->decimals > 0) swprintf(b, 64, L"%.*f", sl->decimals, sl->lo);
        else                  swprintf(b, 64, L"%.0f", sl->lo);
        GetTextExtentPoint32W(g_memDC, b, (int)wcslen(b), &s);
        w2 = s.cx;
        sl->boxW = (w1 > w2) ? w1 : w2;
        if (sl->boxW < 1) sl->boxW = 1;
        if (sl->boxW > 400) sl->boxW = 400;

        {   /* 箭头格子宽度 = ↑ / ↓ 里较宽的那个, 固定不变 */
            wchar_t up[2], dn[2];
            SIZE a, b;
            up[0] = (wchar_t)0x2191; up[1] = 0;
            dn[0] = (wchar_t)0x2193; dn[1] = 0;
            GetTextExtentPoint32W(g_memDC, up, 1, &a);
            GetTextExtentPoint32W(g_memDC, dn, 1, &b);
            sl->arrowW = (a.cx > b.cx) ? a.cx : b.cx;
            if (sl->arrowW < 4) sl->arrowW = 4;
        }
    }
}

static void ComposeTexts(void)
{
    int i, x = 0;
    for (i = 0; i < g_runCount; i++) {
        Run *r = &g_run[i];
        if (r->isText) {                       /* @DEVICE@ */
            DevDisplayName(g_devSel, r->text, MAX_TXT);
            r->x = x;
            x += r->w + g_cfg.letterSpace;
            continue;
        }
        if (!r->isSlot) {
            r->x = x;
            x += r->w + g_cfg.letterSpace;
            continue;
        }
        {
            Slot *sl = &g_cfg.slot[r->slot];
            if (r->isArrow) {
                int hi = (sl->useAlert && sl->value > sl->alertHi);
                int lo = (sl->useAlert && sl->value < sl->alertLo);
                if (hi)      { r->text[0] = (wchar_t)0x2191; r->text[1] = 0; }
                else if (lo) { r->text[0] = (wchar_t)0x2193; r->text[1] = 0; }
                else           r->text[0] = 0;
                r->color = (hi || lo) ? sl->alertColor : sl->color;
            } else {
                if (sl->decimals > 0)
                    swprintf(r->text, MAX_TXT, L"%.*f", sl->decimals, sl->value);
                else
                    swprintf(r->text, MAX_TXT, L"%.0f", sl->value);
                r->color = sl->color;
                if (sl->useAlert &&
                    (sl->value < sl->alertLo || sl->value > sl->alertHi))
                    r->color = sl->alertColor;
            }
        }
        r->x = x;
        x += r->w + g_cfg.letterSpace;
    }
}

/* ================================================================= 字体 ==*/

/* 这个字体族里有没有汉字字形 (没有的话中文会变成方块) */
static int FontHasCJK(const wchar_t *face)
{
    HDC dc = GetDC(NULL);
    HFONT f;
    HGDIOBJ old;
    WORD  idx = 0;
    int   has = 0;

    if (!dc || !face || !face[0]) return 0;
    f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, FF_DONTCARE, face);
    old = SelectObject(dc, f);
    if (GetGlyphIndicesW(dc, L"生", 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR)
        has = (idx != 0xFFFF);
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(NULL, dc);
    return has;
}

/* 决定汉字用哪个字体:
     1) 用户显式写了 font_cjk  -> 用它
     2) 主字体自带中文         -> 一个字体通吃(中英才统一, 也是推荐做法)
     3) 主字体没中文, 又没指定  -> 自动兜底一个中文字体, 免得变方块 */
static void ResolveFonts(void)
{
    static const wchar_t *fb[] = { L"DengXian", L"SimHei", L"SimSun",
                                   L"Microsoft YaHei", NULL };
    int i;

    if (g_cfg.fontCjk[0]) return;
    if (FontHasCJK(g_cfg.font)) return;
    for (i = 0; fb[i]; i++) {
        if (FontHasCJK(fb[i])) { CopyStr(g_cfg.fontCjk, fb[i], 64); return; }
    }
}

/* 段间用不用中文字体 */
static int UseCjkFace(const Run *r)
{
    return (r->cjk && g_cfg.fontCjk[0]) ? 1 : 0;
}

static int ScreenDpi(void)
{
    HDC dc = GetDC(NULL);
    int d  = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d ? d : 96;
}

static void RebuildFont(void)
{
    int px = MulDiv(g_cfg.size, ScreenDpi(), 96);
    HFONT nf, nc;
    TEXTMETRICW tm, tm2;

    if (px < 6)   px = 6;
    if (px > 400) px = 400;

    nf = CreateFontW(-px, 0, 0, 0, g_cfg.weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                     ANTIALIASED_QUALITY, FF_DONTCARE, g_cfg.font);
    if (!nf) nf = (HFONT)GetStockObject(SYSTEM_FONT);

    /* font_cjk 留空 = 和主字体同一个 -> 中英粗细/高度/风格完全一致.
       只有主字体不含汉字时才需要单独指定 */
    nc = CreateFontW(-px, 0, 0, 0, g_cfg.weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                     ANTIALIASED_QUALITY, FF_DONTCARE,
                     g_cfg.fontCjk[0] ? g_cfg.fontCjk : g_cfg.font);
    if (!nc) nc = nf;

    if (g_memDC) SelectObject(g_memDC, nf);
    if (g_scrDC) SelectObject(g_scrDC, nf);
    if (g_font && g_font != (HFONT)GetStockObject(SYSTEM_FONT))
        DeleteObject(g_font);
    if (g_fontCJK && g_fontCJK != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(g_fontCJK);
    g_font    = nf;
    g_fontCJK = nc;

    memset(&tm, 0, sizeof(tm));
    memset(&tm2, 0, sizeof(tm2));
    if (g_memDC) {
        GetTextMetricsW(g_memDC, &tm);
        SelectObject(g_memDC, nc);
        GetTextMetricsW(g_memDC, &tm2);
        SelectObject(g_memDC, nf);
    }
    {
        int h1 = tm.tmHeight  > 0 ? tm.tmHeight  : px;
        int h2 = tm2.tmHeight > 0 ? tm2.tmHeight : px;
        g_cellH = (h1 > h2 ? h1 : h2) + 2;      /* 行高取两种字体的较大者 */
    }
    ComputeBoxes();
}

static void Layout(void)
{
    int i, x = 0;
    int oldPeriod = g_period;
    if (!g_memDC) return;
    for (i = 0; i < g_runCount; i++) {
        SIZE sz;
        Run *r = &g_run[i];
        sz.cx = 0; sz.cy = 0;
        if (r->text[0]) {
            if (UseCjkFace(r)) SelectObject(g_memDC, g_fontCJK);
            else               SelectObject(g_memDC, g_font);
            GetTextExtentPoint32W(g_memDC, r->text, (int)wcslen(r->text), &sz);
            SelectObject(g_memDC, g_font);
        }
        r->tw = sz.cx;
        if (r->isText) {
            r->w = (g_devW > sz.cx) ? g_devW : sz.cx;   /* 设备名固定格子 */
        } else if (r->isArrow) {
            r->w = g_cfg.slot[r->slot].arrowW;   /* 固定格: 有没有箭头都不改变宽度 */
        } else {
            r->w = sz.cx;
            if (r->isSlot && g_cfg.align) {
                int bw = g_cfg.slot[r->slot].boxW;
                if (bw > r->w) r->w = bw;        /* 固定格子, 宽度不随数字位数变 */
            }
        }
        r->x = x;
        x += r->w + g_cfg.letterSpace;
    }
    g_totalW = x - (g_runCount ? g_cfg.letterSpace : 0);
    if (g_totalW < 1) g_totalW = 1;

    g_lineW = g_totalW + PADX * 2;
    g_textH = g_cellH + PADY * 2;
    g_lineH = g_textH + (g_cfg.wave ? (g_cfg.waveH + 2) : 0);
    if (g_lineH > BUF_H - 2) g_lineH = BUF_H - 2;

    /* 行缓冲装的是"一整圈": 文字 + 空隙, 所以取景时纯 memcpy 就够了 */
    g_period = g_lineW + g_cfg.gap;
    if (g_period < 1) g_period = 1;
    if (g_period > BUF_W - 2) g_period = BUF_W - 2;
    if (g_lineW  > g_period)  g_lineW  = g_period;

    if (g_cfg.scroll) {
        g_winW = g_cfg.width;              /* 固定小窗口, 文字在里面滚 */
        if (g_winW > g_period) g_winW = g_period;
    } else {
        g_winW = g_lineW;                  /* 不滚动就整行铺开 */
    }
    if (g_winW < 20)        g_winW = 20;
    if (g_winW > g_period)  g_winW = g_period;
    g_winH = g_lineH;

    /* 波形横向过采样: 采样点比像素密 N 倍, 这样波形也能平滑移动.
       注意不能用"纵向插值"来平滑 —— 1 像素宽的 R 尖峰会被削掉幅度,
       而插值系数每帧都在变, 看起来就是尖峰在蹦迪. 过采样 + 逐列峰值保持才对. */
    g_waveOS = WAVE_OS;
    while (g_waveOS > 1 && g_winW * g_waveOS > BUF_W - 8) g_waveOS--;
    g_waveN  = g_winW * g_waveOS;
    if (g_waveN < 2) g_waveN = 2;
    /* 整行宽度变了(比如配置里改了内容), 按比例换算滚动相位, 避免画面跳一下 */
    if (oldPeriod > 1 && g_period != oldPeriod) {
        g_scroll = g_scroll * (double)g_period / (double)oldPeriod;
        if (g_scroll < 0.0) g_scroll = 0.0;
    }
    if (g_scroll >= (double)g_period) g_scroll = 0.0;
}

/* 如果整行比屏幕还宽, 就自动缩小字号直到塞得下.
   只在启动/重载配置/手工改字号时调用, 不会每帧乱跳. */
static void FitFont(void)
{
    RECT wa;
    int  limit, guard;

    if (!g_cfg.autofit || g_cfg.scroll) return;   /* 滚动模式不需要塞进屏幕 */

    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0; wa.top = 0;
        wa.right  = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    limit = (wa.right - wa.left) - 16;      /* 左右各留一点余量 */
    if (limit < 60) limit = 60;

    for (guard = 0; guard < 12; guard++) {
        int ns;
        Layout();
        if (g_winW <= limit) break;
        if (g_cfg.size <= 6) break;
        ns = (int)((double)g_cfg.size * (double)limit / (double)g_winW);
        if (ns >= g_cfg.size) ns = g_cfg.size - 1;
        if (ns < 6) ns = 6;
        g_cfg.size = ns;
        RebuildFont();
    }
    Layout();
}

/* ============================================================== 离屏缓冲 =*/

static HBITMAP MakeDib(int w, int h, void **bits)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;              /* 负数 = 自上而下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    *bits = NULL;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

static void InitBuffers(void)
{
    g_memDC  = CreateCompatibleDC(NULL);
    g_scrDC  = CreateCompatibleDC(NULL);
    if (!g_screenDC) {
        g_screenDC = CreateDCW(L"DISPLAY", NULL, NULL, NULL);   /* 只建一次 */
        if (!g_screenDC) g_screenDC = GetDC(NULL);
    }
    g_memBmp  = MakeDib(BUF_W, BUF_H, &g_memBits);
    g_scrBmp  = MakeDib(BUF_W, BUF_H, &g_scrBits);
    g_lineBmp = MakeDib(BUF_W, BUF_H, &g_lineBits);
    if (g_memDC && g_memBmp) SelectObject(g_memDC, g_memBmp);
    if (g_scrDC && g_scrBmp) SelectObject(g_scrDC, g_scrBmp);
    if (g_scrDC) SetBkMode(g_scrDC, TRANSPARENT);
    if (g_font) {
        if (g_memDC) SelectObject(g_memDC, g_font);
        if (g_scrDC) SelectObject(g_scrDC, g_font);
    }
}

/* ================================================================== 绘制 =*/

static DWORD PackPremul(COLORREF c, int a)
{
    int r, g, b;
    if (a < 0)   a = 0;
    if (a > 255) a = 255;
    r = GetRValue(c) * a / 255;
    g = GetGValue(c) * a / 255;
    b = GetBValue(c) * a / 255;
    return ((DWORD)b) | ((DWORD)g << 8) | ((DWORD)r << 16) | ((DWORD)a << 24);
}

static void Blend(DWORD *dst, int cr, int cg, int cb, int a)
{
    DWORD d  = *dst;
    int   db = (int)( d        & 0xFF);
    int   dg = (int)((d >>  8) & 0xFF);
    int   dr = (int)((d >> 16) & 0xFF);
    int   da = (int)((d >> 24) & 0xFF);
    int   inv = 255 - a;
    int   sr = cr * a / 255;
    int   sg = cg * a / 255;
    int   sb = cb * a / 255;
    int   ob = sb + db * inv / 255;
    int   og = sg + dg * inv / 255;
    int   or_ = sr + dr * inv / 255;
    int   oa = a + da * inv / 255;
    if (ob > 255) ob = 255;
    if (og > 255) og = 255;
    if (or_ > 255) or_ = 255;
    if (oa > 255) oa = 255;
    *dst = ((DWORD)ob) | ((DWORD)og << 8) | ((DWORD)or_ << 16) | ((DWORD)oa << 24);
}

static void ClearScratch(int w)
{
    int y, x, y0, y1;
    if (!g_scrBits) return;
    if (w > BUF_W) w = BUF_W;
    y0 = PADY - 1; if (y0 < 0) y0 = 0;
    y1 = PADY + g_cellH + 1; if (y1 > BUF_H) y1 = BUF_H;
    for (y = y0; y < y1; y++) {
        DWORD *row = (DWORD *)((BYTE *)g_scrBits + (size_t)y * BUF_W * 4);
        for (x = 0; x < w; x++) row[x] = 0xFF000000u;   /* 不透明黑 */
    }
}

/* 把 scratch 里的白色字形按 col 染色, 叠加到背缓冲的 (x,y) */
static void CompositeRun(const Run *r, int dx, COLORREF col, int aScale)
{
    int bx = PADX + r->x + dx;
    int by = PADY;
    int cr = GetRValue(col), cg = GetGValue(col), cb = GetBValue(col);
    int sx, sy;

    if (!g_lineBits || !g_scrBits) return;
    for (sy = 0; sy < g_cellH; sy++) {
        int dy = by + sy;
        DWORD *drow, *srow;
        if (dy < 0 || dy >= g_lineH || dy >= BUF_H) continue;
        drow = (DWORD *)((BYTE *)g_lineBits + (size_t)dy * BUF_W * 4);
        srow = (DWORD *)((BYTE *)g_scrBits + (size_t)dy * BUF_W * 4);
        for (sx = 0; sx < r->w; sx++) {
            int v, px = bx + sx;
            if (px < 0 || px >= g_period || sx >= BUF_W) continue;
            v = (int)((srow[sx] >> 16) & 0xFF);          /* 白色字形的覆盖率 */
            if (!v) continue;
            v = v * aScale / 255;
            if (!v) continue;
            Blend(&drow[px], cr, cg, cb, v);
        }
    }
}

/* ==================================================== 背景(圆角/毛玻璃) ====*/

static int InsideRoundRect(double px, double py, int w, int h, int r)
{
    double cx, cy, dx, dy;
    if (r <= 0) return (px >= 0 && py >= 0 && px < w && py < h);
    if (px < r && py < r)                   { cx = r;     cy = r;     }
    else if (px >= w - r && py < r)         { cx = w - r; cy = r;     }
    else if (px < r && py >= h - r)         { cx = r;     cy = h - r; }
    else if (px >= w - r && py >= h - r)    { cx = w - r; cy = h - r; }
    else return 1;
    dx = px - cx; dy = py - cy;
    return (dx * dx + dy * dy) <= (double)r * (double)r;
}

/* 圆角覆盖度图: 4x4 超采样 -> 边缘天然抗锯齿.
   半径会被钳到 高/2, 所以给个大值就是"四分之一个圆"的胶囊两端 */
static void EnsureMask(int w, int h, int rad)
{
    int x, y, sx, sy;
    if (w < 1 || h < 1) return;
    if (g_mask && g_maskW == w && g_maskH == h && g_maskR == rad) return;
    if (g_mask) { free(g_mask); g_mask = NULL; }
    g_mask = (BYTE *)malloc((size_t)w * (size_t)h);
    if (!g_mask) return;
    g_maskW = w; g_maskH = h; g_maskR = rad;
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int cov = 0;
            for (sy = 0; sy < 4; sy++) {
                for (sx = 0; sx < 4; sx++) {
                    double px = (double)x + ((double)sx + 0.5) / 4.0;
                    double py = (double)y + ((double)sy + 0.5) / 4.0;
                    if (InsideRoundRect(px, py, w, h, rad)) cov++;
                }
            }
            g_mask[(size_t)y * w + x] = (BYTE)(cov * 255 / 16);
        }
    }
}

static HBITMAP MakeDib(int w, int h, void **bits);

static void EnsureBackdrop(int w, int h)
{
    if (g_bdBmp && g_bdW == w && g_bdH == h) return;
    if (g_bdDC)  { DeleteDC(g_bdDC);   g_bdDC = NULL; }
    if (g_bdBmp) { DeleteObject(g_bdBmp); g_bdBmp = NULL; }
    g_bdW = g_bdH = 0;
    g_bdBmp = MakeDib(w, h, &g_bdBits);
    if (!g_bdBmp) return;
    g_bdDC = CreateCompatibleDC(NULL);
    if (g_bdDC) SelectObject(g_bdDC, g_bdBmp);
    g_bdW = w; g_bdH = h;
    g_bdValid = 0;
}

/* 两趟盒式模糊 (可分离), 只对抓来的那块小图做, 很便宜 */
static void BlurBGRA(void *bits, int w, int h, int rad)
{
    BYTE *tmp;
    int   stride = w * 4, x, y, k;
    if (rad < 1 || w < 2 || h < 2) return;
    tmp = (BYTE *)malloc((size_t)stride * (size_t)h);
    if (!tmp) return;
    for (y = 0; y < h; y++) {                       /* 横向 */
        BYTE *src = (BYTE *)bits + (size_t)y * stride;
        BYTE *dst = tmp + (size_t)y * stride;
        for (x = 0; x < w; x++) {
            int s0 = 0, s1 = 0, s2 = 0, n = 0;
            for (k = -rad; k <= rad; k++) {
                int xx = x + k;
                if (xx < 0) xx = 0;
                if (xx >= w) xx = w - 1;
                s0 += src[xx * 4 + 0]; s1 += src[xx * 4 + 1]; s2 += src[xx * 4 + 2];
                n++;
            }
            dst[x * 4 + 0] = (BYTE)(s0 / n);
            dst[x * 4 + 1] = (BYTE)(s1 / n);
            dst[x * 4 + 2] = (BYTE)(s2 / n);
            dst[x * 4 + 3] = 255;
        }
    }
    for (x = 0; x < w; x++) {                       /* 纵向 */
        for (y = 0; y < h; y++) {
            int s0 = 0, s1 = 0, s2 = 0, n = 0;
            for (k = -rad; k <= rad; k++) {
                int yy = y + k;
                if (yy < 0) yy = 0;
                if (yy >= h) yy = h - 1;
                s0 += tmp[yy * stride + x * 4 + 0];
                s1 += tmp[yy * stride + x * 4 + 1];
                s2 += tmp[yy * stride + x * 4 + 2];
                n++;
            }
            ((BYTE *)bits)[y * stride + x * 4 + 0] = (BYTE)(s0 / n);
            ((BYTE *)bits)[y * stride + x * 4 + 1] = (BYTE)(s1 / n);
            ((BYTE *)bits)[y * stride + x * 4 + 2] = (BYTE)(s2 / n);
            ((BYTE *)bits)[y * stride + x * 4 + 3] = 255;
        }
    }
    free(tmp);
}

/* 毛玻璃: 抓窗口后面的桌面像素.
   问题是窗口自己盖在上面, 直接 BitBlt 会抓到自己 ——
   所以先把自己藏起来, 抓完立刻显示回来. 三步都在同一帧内(<1ms), 不会看到闪 */
static void RefreshBackdrop(void)
{
    if (g_cfg.bgStyle != 2 || !g_hwnd || !g_bdDC || g_winW < 2 || g_winH < 2) return;
    SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                 SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    DwmFlush();          /* 关键: 等 DWM 真的把我合成掉, 否则 BitBlt 抓到的是自己/黑屏 */
    if (g_screenDC)
        BitBlt(g_bdDC, 0, 0, g_winW, g_winH, g_screenDC, g_cfg.posX, g_cfg.posY, SRCCOPY);
    SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                 SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    BlurBGRA(g_bdBits, g_bdW, g_bdH, 7);
    /* 兜底: 万一抓到的是一片黑(抓失败), 就退回黑灰渐变, 免得出现一条黑条 */
    {
        int i, n = g_bdW * g_bdH, sum = 0;
        BYTE *p = (BYTE *)g_bdBits;
        for (i = 0; i < n; i += 7)                        /* 抽样就够了 */
            sum += p[i * 4 + 0] + p[i * 4 + 1] + p[i * 4 + 2];
        g_bdValid = (n > 0 && (sum / (n / 7 + 1)) > 24) ? 1 : 0;
    }
    g_bdTime  = GetTickCount64();
}

/* 把背景铺进窗口缓冲(在文字之前) */
static void PaintBackdrop(void)
{
    int x, y, alpha;
    if (g_cfg.bgStyle == 0 || !g_memBits) return;
    alpha = g_cfg.bgAlpha;
    if (alpha < 10) {                    /* 没设透明度时给每种风格一个合适的默认值, 免得选了却看不见 */
        alpha = (g_cfg.bgStyle == 1) ? 235 : (g_cfg.bgStyle == 2) ? 220 : 150;
    }

    for (y = 0; y < g_winH; y++) {
        DWORD *row = (DWORD *)((BYTE *)g_memBits + (size_t)y * BUF_W * 4);
        int   t  = (g_winH > 1) ? (y * 255 / (g_winH - 1)) : 0;
        int   ir, ig, ib;                            /* 通用渐变(风格1/3, 也是风格2的兜底) */
        if (g_cfg.bgStyle == 3) {                    /* 通透玻璃: 冷色, 上亮下暗 */
            ir = 0x6E + ((0x22 - 0x6E) * t / 255);
            ig = 0x86 + ((0x2A - 0x86) * t / 255);
            ib = 0x9A + ((0x36 - 0x9A) * t / 255);
        } else {                                     /* 黑灰渐变 (毛玻璃兜底也是它) */
            ir = 0x2A + ((0x0B - 0x2A) * t / 255);
            ig = 0x2E + ((0x0D - 0x2E) * t / 255);
            ib = 0x34 + ((0x10 - 0x34) * t / 255);
        }
        for (x = 0; x < g_winW; x++) {
            int cr = ir, cg = ig, cb = ib;
            if (g_cfg.bgStyle == 2 && g_bdValid && g_bdBits && g_bdW == g_winW && g_bdH == g_winH) {
                BYTE *p = (BYTE *)g_bdBits + ((size_t)y * g_winW + x) * 4;
                cr = p[2] * 55 / 100;                /* 压暗, 保证字读得清 */
                cg = p[1] * 55 / 100;
                cb = p[0] * 55 / 100;
            }
            row[x] = PackPremul(RGB(cr, cg, cb), alpha);
        }
    }
}

/* 最后统一套圆角遮罩: 连文字一起裁成胶囊形(滚动内容进入圆角处就该被裁掉) */
static void ApplyRoundedMask(void)
{
    int x, y;
    if (g_cfg.bgStyle == 0 || !g_memBits) return;
    EnsureMask(g_winW, g_winH, g_cfg.bgRadius);
    if (!g_mask) return;
    for (y = 0; y < g_winH; y++) {
        DWORD *row = (DWORD *)((BYTE *)g_memBits + (size_t)y * BUF_W * 4);
        BYTE  *m   = g_mask + (size_t)y * g_maskW;
        for (x = 0; x < g_winW; x++) {
            int cov = m[x];
            DWORD p;
            if (cov == 255) continue;
            if (cov == 0) { row[x] = 0; continue; }
            p = row[x];
            row[x] = ((DWORD)(((p >> 24) & 0xFF) * cov / 255) << 24)
                   | ((DWORD)(((p >> 16) & 0xFF) * cov / 255) << 16)
                   | ((DWORD)(((p >>  8) & 0xFF) * cov / 255) <<  8)
                   |  (DWORD)(( p        & 0xFF) * cov / 255);
        }
    }
}

/* 1) 把整行(所有段)画进"行缓冲" g_lineBits, 宽度 g_lineW */
static void PaintLine(void)
{
    DWORD bg;
    int   alpha = g_cfg.bgAlpha;
    int   i, x, y;

    if (!g_lineBits || !g_scrBits) return;
    if (g_cfg.grabFull && alpha < 1) alpha = 1;   /* 几乎不可见但可点击 */
    if (g_cfg.bgStyle != 0) alpha = 0;            /* 有背景样式时, 行缓冲只装文字(透明底) */
    bg = PackPremul(g_cfg.bg, alpha);

    for (y = 0; y < g_lineH; y++) {
        DWORD *row = (DWORD *)((BYTE *)g_lineBits + (size_t)y * BUF_W * 4);
        for (x = 0; x < g_period; x++) row[x] = bg;
    }

    for (i = 0; i < g_runCount; i++) {
        Run *r = &g_run[i];
        if (r->w <= 0 || !r->text[0]) continue;

        ClearScratch(r->w);
        if (UseCjkFace(r)) SelectObject(g_scrDC, g_fontCJK);
        else               SelectObject(g_scrDC, g_font);
        SetTextColor(g_scrDC, RGB(255, 255, 255));
        {
            int tx = 0;
            if (r->w > r->tw) {
                if (r->isArrow)     tx = (r->w - r->tw) / 2;            /* 箭头居中 */
                else if (r->isSlot) tx = r->w - r->tw - g_cfg.bolden;   /* 数字右对齐 */
            }
            if (tx < 0) tx = 0;
            TextOutW(g_scrDC, tx, PADY, r->text, (int)wcslen(r->text));
        }

        if (g_cfg.glitch) {
            CompositeRun(r, -1, RGB(0xFF, 0x20, 0x40), 110);
            CompositeRun(r,  1, RGB(0x00, 0xE5, 0xFF), 110);
        }
        CompositeRun(r, 0, r->color, g_cfg.opacity);
        if (g_cfg.bolden > 0 && r->tw > 0) {   /* 往右补画几遍, 笔画就粗了 */
            int k;
            for (k = 1; k <= g_cfg.bolden; k++)
                CompositeRun(r, k, r->color, g_cfg.opacity);
        }
    }
}

/* 两个预乘像素按权重线性插值 (w: 0..255). 预乘空间里线性插值是正确的 */
static DWORD Blend2(DWORD p, DWORD q, int w)
{
    int iw = 256 - w;
    DWORD r = (((((p >> 16) & 0xFF) * iw + ((q >> 16) & 0xFF) * w) >> 8) << 16);
    DWORD g = (((((p >>  8) & 0xFF) * iw + ((q >>  8) & 0xFF) * w) >> 8) <<  8);
    DWORD b =  ((((p & 0xFF) * iw + (q & 0xFF) * w) >> 8));
    DWORD a = (((((p >> 24) & 0xFF) * iw + ((q >> 24) & 0xFF) * w) >> 8) << 24);
    return r | g | b | a;
}

/* 预乘源像素 over 预乘目标像素 (有背景样式时需要把文字叠在背景上, 不能直接覆盖) */
static void BlendPx(DWORD *dst, DWORD src)
{
    int sa = (int)((src >> 24) & 0xFF);
    int inv, sb, sg, sr, d, db, dg, dr, da;
    if (sa == 0) return;
    if (sa == 255) { *dst = src; return; }
    inv = 255 - sa;
    sb = (int)( src        & 0xFF);
    sg = (int)((src >>  8) & 0xFF);
    sr = (int)((src >> 16) & 0xFF);
    d  = (int)*dst;
    db = (int)( d        & 0xFF);
    dg = (int)((d >>  8) & 0xFF);
    dr = (int)((d >> 16) & 0xFF);
    da = (int)((d >> 24) & 0xFF);
    db = sb + db * inv / 255; if (db > 255) db = 255;
    dg = sg + dg * inv / 255; if (dg > 255) dg = 255;
    dr = sr + dr * inv / 255; if (dr > 255) dr = 255;
    da = sa + da * inv / 255; if (da > 255) da = 255;
    *dst = ((DWORD)db) | ((DWORD)dg << 8) | ((DWORD)dr << 16) | ((DWORD)da << 24);
}

/* 2) 从行缓冲里按滚动偏移"取景"到窗口缓冲 g_memBits.
      整周期都在行缓冲里, 所以边界处理很简单.
      开 smooth 时做亚像素插值: 否则 0.92 像素/帧这种速度会"每 12 帧停一下", 看着就是顿挫. */
static void PaintWindow(void)
{
    int    y, sx, period, frac;
    double fl;

    if (!g_memBits || !g_lineBits) return;

    period = g_period;
    if (period < 1) period = 1;

    fl = g_scroll;
    sx = (int)fl;                              /* 向下取整 */
    if (sx < 0) sx = 0;
    while (sx >= period) sx -= period;
    frac = (int)((fl - (double)(int)fl) * 256.0);
    if (frac < 0)   frac = 0;
    if (frac > 255) frac = 255;
    if (!g_cfg.smooth) frac = 0;

    for (y = 0; y < g_winH; y++) {
        DWORD *dst = (DWORD *)((BYTE *)g_memBits + (size_t)y * BUF_W * 4);
        DWORD *src = (DWORD *)((BYTE *)g_lineBits + (size_t)y * BUF_W * 4);
        int   n1  = period - sx;
        int   x;

        if (n1 > g_winW) n1 = g_winW;
        if (frac == 0 && g_cfg.bgStyle == 0) {   /* 无背景样式: 直接覆盖, 最快 */
            memcpy(dst, src + sx, (size_t)n1 * 4);
            if (n1 < g_winW)
                memcpy(dst + n1, src, (size_t)(g_winW - n1) * 4);
        } else if (g_cfg.bgStyle == 0) {
            for (x = 0; x < g_winW; x++) {
                int i0 = sx + x;  if (i0 >= period) i0 -= period;
                int i1 = i0 + 1;  if (i1 >= period) i1 -= period;
                dst[x] = Blend2(src[i0], src[i1], frac);
            }
        } else {                                 /* 有背景: 文字要叠在背景上 */
            for (x = 0; x < g_winW; x++) {
                int i0 = sx + x;  if (i0 >= period) i0 -= period;
                DWORD s;
                if (frac == 0) {
                    s = src[i0];
                } else {
                    int i1 = i0 + 1;  if (i1 >= period) i1 -= period;
                    s = Blend2(src[i0], src[i1], frac);
                }
                BlendPx(&dst[x], s);
            }
        }
    }
}

static void PaintWave(double dt);   /* 定义在下面, PaintAll 要用 */
static int  SnapStep(double spd, double dt);

static void PaintAll(void)
{
    PaintBackdrop();
    PaintLine();
    PaintWindow();
    if (g_cfg.wave) PaintWave(0.0);   /* dt=0: 只重画不推进 */
    ApplyRoundedMask();
}

/* ---------------------------------------------------------- 实时波形 ----*/

/* 归一化形状函数, 相位 t 在 [0,1) */
static double WaveShape(double t)
{
    if (g_cfg.waveType == 1) {                 /* PPG 脉搏波 */
        double v = 0;
        v += 1.00 * exp(-pow((t - 0.18) / 0.055, 2.0));
        v += 0.35 * exp(-pow((t - 0.42) / 0.090, 2.0));
        return v;
    }
    if (g_cfg.waveType == 2)                   /* 呼吸波 */
        return 0.5 + 0.5 * sin(t * 6.283185307179586);
    {                                          /* ECG: P-QRS-T */
        double v = 0;
        v +=  0.12 * exp(-pow((t - 0.100) / 0.022, 2.0));   /* P */
        v += -0.10 * exp(-pow((t - 0.255) / 0.008, 2.0));   /* Q */
        v +=  1.00 * exp(-pow((t - 0.285) / 0.009, 2.0));   /* R */
        v += -0.28 * exp(-pow((t - 0.315) / 0.011, 2.0));   /* S */
        v +=  0.30 * exp(-pow((t - 0.460) / 0.032, 2.0));   /* T */
        return v;
    }
}

/* 推进相位并返回一个归一化到 0..1 的样本.
   注意: 这里绝不做"峰值保持"或插值 —— 平滑交给列级 min/max 抽取.
   早期版本在这里取区间极值, 结果平滑段变成了"最小/最大包络", 线就在 1px/2px 之间跳 */
static double WaveNext(void)
{
    Slot  *sl;
    double perMin, dPh, v, spd, rate;

    if (g_cfg.waveType == 2) { sl = FindSlot(L"RR"); perMin = sl ? sl->value : 16.0; }
    else                     { sl = FindSlot(L"HR"); perMin = sl ? sl->value : 72.0; }
    if (perMin < 1.0)   perMin = 1.0;
    if (perMin > 300.0) perMin = 300.0;

    /* 相位步长 = 心率/60 / 每秒推入的采样数. 采样数 = 速度 x 过采样倍数,
       所以波形和上面的文字横向速度一致 */
    spd  = (g_cfg.speed > 0) ? (double)g_cfg.speed : 40.0;
    rate = spd * (double)((g_waveOS > 0) ? g_waveOS : 1);
    if (rate < 1.0) rate = 1.0;
    dPh  = (perMin / 60.0) / rate;

    v = WaveShape(g_wavePhase);
    g_wavePhase += dPh;
    while (g_wavePhase >= 1.0) g_wavePhase -= 1.0;

    v = (v + 0.35) / 1.40;                     /* 归一化到 0..1 */
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return v;
}

/* 画在窗口缓冲的底部, 不跟着滚动 */
static void PaintWave(double dt)
{
    int top, hgt, x, i;
    int cr = GetRValue(g_cfg.waveColor);
    int cg = GetGValue(g_cfg.waveColor);
    int cb = GetBValue(g_cfg.waveColor);
    int a  = g_cfg.opacity;

    if (!g_cfg.wave || !g_memBits || g_winW < 2) return;

    /* 采样率 = 滚动速度 x 过采样倍数, 和上面的文字保持同一个横向速度 */
    {
        double spd = (g_cfg.speed > 0) ? (double)g_cfg.speed : 40.0;
        int    k;
        if (g_cfg.snap) g_waveAcc += (double)SnapStep(spd, dt) * (double)g_waveOS;
        else            g_waveAcc += spd * (double)g_waveOS * dt;
        k = (int)g_waveAcc;
        if (k > 0) {
            if (k > g_waveN) k = g_waveN;
            g_waveAcc -= (double)k;
            memmove(g_waveBuf, g_waveBuf + k, (size_t)(g_waveN - k) * sizeof(float));
            for (i = 0; i < k; i++) g_waveBuf[g_waveN - k + i] = (float)WaveNext();
        }
    }

    top = g_textH + 1;
    hgt = g_winH - top - 1;
    if (hgt < 4) return;

    /* ---- 横向抗锯齿的折线渲染 ----
       1) 逐"子列"算纵向范围. 子列 = 相邻两个采样之间的线段, 比像素密 OS 倍.
       2) 每个像素列 = 它覆盖的 OS 个子列的横向平均覆盖度.
       这样尖峰的宽度不会随亚像素位置在"1 列 / 2 列"之间闪
       (直接按列取 min/max 就会闪, 那正是之前"蹦迪"的残余来源). */
    {
        static int subT[BUF_W], subB[BUF_W];
        int n = g_waveN - 1;
        if (n > BUF_W - 1) n = BUF_W - 1;
        for (i = 0; i < n; i++) {
            int ya = top + (int)((1.0f - g_waveBuf[i])     * (float)(hgt - 1));
            int yb = top + (int)((1.0f - g_waveBuf[i + 1]) * (float)(hgt - 1));
            if (ya > yb) { int t = ya; ya = yb; yb = t; }
            /* 描边: 上下各往外扩 N 像素. N=0 就是最细的原始线宽 */
            if (g_cfg.waveBold > 0) {
                ya -= g_cfg.waveBold;
                yb += g_cfg.waveBold;
            }
            if (ya < top)           ya = top;
            if (yb > top + hgt - 1) yb = top + hgt - 1;
            subT[i] = ya;
            subB[i] = yb;
        }
        for (x = 0; x < g_winW; x++) {
            static int covs[300];
            int b = x * g_waveOS;
            int e = b + g_waveOS;
            int y0 = top + hgt, y1 = top, y, covMax = 0;
            if (e > n) e = n;
            if (b >= e) continue;
            for (i = b; i < e; i++) {
                if (subT[i] < y0) y0 = subT[i];
                if (subB[i] > y1) y1 = subB[i];
            }
            if (y1 < y0) continue;
            if (y1 - y0 > 290) y1 = y0 + 290;
            for (y = y0; y <= y1; y++) {
                int cov = 0;
                for (i = b; i < e; i++) {
                    if (y >= subT[i] && y <= subB[i]) cov++;
                }
                covs[y - y0] = cov;
                if (cov > covMax) covMax = cov;
            }
            if (covMax <= 0) continue;
            for (y = y0; y <= y1; y++) {
                int a2;
                DWORD *row;
                if (!covs[y - y0]) continue;
                /* 用"本列的最大覆盖度"归一化: 主线那一行永远是满亮度, 只有边缘按比例淡出.
                   如果直接用 cov/子列数, 平滑段每行都只有 ~50% 亮度 -> 整条线看着又细又虚 */
                a2 = a * covs[y - y0] / covMax;
                if (a2 <= 0) continue;
                row = (DWORD *)((BYTE *)g_memBits + (size_t)y * BUF_W * 4);
                Blend(&row[x], cr, cg, cb, a2);
            }
        }
    }
}

static void ClampPos(void)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (g_cfg.posX == INT_MIN || g_cfg.posY == INT_MIN) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        g_cfg.posX = wa.left + 8;                 /* 默认: 左下角, 贴着任务栏 */
        g_cfg.posY = wa.bottom - g_winH - 5;
    }
    if (g_cfg.posX < vx)            g_cfg.posX = vx;
    if (g_cfg.posY < vy)            g_cfg.posY = vy;
    if (g_cfg.posX + g_winW > vx + vw) g_cfg.posX = vx + vw - g_winW;
    if (g_cfg.posY + g_winH > vy + vh) g_cfg.posY = vy + vh - g_winH;
    if (g_cfg.posX < vx) g_cfg.posX = vx;
    if (g_cfg.posY < vy) g_cfg.posY = vy;
}

static void PushToScreen(void)
{
    POINT dst, src;
    SIZE  size;
    BLENDFUNCTION bf;

    if (!g_hwnd || !g_memDC) return;
    if (!g_screenDC) {
        g_screenDC = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
        if (!g_screenDC) g_screenDC = GetDC(NULL);
        if (!g_screenDC) return;
    }

    dst.x = g_cfg.posX;
    dst.y = g_cfg.posY;
    src.x = 0;
    src.y = 0;
    size.cx = g_winW;
    size.cy = g_winH;

    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(g_hwnd, g_screenDC, &dst, &size, g_memDC, &src, 0, &bf, ULW_ALPHA);
}

/* 一次完整的"重排 + 重绘 + 上屏" */
static void Refresh(void)
{
    ComposeTexts();
    Layout();
    ClampPos();
    PaintAll();
    PushToScreen();
    g_lastSx = (int)g_scroll;
    g_dirty  = 0;
}

/* 每帧推进多少像素. 用真实 dt 算, 这样帧间隔有波动时平均速度也不会跟着变.
   snap=1: 取整 -> 每帧位移都是整数像素, 图像又匀又脆(不需要插值);
           代价是低速会被抬到 1 像素/帧 (60fps 下最低 60px/s) */
static int SnapStep(double spd, double dt)
{
    int s;
    if (spd <= 0.0) return 0;
    s = (int)(spd * dt + 0.5);
    if (s < 1) s = 1;
    return s;
}

/* 看门线程: 只干一件事 —— 当主循环被模态循环(右键菜单/拖窗口)卡住时,
   用 PostMessage 投递 WM_FRAME.
   为什么不用 WM_TIMER: 实测 TrackPopupMenu 的模态循环里 WM_TIMER 会被大幅合并/丢弃,
   帧率只能到 ~37fps. 而"投递的消息"是队列里的正式消息, 任何消息循环都必须派发, 所以能到 60fps.
   线程里只做 PostMessage, 所有 GDI 绘制仍然在主线程, 不涉及跨线程画窗口. */
static DWORD WINAPI WatchThread(LPVOID p)
{
    LARGE_INTEGER f, t0, tn;
    double last = 0.0;
    (void)p;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        Sleep(4);
        if (!g_wndAlive) break;
        if (!g_hTimer || !g_hwnd) continue;
        /* 主循环还在正常出帧 -> 不插手 */
        if ((GetTickCount64() - g_lastMainFrameMs) < 30) continue;
        /* 队列里已经躺着一条还没被处理的 WM_FRAME 就不要再投了.
           否则长时间开着菜单会堆几千条消息, 关掉后一次性补帧会看到明显跳变.
           超过 150ms 还没被处理就认为丢了, 清掉重来. */
        if (InterlockedCompareExchange(&g_framePending, 0, 0)) {
            if (GetTickCount64() - g_framePendingMs < 150) continue;
            InterlockedExchange(&g_framePending, 0);
        }
        QueryPerformanceCounter(&tn);
        {
            double t = (double)(tn.QuadPart - t0.QuadPart) / (double)f.QuadPart;
            if (t - last >= 0.0075) {
                last = t;
                InterlockedExchange(&g_framePending, 1);
                g_framePendingMs = GetTickCount64();
                if (!PostMessageW(g_hwnd, WM_FRAME, 0, 0)) {
                    InterlockedExchange(&g_framePending, 0);
                    break;
                }
            }
        }
    }
    return 0;
}

static void StartWatchThread(void)
{
    if (g_watchThread) return;
    g_wndAlive = 1;
    g_watchThread = CreateThread(NULL, 0, WatchThread, NULL, 0, NULL);
}

/* 一帧滚动: 推进偏移 -> 取景 -> 上屏. 画面没变就直接跳过, 不做无谓的 ULW */
static void ScrollTick(double dt)
{
    int sx;
    if (g_cfg.scroll && g_cfg.speed > 0) {
        if (g_cfg.snap) g_scroll += (double)SnapStep((double)g_cfg.speed, dt);
        else            g_scroll += (double)g_cfg.speed * dt;
        while (g_period > 0 && g_scroll >= (double)g_period) g_scroll -= (double)g_period;
    }
    /* --ftrace: 把每帧的时间戳和滚动位置记下来, 用来看"顿挫"到底是帧节奏问题还是渲染问题 */
    if (g_ftrace && g_qpcFreq.QuadPart) {
        LARGE_INTEGER now;
        if (!g_ftraceF) {
            g_ftraceF = _wfopen(L"ftrace.txt", L"w, ccs=UTF-8");
            if (g_ftraceF) setvbuf(g_ftraceF, NULL, _IOFBF, 1 << 20);
        }
        QueryPerformanceCounter(&now);
        if (g_ftraceF) {
            fwprintf(g_ftraceF, L"%.6f %.4f\n",
                     (double)(now.QuadPart - g_qpcStart.QuadPart) / (double)g_qpcFreq.QuadPart,
                     g_scroll);
            if (++g_ftraceN >= 64) { g_ftraceN = 0; fflush(g_ftraceF); }   /* 定期落盘, 被强杀也不丢太多 */
        }
    }
    sx = (int)g_scroll;
    g_lastTickMs = GetTickCount64();      /* 无论画不画, 都记一笔"这一帧处理过了" */
    /* 波形开着的话每一帧画面都在变, 不能跳帧 */
    if (!g_cfg.wave && sx == g_lastSx && !g_dirty) return;
    g_lastSx = sx;
    g_dirty  = 0;
    PaintBackdrop();
    PaintWindow();
    if (g_cfg.wave) PaintWave(dt);
    ApplyRoundedMask();
    PushToScreen();
}

/* 帧同步: 用高精度等待定时器.
   注意用"帧内补偿"而不是"跨帧漂移补偿":
     跨帧补偿 = 某帧晚了下一帧就提前补回来 -> 平均间隔对, 但每帧忽长忽短, 看着就是顿挫.
     帧内补偿 = 等待时间 = 周期 - 本帧已耗时 -> 每帧总长恒等于周期, 又准又匀. */
static void ScheduleFrame(double workSec)
{
    LARGE_INTEGER due;
    double wait;

    if (!g_hTimer) return;
    g_lastWork = workSec;
    wait = g_frameInterval - workSec - g_latEwma;
    if (wait < 0.004) wait = 0.004;
    if (wait > 0.250) wait = 0.250;
    g_lastWait = wait;
    due.QuadPart = -(LONGLONG)(wait * 10000000.0);
    SetWaitableTimer(g_hTimer, &due, 0, NULL, NULL, FALSE);
}

/* 建帧定时器; 已经建过就复用, 绝不能换句柄
   (主循环正卡在旧句柄上等信号, 换掉就永远等不到了) */
static void CreateFrameTimer(void)
{
    g_frameInterval = 1.0 / (double)(g_cfg.fps > 0 ? g_cfg.fps : 60);

    if (!g_hTimer) {
        /* 必须用手动复位! 自动复位定时器被 WaitForSingleObject(...,0) 探测一下就会清掉信号,
           兜底逻辑"看一眼但不补帧"时就会把这一帧吃掉 -> 周期性掉帧.
           手动复位只认 SetWaitableTimer, 探测是无副作用的安全操作. */
        g_hTimer = CreateWaitableTimerExW(NULL, NULL,
                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION |
                       CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
        if (!g_hTimer)
            g_hTimer = CreateWaitableTimerExW(NULL, NULL,
                           CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
        if (!g_hTimer)
            g_hTimer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }

    if (g_hTimer) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcStart);
        g_frameNo    = 0;
        g_lastTickMs = GetTickCount64();
        g_lastMainFrameMs = g_lastTickMs;
        ScheduleFrame(0.0);                    /* 关键: 重新武装, 否则主循环没有下一帧 */
    }
    /* 不管有没有高精度定时器, 都挂一个 WM_TIMER 兜底:
       菜单弹出、拖动窗口这些"模态消息循环"里, 主循环的等待收不到高精度信号,
       但 WM_TIMER 会被模态循环正常派发 -> 动画不会卡住 */
    if (g_hwnd) SetTimer(g_hwnd, 2, 16, NULL);
    g_lastFrameValid = 0;
}

/* 用真实流逝时间算 dt.
   这样无论谁触发(高精度定时器 / WM_TIMER 兜底 / 两个同时), 推进速度都一致:
   不会双倍推进, 也不会在模态循环里卡住 */
static double FrameDt(void)
{
    LARGE_INTEGER now;
    double dt;

    if (g_qpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&now);
    if (!g_lastFrameValid) {
        g_lastFrameQpc   = now;
        g_lastFrameValid = 1;
        return g_frameInterval;
    }
    dt = (double)(now.QuadPart - g_lastFrameQpc.QuadPart) / (double)g_qpcFreq.QuadPart;
    g_lastFrameQpc = now;
    if (dt <= 0.0) dt = 0.001;
    if (dt > 0.25) dt = 0.25;                  /* 卡顿/睡眠后别一次跳太远 */
    return dt;
}

static void FullReset(void)
{
    g_wavePhase = 0.0;
    memset(g_waveBuf, 0, sizeof(g_waveBuf));
    g_lastSx = -1;
    g_dirty  = 1;
    RebuildFont();
    BuildRuns();
    UpdateValues();
    ComposeTexts();
    Layout();
    FitFont();
    Refresh();
    if (g_cfg.bgStyle == 2) {              /* 毛玻璃: 抓背景 + 重画一次 */
        EnsureBackdrop(g_winW, g_winH);
        RefreshBackdrop();
        PaintAll();
        PushToScreen();
    }
    if (g_hwnd) {
        KillTimer(g_hwnd, 1);
        KillTimer(g_hwnd, 2);
        SetTimer(g_hwnd, 1, g_cfg.interval, NULL);
        CreateFrameTimer();
        SetWindowPos(g_hwnd,
                     g_cfg.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

/* ================================================================== 菜单 =*/

/* 所有菜单命令集中在这里, 这样 ShowMenu 和 WM_COMMAND 都能用,
   也方便用脚本 PostMessage(WM_COMMAND, IDM_xxx) 触发 / 自动化测试 */
static void DoCommand(int cmd)
{
    switch (cmd) {
    case IDM_FONTUP:
        g_cfg.size += 2;
        if (g_cfg.size > 200) g_cfg.size = 200;
        FullReset();
        SaveState();
        break;
    case IDM_FONTDN:
        g_cfg.size -= 2;
        if (g_cfg.size < 6) g_cfg.size = 6;
        FullReset();
        SaveState();
        break;
    case IDM_TOP:
        g_cfg.topmost = !g_cfg.topmost;
        FullReset();
        break;
    case IDM_RESET:
        g_cfg.posX = INT_MIN;
        g_cfg.posY = INT_MIN;
        FullReset();
        SaveState();
        break;
    case IDM_REFRESH:
        FullReset();                 /* 重新武装定时器 + 全量重绘, 卡了就点它 */
        break;
    case IDM_RELOAD:
        LoadConfig();
        LoadState();
        FullReset();
        break;
    case IDM_AUTORUN:
        SetAutoRun(!IsAutoRun());
        break;
    case IDM_DEVICE:
        ShowDeviceWindow();
        break;
    case IDM_SETTINGS:
        ShowSettingsWindow();
        break;
    case IDM_TRAYSHOW:
        ToggleHidden();
        break;
    case IDM_WAVE:
        g_cfg.wave = !g_cfg.wave;
        IniSetValue(L"wave", g_cfg.wave ? L"1" : L"0");   /* ini 是唯一来源 */
        g_waveOverride = -1;
        FullReset();
        break;
    case IDM_EXIT:
        DestroyWindow(g_hwnd);
        break;
    default:
        break;
    }
}

static void ShowMenu(void)
{
    HMENU m;
    POINT p;
    int   cmd;

    m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING,    IDM_FONTUP, L"字号 +   (也可以滚轮)");
    AppendMenuW(m, MF_STRING,    IDM_FONTDN, L"字号 -");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING,    IDM_TOP,    g_cfg.topmost ? L"取消置顶" : L"置顶");
    AppendMenuW(m, MF_STRING,    IDM_RESET,  L"回到默认位置");
    AppendMenuW(m, MF_STRING,    IDM_REFRESH,L"刷新显示");
    AppendMenuW(m, MF_STRING,    IDM_RELOAD, L"重新加载配置 (vitals.ini)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING,    IDM_DEVICE,  L"选择监测设备...");
    AppendMenuW(m, MF_STRING | (g_cfg.wave ? MF_CHECKED : 0), IDM_WAVE, L"波形小图");
    AppendMenuW(m, MF_STRING | (IsAutoRun() ? MF_CHECKED : 0), IDM_AUTORUN, L"开机自启动");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING,    IDM_EXIT,   L"退出");

    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              p.x, p.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    DoCommand(cmd);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
}

/* ============================================================== 窗口过程 =*/

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    /* explorer 重启后重新挂托盘图标 */
    if (g_taskbarCreated && msg == g_taskbarCreated) {
        g_trayOn = 0;
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WM_TRAYMSG:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK || LOWORD(lp) == WM_LBUTTONUP)
            DoCommand(IDM_SETTINGS);
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            TrayMenu();
        return 0;

    case WM_FRAME:                                  /* 看门线程投递的帧 */
        InterlockedExchange(&g_framePending, 0);
        ScrollTick(FrameDt());
        return 0;

    case WM_TIMER:
        if (wp == 9) {                      /* --ftrace: 跑够时间自己正常退出, 好让日志落盘 */
            DestroyWindow(h);
            return 0;
        }
        if (wp == 2) {                                  /* WM_TIMER 兜底 */
            /* 只在"没有高精度定时器"的退化模式下才靠它驱动帧.
               有高精度定时器时, 模态循环里的补帧交给看门线程投递 WM_FRAME 去做
               (实测 WM_TIMER 在 TrackPopupMenu 里会被大量合并掉) */
            if (!g_hTimer) ScrollTick(FrameDt());
        } else {                                        /* 数值刷新 */
            if (UpdateValues()) Refresh();              /* 没槽动就不重画 */
            /* 毛玻璃: 每 2 秒重抓一次背景(桌面上的东西可能变了) */
            if (g_cfg.bgStyle == 2 && (GetTickCount64() - g_bdTime) > 2000) {
                EnsureBackdrop(g_winW, g_winH);
                RefreshBackdrop();
                PaintAll();
                PushToScreen();
                g_lastSx = (int)g_scroll;
                g_dirty  = 0;
            }
            /* 看门狗: 帧定时器要是被什么操作弄丢了, 超过 500ms 没出帧就重新武装,
               动画永远不会卡死 */
            if (g_hTimer && (GetTickCount64() - g_lastTickMs) > 500) {
                QueryPerformanceCounter(&g_qpcStart);
                g_frameNo = 0;
                ScheduleFrame(0.0);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
        g_drag = 1;
        g_dragOff.x = (LONG)(short)LOWORD(lp);
        g_dragOff.y = (LONG)(short)HIWORD(lp);
        SetCapture(h);
        return 0;

    case WM_MOUSEMOVE:
        if (g_drag) {
            POINT cur;
            GetCursorPos(&cur);
            g_cfg.posX = cur.x - g_dragOff.x;
            g_cfg.posY = cur.y - g_dragOff.y;
            ClampPos();
            PushToScreen();
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_drag) {
            g_drag = 0;
            ReleaseCapture();
            if (g_cfg.bgStyle == 2) {          /* 拖完位置变了, 背景要重抓 */
                RefreshBackdrop();
                PaintAll();
                PushToScreen();
            }
            SaveState();
        }
        return 0;

    case WM_MOUSEWHEEL:
        {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            if (d > 0)      g_cfg.size += 2;
            else if (d < 0) g_cfg.size -= 2;
            if (g_cfg.size < 6)   g_cfg.size = 6;
            if (g_cfg.size > 200) g_cfg.size = 200;
            FullReset();
            SaveState();
        }
        return 0;

    case WM_RBUTTONUP:
        ShowMenu();
        return 0;

    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
        /* 只是重排重画, 绝不能重置滚动位置 / 不能重掷数值, 否则画面会抽一下 */
        RebuildFont();
        Layout();
        ClampPos();
        PaintAll();
        PushToScreen();
        g_lastSx = (int)g_scroll;
        g_dirty  = 0;
        return 0;

    case WM_DPICHANGED:
        FullReset();
        return 0;

    case WM_COMMAND:
        if (HIWORD(wp) == 0 && LOWORD(wp)) DoCommand((int)LOWORD(wp));
        return 0;

    case WM_DESTROY:
        KillTimer(h, 1);
        g_wndAlive = 0;
        if (g_ftraceF) { fclose(g_ftraceF); g_ftraceF = NULL; }
        TrayDel();
        SaveState();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ================================================== 自检 (--selftest) ====*/

static void SaveBmpW(const wchar_t *path, const void *bits, int w, int h, int stride)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f;
    int y;
    if (w <= 0 || h <= 0 || !bits) return;
    f = _wfopen(path, L"wb");
    if (!f) return;
    ZeroMemory(&fh, sizeof(fh));
    ZeroMemory(&ih, sizeof(ih));
    ih.biSize        = sizeof(BITMAPINFOHEADER);
    ih.biWidth       = w;
    ih.biHeight      = h;
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = (DWORD)((size_t)w * 4 * h);
    fh.bfType        = 0x4D42;
    fh.bfOffBits     = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize        = fh.bfOffBits + ih.biSizeImage;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (y = h - 1; y >= 0; y--)
        fwrite((const BYTE *)bits + (size_t)y * stride, 1, (size_t)w * 4, f);
    fclose(f);
}

static int ScratchMaxR(int w, int h)
{
    int x, y, mx = 0;
    if (!g_scrBits) return -1;
    for (y = 0; y < h; y++) {
        DWORD *row = (DWORD *)((BYTE *)g_scrBits + (size_t)y * BUF_W * 4);
        for (x = 0; x < w; x++) {
            int v = (int)((row[x] >> 16) & 0xFF);
            if (v > mx) mx = v;
        }
    }
    return mx;
}

static void SelfTest(void)
{
    FILE *lg = _wfopen(L"selftest.txt", L"w, ccs=UTF-8");
    int   i, lit = 0, maxR = 0, drawn = 0, at = 0;

    if (!lg) return;

    fwprintf(lg, L"runCount    = %d\n", g_runCount);
    fwprintf(lg, L"slotCount   = %d\n", g_cfg.slotCount);
    fwprintf(lg, L"size        = %d\n", g_cfg.size);
    fwprintf(lg, L"totalW      = %d\n", g_totalW);
    fwprintf(lg, L"winW x winH = %d x %d\n", g_winW, g_winH);
    fwprintf(lg, L"cellH       = %d\n", g_cellH);
    fwprintf(lg, L"memBits     = %p\n", g_memBits);
    fwprintf(lg, L"scrBits     = %p\n", g_scrBits);
    fwprintf(lg, L"memDC/scrDC = %p / %p\n", (void *)g_memDC, (void *)g_scrDC);
    fwprintf(lg, L"font handle = %p  objSize=%d\n",
             (void *)g_font, g_font ? GetObjectW(g_font, 0, NULL) : 0);
    fwprintf(lg, L"bmp  handle = %p  objSize=%d\n",
             (void *)g_scrBmp, g_scrBmp ? GetObjectW(g_scrBmp, 0, NULL) : 0);

    /* ---- 字体体检: 中英宽度比是不是 2:1 (全角/半角) ---- */
    {
        SIZE s;
        int  dw = 0, hw = 0;
        if (g_memDC) {
            SelectObject(g_memDC, g_font);
            GetTextExtentPoint32W(g_memDC, L"0", 1, &s);  dw = s.cx;
            SelectObject(g_memDC, g_fontCJK);
            GetTextExtentPoint32W(g_memDC, L"生", 1, &s); hw = s.cx;
            SelectObject(g_memDC, g_font);
        }
        fwprintf(lg, L"字体         = %ls   (字重 %d)\n", g_cfg.font, g_cfg.weight);
        fwprintf(lg, L"汉字字体     = %ls\n",
                 g_cfg.fontCjk[0] ? g_cfg.fontCjk : L"(与主字体相同)");
        fwprintf(lg, L"主字体有汉字 = %ls\n", FontHasCJK(g_cfg.font) ? L"是" : L"否");
        fwprintf(lg, L"字体体检     : 数字宽=%d  汉字宽=%d  中/英比=%.2f  %ls\n",
                 dw, hw, (dw > 0) ? (double)hw / (double)dw : 0.0,
                 (dw > 0 && hw == dw * 2) ? L"(完美 2:1 全角/半角, 中英宽度统一)"
                                          : L"!! 不是 2:1, 中英宽度不统一, 换个字体");
    }

    /* 逐段: 清 scratch -> 画白字 -> 量覆盖率峰值 */
    for (i = 0; i < g_runCount; i++) {
        Run *r = &g_run[i];
        int m, ww;
        if (r->w <= 0 || !r->text[0]) continue;
        ClearScratch(r->w);
        SetTextColor(g_scrDC, RGB(255, 255, 255));
        TextOutW(g_scrDC, 0, PADY, r->text, (int)wcslen(r->text));
        ww = r->w > 240 ? 240 : r->w;
        m = ScratchMaxR(ww, PADY + g_cellH + 1);
        if (m > maxR) maxR = m;
        if (m > 0) drawn++;
        fwprintf(lg, L"  run[%d] w=%4d  text='%ls'  scratchMaxR=%d\n", i, r->w, r->text, m);
        at++;
        if (at >= 6) break;
    }
    fwprintf(lg, L"有笔画输出的段数 = %d / %d     覆盖率峰值 = %d\n", drawn, at, maxR);

    /* 宽度恒定性: 反复全量刷新, 整行宽度只要变一次, 滚动画面就会抖 */
    {
        int w0 = 0, bad = 0, k, n, outN = 0;
        for (k = 0; k < 300; k++) {
            for (n = 0; n < g_cfg.slotCount; n++) g_cfg.slot[n].last = 0;
            UpdateValues();
            ComposeTexts();
            Layout();
            for (n = 0; n < g_cfg.slotCount; n++) {
                Slot *sl = &g_cfg.slot[n];
                if (sl->useAlert &&
                    (sl->value < sl->alertLo || sl->value > sl->alertHi)) outN++;
            }
            if (k == 0) w0 = g_totalW;
            else if (g_totalW != w0) bad++;
        }
        fwprintf(lg, L"宽度恒定    : 300 次全量刷新, 宽度变化 %d 次  (0 = 滚动不抖)\n", bad);
        fwprintf(lg, L"              align=%d  首帧宽=%d  末帧宽=%d\n",
                 g_cfg.align, w0, g_totalW);
        if (g_cfg.arrow && g_cfg.slotCount > 0)
            fwprintf(lg, L"箭头频率    : %.1f%% 的槽处于超范围状态  (太低就调小 pull / 调大步长)\n",
                     100.0 * (double)outN / (300.0 * (double)g_cfg.slotCount));
    }

    /* 整幅重绘后统计背缓冲 */
    PaintAll();
    for (i = 0; i < g_winH; i++) {
        DWORD *row = (DWORD *)((BYTE *)g_memBits + (size_t)i * BUF_W * 4);
        int x;
        for (x = 0; x < g_winW; x++) {
            if ((row[x] >> 24) > 200 && (row[x] & 0x00FFFFFFu)) lit++;
        }
    }
    fwprintf(lg, L"背缓冲亮像素 = %d\n", lit);

    SaveBmpW(L"dump_back.bmp", g_memBits, g_winW, g_winH, BUF_W * 4);
    SaveBmpW(L"dump_scr.bmp",  g_scrBits, g_winW, g_winH, BUF_W * 4);
    fclose(lg);
}

/* ================================================== 性能测试 (--bench) ===*/

static void Bench(void)
{
    LARGE_INTEGER f, t0, t1, t2, t3, tStart, tNow;
    double msPer, sumLine = 0, sumPaint = 0, sumULW = 0, sumTotal = 0;
    double elapsed, fps;
    double ivMin = 1e9, ivMax = 0.0, ivSum = 0.0, ivSum2 = 0.0, ivPrev = 0.0;
    double ivAvg = 0.0, ivSd = 0.0;
    int    i, N = 900, nLine = 0, frames = 0, dropped = 0, ivN = 0;
    FILE  *lg;

    QueryPerformanceFrequency(&f);
    msPer = 1000.0 / (double)f.QuadPart;

    /* 阶段1: 连续跑 N 帧, 量各段耗时 */
    for (i = 0; i < N; i++) {
        QueryPerformanceCounter(&t0);
        if ((i % 36) == 0) {                 /* 模拟 600ms 一次的数值刷新 */
            UpdateValues();
            ComposeTexts();
            PaintLine();
            nLine++;
        }
        QueryPerformanceCounter(&t1);
        PaintWindow();
        QueryPerformanceCounter(&t2);
        PushToScreen();
        QueryPerformanceCounter(&t3);

        g_scroll += (double)g_cfg.speed * g_frameInterval;
        while (g_period > 0 && g_scroll >= (double)g_period) g_scroll -= (double)g_period;

        sumLine  += (double)(t1.QuadPart - t0.QuadPart);
        sumPaint += (double)(t2.QuadPart - t1.QuadPart);
        sumULW   += (double)(t3.QuadPart - t2.QuadPart);
        sumTotal += (double)(t3.QuadPart - t0.QuadPart);
    }

    /* 阶段2: 真实按目标帧率跑 3 秒, 数实际帧数 */
    if (g_hTimer) {
        g_frameNo = 0;
        QueryPerformanceCounter(&g_qpcStart);
        QueryPerformanceCounter(&tStart);
        ScheduleFrame(0.0);
        for (;;) {
            LARGE_INTEGER ta, tb;
            WaitForSingleObject(g_hTimer, 1000);
            QueryPerformanceCounter(&ta);
            ScrollTick(FrameDt());
            QueryPerformanceCounter(&tb);
            ScheduleFrame((double)(tb.QuadPart - ta.QuadPart) / (double)f.QuadPart);
            frames++;
            QueryPerformanceCounter(&tNow);
            elapsed = (double)(tNow.QuadPart - tStart.QuadPart) / (double)f.QuadPart;
            {   /* 帧间隔统计: 这是判断"掉帧"最直接的证据 */
                if (frames > 1) {
                    double iv = (elapsed - ivPrev) * 1000.0;
                    if (iv < ivMin) ivMin = iv;
                    if (iv > ivMax) ivMax = iv;
                    ivSum  += iv;
                    ivSum2 += iv * iv;
                    ivN++;
                    if (iv > (1000.0 / (double)g_cfg.fps) * 1.5) dropped++;
                }
                ivPrev = elapsed;
            }
            if (elapsed >= 3.0) break;
        }
    } else {
        elapsed = 0.0;
    }
    fps = (elapsed > 0.0) ? (double)frames / elapsed : 0.0;

    lg = _wfopen(L"bench.txt", L"w, ccs=UTF-8");
    if (!lg) return;
    fwprintf(lg, L"窗口尺寸     = %d x %d\n", g_winW, g_winH);
    fwprintf(lg, L"整行/一圈    = %d / %d\n", g_lineW, g_period);
    fwprintf(lg, L"目标帧率     = %d fps   (每帧预算 %.2f ms)\n",
             g_cfg.fps, g_frameInterval * 1000.0);
    fwprintf(lg, L"\n--- 阶段1: 连续 %d 帧, 各段平均耗时 ---\n", N);
    fwprintf(lg, L"行重绘       = %7.3f ms   (N 帧里只做了 %d 次, 数值更新时才做)\n",
             sumLine * msPer / N, nLine);
    fwprintf(lg, L"取景(取窗口) = %7.3f ms   (纯 memcpy)\n", sumPaint * msPer / N);
    fwprintf(lg, L"上屏(ULW)    = %7.3f ms\n", sumULW * msPer / N);
    fwprintf(lg, L"每帧合计     = %7.3f ms   ->  理论上限 %.0f fps\n",
             sumTotal * msPer / N, 1000.0 / (sumTotal * msPer / N));
    if (frames > 2) {
        double var;
        ivAvg = ivSum  / (double)ivN;
        var   = ivSum2 / (double)ivN - ivAvg * ivAvg;
        if (var < 0.0) var = 0.0;      /* 浮点误差可能算出 -1e-15, sqrt 会变 nan */
        ivSd  = sqrt(var);
        fwprintf(lg, L"\n--- 阶段2: 真实跑 %.2f 秒 ---\n", elapsed);
        fwprintf(lg, L"实跑帧率     = %.1f fps   (%d 帧)\n", fps, frames);
        fwprintf(lg, L"帧间隔       = 平均 %.2f ms   最小 %.2f   最大 %.2f   标准差 %.2f ms\n",
                 ivAvg, ivMin, ivMax, ivSd);
        fwprintf(lg, L"明显掉帧     = %d 次  (间隔超过 %.1f ms 的次数)\n",
                 dropped, (1000.0 / (double)g_cfg.fps) * 1.5);
        fwprintf(lg, L"             (标准差 <1ms 且掉帧 0 次 = 帧节奏很稳)\n");
    }
    fclose(lg);
}

/* ================================================ 开机自启动(注册表) ====*/

#define AUTORUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTORUN_VAL L"VitalsStrip"

static int IsAutoRun(void)
{
    HKEY k = NULL;
    int  found = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t v[MAX_PATHW * 2];
        DWORD   cb = sizeof(v);
        if (RegQueryValueExW(k, AUTORUN_VAL, NULL, NULL, (BYTE *)v, &cb) == ERROR_SUCCESS)
            found = 1;
        RegCloseKey(k);
    }
    return found;
}

static void SetAutoRun(int on)
{
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, AUTORUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATHW], buf[MAX_PATHW + 4];
        exe[0] = 0;
        GetModuleFileNameW(NULL, exe, MAX_PATHW);
        _snwprintf(buf, MAX_PATHW + 4, L"\"%s\"", exe);
        buf[MAX_PATHW + 3] = 0;
        RegSetValueExW(k, AUTORUN_VAL, 0, REG_SZ, (const BYTE *)buf,
                       (DWORD)((wcslen(buf) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, AUTORUN_VAL);
    }
    RegCloseKey(k);
}

/* ============================================ 监测设备选择界面(装逼) ====*/

#define DEVC_W      676
#define DEVC_H      648
#define DEVC_ROW    18
#define DEVC_TOP    66

static void DevPaint(HWND h)
{
    PAINTSTRUCT ps;
    HDC   dc = BeginPaint(h, &ps);
    RECT  rc;
    HBRUSH br;
    int   i, y;
    wchar_t buf[256];

    GetClientRect(h, &rc);

    br = CreateSolidBrush(RGB(0x0A, 0x0E, 0x13));
    FillRect(dc, &rc, br);
    DeleteObject(br);
    {   /* 左边一条霓虹竖线 */
        RECT r2 = { 0, 0, 3, rc.bottom };
        br = CreateSolidBrush(RGB(0x00, 0xE5, 0xFF));
        FillRect(dc, &r2, br);
        DeleteObject(br);
    }
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, g_devFontB);
    SetTextColor(dc, RGB(0x00, 0xE5, 0xFF));
    TextOutW(dc, 16, 12, L"监测设备  //  MONITORING DEVICES", 31);

    SelectObject(dc, g_devFont);
    SetTextColor(dc, RGB(0x46, 0x5E, 0x6B));
    _snwprintf(buf, 256, L"SCANNING COMPLETE   ·   %d SOURCES FOUND   ·   BLE 5.3 / LAN / USB / WLAN   ·   AUTO-DISCOVERY ON",
               g_devCount);
    buf[255] = 0;
    TextOutW(dc, 16, 36, buf, (int)wcslen(buf));

    /* 列标题 */
    SetTextColor(dc, RGB(0x33, 0x45, 0x4F));
    TextOutW(dc, 30,  DEVC_TOP - 16, L"VENDOR", 6);
    TextOutW(dc, 140, DEVC_TOP - 16, L"MODEL", 5);
    TextOutW(dc, 340, DEVC_TOP - 16, L"TYPE", 4);
    TextOutW(dc, 520, DEVC_TOP - 16, L"LINK", 4);
    TextOutW(dc, 600, DEVC_TOP - 16, L"RTT", 3);

    for (i = 0; i < g_devCount; i++) {
        y = DEVC_TOP + i * DEVC_ROW;
        if (y + DEVC_ROW > rc.bottom - 30) break;

        if (i == g_devSel) {
            RECT r2 = { 6, y - 2, rc.right - 6, y + DEVC_ROW - 3 };
            br = CreateSolidBrush(RGB(0x10, 0x22, 0x2B));
            FillRect(dc, &r2, br);
            DeleteObject(br);
        }
        /* 标记 */
        SetTextColor(dc, (i == g_devSel) ? RGB(0x39, 0xFF, 0x14) : RGB(0x28, 0x36, 0x40));
        TextOutW(dc, 12, y, (i == g_devSel) ? L">" : L".", 1);
        /* 厂商 */
        SetTextColor(dc, (i == g_devSel) ? RGB(0x39, 0xFF, 0x14) : RGB(0x2C, 0x63, 0x46));
        TextOutW(dc, 30, y, g_dev[i].vendor, (int)wcslen(g_dev[i].vendor));
        /* 型号 */
        SetTextColor(dc, (i == g_devSel) ? RGB(0xD8, 0xF8, 0xFF) : RGB(0x6E, 0x82, 0x8D));
        TextOutW(dc, 140, y, g_dev[i].name, (int)wcslen(g_dev[i].name));
        /* 类别 */
        SetTextColor(dc, (i == g_devSel) ? RGB(0x9A, 0xB6, 0xC2) : RGB(0x48, 0x5A, 0x65));
        TextOutW(dc, 340, y, g_dev[i].kind, (int)wcslen(g_dev[i].kind));
        /* 接口 */
        SetTextColor(dc, RGB(0x48, 0x5A, 0x65));
        TextOutW(dc, 520, y, g_dev[i].link, (int)wcslen(g_dev[i].link));
        /* 只有当前连接的那个显示延迟 */
        if (i == g_devSel) {
            wchar_t lb[32];
            _snwprintf(lb, 32, L"%d ms", 6 + (i * 7) % 34);
            lb[31] = 0;
            SetTextColor(dc, RGB(0x00, 0xE5, 0xFF));
            TextOutW(dc, 600, y, lb, (int)wcslen(lb));
        }
    }

    /* 页脚 */
    {
        wchar_t lb[300];
        DevDisplayName(g_devSel, buf, 256);
        _snwprintf(lb, 300, L"> CONNECTED: %s    ·    点击任意一行切换信号源    ·    SIMULATED SOURCE (演示用, 未连接真实设备)", buf);
        lb[299] = 0;
        SetTextColor(dc, RGB(0x2E, 0x40, 0x4A));
        TextOutW(dc, 16, rc.bottom - 24, lb, (int)wcslen(lb));
    }
    EndPaint(h, &ps);
}

static LRESULT CALLBACK DevProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        if (!g_devFont) {
            g_devFont  = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, FF_DONTCARE, L"DengXian");
            g_devFontB = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, FF_DONTCARE, L"DengXian");
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        DevPaint(h);
        return 0;

    case WM_LBUTTONDOWN:
        {
            int y = (short)HIWORD(lp);
            int idx = (y - DEVC_TOP) / DEVC_ROW;
            if (y >= DEVC_TOP && idx >= 0 && idx < g_devCount) {
                g_devSel = idx;
                InvalidateRect(h, NULL, FALSE);
                FullReset();                       /* 滚动条上的 @DEVICE@ 跟着变 */
                SaveState();
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        g_devWnd = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowDeviceWindow(void)
{
    WNDCLASSEXW wc;

    if (g_devWnd) {
        SetForegroundWindow(g_devWnd);
        InvalidateRect(g_devWnd, NULL, FALSE);
        return;
    }
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DevProc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"VitalsDeviceWnd";
    RegisterClassExW(&wc);

    g_devWnd = CreateWindowExW(0, L"VitalsDeviceWnd",
                    L"监测设备 / MONITORING DEVICES",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                    CW_USEDEFAULT, CW_USEDEFAULT, DEVC_W + 16, DEVC_H + 39,
                    g_hwnd, NULL, g_inst, NULL);
    if (g_devWnd) {
        ShowWindow(g_devWnd, SW_SHOW);
        SetForegroundWindow(g_devWnd);
    }
}

/* ==================================================== 图标 / 托盘图标 ====*/

static void LoadIcons(void)
{
    g_iconBig = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    g_iconSmall = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_iconBig)   g_iconBig   = LoadIconW(NULL, IDI_APPLICATION);
    if (!g_iconSmall) g_iconSmall = g_iconBig;
}

static void TrayAdd(void)
{
    if (!g_hwnd || g_trayOn) return;
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = g_hwnd;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYMSG;
    g_nid.hIcon  = g_iconSmall;
    wcsncpy(g_nid.szTip, L"生命体征监测  vitals", 127);
    g_nid.szTip[127] = 0;
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) g_trayOn = 1;
}

static void TrayDel(void)
{
    if (!g_trayOn) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayOn = 0;
}

static void TrayMenu(void)
{
    HMENU m;
    POINT p;
    int   cmd;

    m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_TRAYSHOW, g_hidden ? L"显示面板" : L"隐藏面板");
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_cfg.wave ? MF_CHECKED : 0), IDM_WAVE, L"波形小图");
    AppendMenuW(m, MF_STRING, IDM_DEVICE, L"选择监测设备...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    DoCommand(cmd);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
}

static void ToggleHidden(void)
{
    g_hidden = !g_hidden;
    ShowWindow(g_hwnd, g_hidden ? SW_HIDE : SW_SHOWNA);
    if (!g_hidden) { g_dirty = 1; ScrollTick(0.0); }
}

/* ================================================== 设置界面 (现代化) ====*/

#include <stddef.h>

#define SETC_W 780
#define SETC_H 566
#define SET_TABW 152
#define SET_ROWY 62
#define SET_ROWH 34
#define SET_LBLX 176
#define SET_CTLX 452

#define ST_TOGGLE 0
#define ST_SLIDE  1
#define ST_COLOR  2
#define ST_CHOICE 3
#define ST_BUTTON 4

typedef struct {
    int tab;
    const wchar_t *label;
    const wchar_t *key;        /* ini 键名 */
    int type;
    int off;                   /* offsetof(Config, xxx) */
    int lo, hi, step;
    const int *vals;           /* choice 的取值 */
    const wchar_t *const *names;
    int nnames;
    int act;                   /* button 的动作 ID */
} SetItem;

static const wchar_t *CH_WEIGHT[] = { L"常规", L"半粗", L"粗体" };
static const int      V_WEIGHT[]  = { 400, 600, 700 };
static const wchar_t *CH_FPS[]    = { L"30", L"60", L"120" };
static const int      V_FPS[]     = { 30, 60, 120 };
static const wchar_t *CH_WAVE[]   = { L"ECG 心电", L"PPG 脉搏", L"RESP 呼吸" };
static const int      V_WAVE[]    = { 0, 1, 2 };
static const wchar_t *CH_BG[]     = { L"无", L"渐变", L"毛玻璃", L"通透" };
static const int      V_BG[]      = { 0, 1, 2, 3 };

static const SetItem g_set_items[] = {
/*  tab  label            key                type       offset                        lo    hi   step  vals      names      n  act */
    { 0, L"字号",          L"size",           ST_SLIDE,  offsetof(Config, size),          8,   40,   1,  NULL,     NULL,       0, 0 },
    { 0, L"字重",          L"weight",         ST_CHOICE, offsetof(Config, weight),        0,    0,   0,  V_WEIGHT, CH_WEIGHT,  3, 0 },
    { 0, L"段间距",        L"letter_space",   ST_SLIDE,  offsetof(Config, letterSpace),   0,   20,   1,  NULL,     NULL,       0, 0 },
    { 0, L"不透明度",      L"opacity",        ST_SLIDE,  offsetof(Config, opacity),      20,  255,   5,  NULL,     NULL,       0, 0 },
    { 0, L"文字颜色",      L"color",          ST_COLOR,  offsetof(Config, color),         0,    0,   0,  NULL,     NULL,       0, 0 },
    { 0, L"背景色",        L"bg",             ST_COLOR,  offsetof(Config, bg),            0,    0,   0,  NULL,     NULL,       0, 0 },
    { 0, L"背景样式",      L"bg_style",       ST_CHOICE, offsetof(Config, bgStyle),       0,    0,   0,  V_BG,     CH_BG,      4, 0 },
    { 0, L"背景透明度",    L"bg_alpha",       ST_SLIDE,  offsetof(Config, bgAlpha),       0,  255,   5,  NULL,     NULL,       0, 0 },
    { 0, L"圆角半径",      L"bg_radius",      ST_SLIDE,  offsetof(Config, bgRadius),      0,  120,   4,  NULL,     NULL,       0, 0 },
    { 0, L"红青重影",      L"glitch",         ST_TOGGLE, offsetof(Config, glitch),        0,    0,   0,  NULL,     NULL,       0, 0 },
    { 0, L"笔画加粗",      L"bolden",         ST_SLIDE,  offsetof(Config, bolden),        0,    2,   1,  NULL,     NULL,       0, 0 },
    { 0, L"亚像素平滑",    L"smooth",         ST_TOGGLE, offsetof(Config, smooth),        0,    0,   0,  NULL,     NULL,       0, 0 },
    { 0, L"超标箭头 ↑↓",   L"arrow",          ST_TOGGLE, offsetof(Config, arrow),         0,    0,   0,  NULL,     NULL,       0, 0 },
    { 0, L"数字等宽对齐",  L"align",          ST_TOGGLE, offsetof(Config, align),         0,    0,   0,  NULL,     NULL,       0, 0 },

    { 1, L"跑马灯滚动",    L"scroll",         ST_TOGGLE, offsetof(Config, scroll),        0,    0,   0,  NULL,     NULL,       0, 0 },
    { 1, L"条宽",          L"width",          ST_SLIDE,  offsetof(Config, width),       200, 1400,  20,  NULL,     NULL,       0, 0 },
    { 1, L"滚动速度",      L"speed",          ST_SLIDE,  offsetof(Config, speed),         0,  200,   5,  NULL,     NULL,       0, 0 },
    { 1, L"圈间空隙",      L"gap",            ST_SLIDE,  offsetof(Config, gap),           0,  400,  10,  NULL,     NULL,       0, 0 },
    { 1, L"帧率",          L"fps",            ST_CHOICE, offsetof(Config, fps),           0,    0,   0,  V_FPS,    CH_FPS,     3, 0 },
    { 1, L"数值检查间隔",  L"interval",       ST_SLIDE,  offsetof(Config, interval),     50, 1000,  50,  NULL,     NULL,       0, 0 },
    { 1, L"整数步进",      L"snap",           ST_TOGGLE, offsetof(Config, snap),          0,    0,   0,  NULL,     NULL,       0, 0 },
    { 1, L"永远置顶",      L"topmost",        ST_TOGGLE, offsetof(Config, topmost),       0,    0,   0,  NULL,     NULL,       0, 0 },
    { 1, L"整条可拖动",    L"grab_full",      ST_TOGGLE, offsetof(Config, grabFull),      0,    0,   0,  NULL,     NULL,       0, 0 },

    { 2, L"显示波形",      L"wave",           ST_TOGGLE, offsetof(Config, wave),          0,    0,   0,  NULL,     NULL,       0, 0 },
    { 2, L"波形高度",      L"wave_h",         ST_SLIDE,  offsetof(Config, waveH),         8,   80,   2,  NULL,     NULL,       0, 0 },
    { 2, L"波形粗细",      L"wave_bold",      ST_SLIDE,  offsetof(Config, waveBold),      0,    4,   1,  NULL,     NULL,       0, 0 },
    { 2, L"波形类型",      L"wave_type",      ST_CHOICE, offsetof(Config, waveType),      0,    0,   0,  V_WAVE,   CH_WAVE,    3, 0 },
    { 2, L"波形颜色",      L"wave_color",     ST_COLOR,  offsetof(Config, waveColor),      0,    0,   0,  NULL,     NULL,       0, 0 },
    { 2, L"回归强度 pull", L"pull",           ST_SLIDE,  offsetof(Config, pullDef),       0,   20,   1,  NULL,     NULL,       0, 0 },

    { 3, L"编辑全部指标 / 数值槽", NULL,      ST_BUTTON, 0,                              0,    0,   0,  NULL,     NULL,       0, 200 },
    { 4, L"打开配置文件 vitals.ini", NULL,    ST_BUTTON, 0,                              0,    0,   0,  NULL,     NULL,       0, 201 },
    { 4, L"打开程序所在文件夹",     NULL,     ST_BUTTON, 0,                              0,    0,   0,  NULL,     NULL,       0, 202 },
};
static const int g_set_count = (int)(sizeof(g_set_items) / sizeof(g_set_items[0]));

static Config g_set;                 /* 编辑用的副本 */
static HWND   g_setWnd  = NULL;
static int    g_setTab  = 0;
static int    g_dragIdx = -1;        /* 正在拖的滑块 */
static int    g_setLog  = 0;         /* --setlog: 把点击坐标写进 setlog.txt */
static HFONT  g_setFont = NULL;
static HFONT  g_setFontB = NULL;
static const COLORREF NEON[8] = {
    RGB(0x00,0xE5,0xFF), RGB(0x39,0xFF,0x14), RGB(0xFF,0x2E,0x88), RGB(0xFF,0xB3,0x00),
    RGB(0xFF,0xFF,0xFF), RGB(0x9A,0xB6,0xC2), RGB(0x7A,0x5C,0xFF), RGB(0xFF,0x40,0x40)
};

static int *SetInt(int i)
{
    return (int *)((char *)&g_set + g_set_items[i].off);
}
static COLORREF *SetColor(int i)
{
    return (COLORREF *)((char *)&g_set + g_set_items[i].off);
}

/* 按 key 改写 ini 里那一行, 保留你写的注释和其它内容 */
static void IniSetValue(const wchar_t *key, const wchar_t *val)
{
    wchar_t  path[MAX_PATHW];
    wchar_t *txt, *out, *s;
    size_t   cap, olen = 0, kl = wcslen(key);
    int      found = 0;

    GetIniPath(path, MAX_PATHW);
    txt = ReadTextFileW(path);
    cap = (txt ? wcslen(txt) : 0) + 8192;
    out = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!out) { if (txt) free(txt); return; }
    out[0] = 0;

    if (txt) {
        s = txt;
        while (s && *s) {
            wchar_t *nl = wcschr(s, L'\n');
            wchar_t *line = s;
            if (nl) { *nl = 0; s = nl + 1; } else s = NULL;
            /* 行尾的 \r 全部剥掉(可能有多个: 老版本文本模式写入会越攒越多),
               换行统一由我们自己补, 这样反复保存也不会膨胀 */
            { size_t ln = wcslen(line); while (ln > 0 && line[ln-1] == L'\r') line[--ln] = 0; }

            if (!found) {
                wchar_t *p = line;
                while (*p == L' ' || *p == L'\t') p++;
                if (_wcsnicmp(p, key, kl) == 0) {
                    wchar_t *q = p + kl;
                    while (*q == L' ' || *q == L'\t') q++;
                    if (*q == L'=') {
                        wchar_t tail[512];
                        wchar_t *cm = wcschr(q + 1, L';');
                        tail[0] = 0;
                        if (cm) {
                            size_t tn;
                            CopyStr(tail, cm, 512);
                            tn = wcslen(tail);
                            while (tn > 0 && (tail[tn-1] == L'\r' || tail[tn-1] == L'\n')) tail[--tn] = 0;
                        }
                        _snwprintf(out + olen, cap - olen, L"%-12s = %-16s%s\n", key, val, tail);
                        out[cap - 1] = 0;
                        olen = wcslen(out);
                        found = 1;
                        continue;
                    }
                }
            }
            _snwprintf(out + olen, cap - olen, L"%s\n", line);
            out[cap - 1] = 0;
            olen = wcslen(out);
        }
    }
    if (!found) {
        _snwprintf(out + olen, cap - olen, L"%-12s = %s\n", key, val);
        out[cap - 1] = 0;
    }
    {
        /* 用文本模式: 只有文本模式下 CRT 才认 ccs=UTF-8 (加了 'b' 会退化成 UTF-16LE).
           \r\n 由 CRT 负责翻译, 我们只写 \n; 读的时候再把 \r 剥掉, 所以反复保存不会膨胀. */
        FILE *f = _wfopen(path, L"w, ccs=UTF-8");
        if (f) { fwprintf(f, L"%s", out); fclose(f); }
    }
    free(out);
    if (txt) free(txt);
}

static void SetSaveAll(void)
{
    int i;
    wchar_t buf[64];
    for (i = 0; i < g_set_count; i++) {
        const SetItem *it = &g_set_items[i];
        if (!it->key) continue;
        if (it->type == ST_COLOR) {
            COLORREF c = *SetColor(i);
            _snwprintf(buf, 64, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
        } else if (_wcsicmp(it->key, L"wave_type") == 0) {
            int v = *SetInt(i);
            IniSetValue(L"wave_type", (v == 1) ? L"ppg" : (v == 2) ? L"resp" : L"ecg");
            continue;
        } else if (_wcsicmp(it->key, L"bg_style") == 0) {
            int v = *SetInt(i);
            IniSetValue(L"bg_style", (v == 1) ? L"grad" : (v == 2) ? L"frost" :
                                     (v == 3) ? L"glass" : L"none");
            continue;
        } else {
            _snwprintf(buf, 64, L"%d", *SetInt(i));
        }
        buf[63] = 0;
        IniSetValue(it->key, buf);
    }
}

/* ---------------------------------------------------------- 控件绘制 ----*/

static void DrawToggle(HDC dc, int x, int y, int on)
{
    HBRUSH br; RECT r; HBRUSH ob;
    r.left = x; r.top = y; r.right = x + 46; r.bottom = y + 22;
    br = CreateSolidBrush(on ? RGB(0x00,0x8C,0xA0) : RGB(0x1C,0x26,0x2E));
    ob = (HBRUSH)SelectObject(dc, br);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 22, 22);
    SelectObject(dc, ob); DeleteObject(br);
    br = CreateSolidBrush(on ? RGB(0x39,0xFF,0x14) : RGB(0x44,0x56,0x62));
    ob = (HBRUSH)SelectObject(dc, br);
    {
        int kx = on ? (x + 46 - 19) : (x + 3);
        Ellipse(dc, kx, y + 3, kx + 16, y + 19);
    }
    SelectObject(dc, ob); DeleteObject(br);
}

static void DrawSlider(HDC dc, int x, int y, int w, int v, int lo, int hi)
{
    HBRUSH br; HBRUSH ob; RECT r;
    int kx;
    if (hi <= lo) hi = lo + 1;
    kx = x + (int)((double)(v - lo) / (double)(hi - lo) * (double)w);
    if (kx < x) kx = x;
    if (kx > x + w) kx = x + w;

    br = CreateSolidBrush(RGB(0x1C,0x26,0x2E));
    ob = (HBRUSH)SelectObject(dc, br);
    r.left = x; r.top = y + 9; r.right = x + w; r.bottom = y + 13;
    FillRect(dc, &r, br);
    SelectObject(dc, ob); DeleteObject(br);

    br = CreateSolidBrush(RGB(0x00,0xE5,0xFF));
    r.left = x; r.top = y + 9; r.right = kx; r.bottom = y + 13;
    FillRect(dc, &r, br);

    ob = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, kx - 7, y + 4, kx + 7, y + 18);
    SelectObject(dc, ob); DeleteObject(br);
}

static void DrawSwatch(HDC dc, int x, int y, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    RECT r; r.left = x; r.top = y; r.right = x + 52; r.bottom = y + 22;
    FillRect(dc, &r, br);
    DeleteObject(br);
    {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(0x2A,0x38,0x44));
        HGDIOBJ op = SelectObject(dc, p);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(p);
    }
}

static void DrawChoice(HDC dc, int x, int y, const SetItem *it, int cur)
{
    int k;
    for (k = 0; k < it->nnames; k++) {
        int bx = x + k * 82;
        RECT r; HBRUSH br;
        int sel = (it->vals[k] == cur);
        r.left = bx; r.top = y; r.right = bx + 76; r.bottom = y + 24;
        br = CreateSolidBrush(sel ? RGB(0x10,0x3A,0x44) : RGB(0x12,0x1A,0x20));
        FillRect(dc, &r, br);
        DeleteObject(br);
        {
            HPEN p = CreatePen(PS_SOLID, 1, sel ? RGB(0x00,0xE5,0xFF) : RGB(0x24,0x30,0x3A));
            HGDIOBJ op = SelectObject(dc, p);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(p);
        }
        SetTextColor(dc, sel ? RGB(0xD8,0xF8,0xFF) : RGB(0x5A,0x6E,0x7A));
        DrawTextW(dc, it->names[k], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static void DrawBtn(HDC dc, int x, int y, int w, int h, const wchar_t *label, int on)
{
    RECT r; HBRUSH br;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    br = CreateSolidBrush(on ? RGB(0x10,0x3A,0x44) : RGB(0x12,0x1A,0x20));
    FillRect(dc, &r, br);
    DeleteObject(br);
    {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(0x00,0xE5,0xFF));
        HGDIOBJ op = SelectObject(dc, p);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 8, 8);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(p);
    }
    SetTextColor(dc, RGB(0xB8,0xE6,0xF0));
    DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void SetPaint(HWND h)
{
    PAINTSTRUCT ps;
    HDC     dc = BeginPaint(h, &ps);
    RECT    rc, r2;
    HBRUSH  br;
    int     i, y, t;
    static const wchar_t *tabs[] = { L"外观", L"滚动", L"波形", L"指标", L"关于" };

    GetClientRect(h, &rc);
    br = CreateSolidBrush(RGB(0x0A,0x0E,0x13)); FillRect(dc, &rc, br); DeleteObject(br);
    r2.left = 0; r2.top = 0; r2.right = SET_TABW; r2.bottom = rc.bottom;
    br = CreateSolidBrush(RGB(0x07,0x0B,0x0F)); FillRect(dc, &r2, br); DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, g_setFontB);
    SetTextColor(dc, RGB(0x00,0xE5,0xFF));
    TextOutW(dc, 20, 18, L"vitals  设置", 11);

    for (t = 0; t < 5; t++) {
        y = 64 + t * 40;
        if (t == g_setTab) {
            r2.left = 0; r2.top = y - 7; r2.right = SET_TABW; r2.bottom = y + 25;
            br = CreateSolidBrush(RGB(0x10,0x22,0x2B)); FillRect(dc, &r2, br); DeleteObject(br);
            r2.left = 0; r2.right = 3;
            br = CreateSolidBrush(RGB(0x00,0xE5,0xFF)); FillRect(dc, &r2, br); DeleteObject(br);
        }
        SelectObject(dc, g_setFont);
        SetTextColor(dc, (t == g_setTab) ? RGB(0xD8,0xF8,0xFF) : RGB(0x54,0x68,0x74));
        TextOutW(dc, 22, y, tabs[t], (int)wcslen(tabs[t]));
    }

    SelectObject(dc, g_setFont);
    if (g_setTab == 3) {                      /* 指标页: 只读清单 */
        int n = 0;
        SetTextColor(dc, RGB(0x54,0x68,0x74));
        TextOutW(dc, SET_LBLX, SET_ROWY - 26, L"当前数值槽 (要改指标/范围请编辑 vitals.ini):", 42);
        for (i = 0; i < g_cfg.slotCount && n < 20; i++) {
            wchar_t b[160];
            Slot *sl = &g_cfg.slot[i];
            _snwprintf(b, 160, L"%-8s  %g ~ %g   %s   rate=%dms", sl->key, sl->lo, sl->hi,
                       (sl->mode == 0) ? L"fixed" : (sl->mode == 1) ? L"random" : L"walk", sl->rate);
            b[159] = 0;
            SetTextColor(dc, RGB(0x7A,0x8E,0x99));
            TextOutW(dc, SET_LBLX, SET_ROWY + n * 20, b, (int)wcslen(b));
            n++;
        }
        for (i = 0; i < g_set_count; i++) {
            const SetItem *it = &g_set_items[i];
            if (it->tab != 3 || it->type != ST_BUTTON) continue;
            DrawBtn(dc, SET_LBLX, 470, 200, 30, it->label, 0);
        }
    } else if (g_setTab == 4) {               /* 关于页 */
        int yy = SET_ROWY;
        SetTextColor(dc, RGB(0x9A,0xB6,0xC2));
        TextOutW(dc, SET_LBLX, yy, L"vitals — 桌面生命体征跑马灯", 26); yy += 26;
        SetTextColor(dc, RGB(0x54,0x68,0x74));
        TextOutW(dc, SET_LBLX, yy, L"纯 C + Win32 GDI, 60fps, 无第三方依赖", 33); yy += 22;
        TextOutW(dc, SET_LBLX, yy, L"右键托盘图标可以显示/隐藏面板、打开本设置、退出", 25); yy += 22;
        TextOutW(dc, SET_LBLX, yy, L"所有数值均为模拟数据, 未连接任何真实医疗设备", 22); yy += 40;
        for (i = 0; i < g_set_count; i++) {
            const SetItem *it = &g_set_items[i];
            if (it->tab != 4 || it->type != ST_BUTTON) continue;
            DrawBtn(dc, SET_LBLX, yy, 230, 30, it->label, 0);
            yy += 40;
        }
    } else {                                   /* 普通设置页 */
        {
            int row = 0;
            for (i = 0; i < g_set_count; i++) {
                const SetItem *it = &g_set_items[i];
                int v;
                wchar_t b[64];
                RECT vr;
                if (it->tab != g_setTab) continue;
                y = SET_ROWY + row * SET_ROWH;
                row++;
                SetTextColor(dc, RGB(0x8A,0x9E,0xAA));
                TextOutW(dc, SET_LBLX, y + 3, it->label, (int)wcslen(it->label));
                if (it->type == ST_TOGGLE) {
                    DrawToggle(dc, SET_CTLX, y, *SetInt(i) ? 1 : 0);
                    TextOutW(dc, SET_CTLX + 58, y + 3, *SetInt(i) ? L"开" : L"关", 1);
                } else if (it->type == ST_SLIDE) {
                    v = *SetInt(i);
                    DrawSlider(dc, SET_CTLX, y, 220, v, it->lo, it->hi);
                    _snwprintf(b, 64, L"%d", v); b[63] = 0;
                    vr.left = SET_CTLX + 236; vr.top = y;
                    vr.right = SET_CTLX + 320; vr.bottom = y + 24;
                    DrawTextW(dc, b, -1, &vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                } else if (it->type == ST_COLOR) {
                    COLORREF c = *SetColor(i);
                    DrawSwatch(dc, SET_CTLX, y, c);
                    _snwprintf(b, 64, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
                    b[63] = 0;
                    TextOutW(dc, SET_CTLX + 62, y + 3, b, (int)wcslen(b));
                } else if (it->type == ST_CHOICE) {
                    DrawChoice(dc, SET_CTLX, y, it, *SetInt(i));
                }
            }
        }
    }

    /* 底部按钮 */
    DrawBtn(dc, SET_TABW + 24, rc.bottom - 48, 130, 32, L"保存并应用", 1);
    DrawBtn(dc, SET_TABW + 166, rc.bottom - 48, 100, 32, L"放弃", 0);
    SetTextColor(dc, RGB(0x35,0x45,0x50));
    TextOutW(dc, SET_TABW + 286, rc.bottom - 40,
             L"写入 vitals.ini(保留注释) 并立即生效", 22);
    EndPaint(h, &ps);
}

/* ---------------------------------------------------------- 交互 -------*/

static void SetAct(int act)
{
    wchar_t path[MAX_PATHW];
    if (act == 202) {
        GetModuleFileNameW(NULL, path, MAX_PATHW);
        { wchar_t *p = wcsrchr(path, L'\\'); if (p) *p = 0; }
        ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOW);
        return;
    }
    GetIniPath(path, MAX_PATHW);
    ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOW);
}

static int SetFindRow(int my, int *outY)
{
    int i, row = 0;
    if (g_setTab >= 3) return -1;
    for (i = 0; i < g_set_count; i++) {
        const SetItem *it = &g_set_items[i];
        int y;
        if (it->tab != g_setTab) continue;
        y = SET_ROWY + row * SET_ROWH;
        if (my >= y - 8 && my < y + SET_ROWH - 8) { *outY = y; return i; }
        row++;
    }
    return -1;
}

static void SetSliderFromX(int idx, int mx)
{
    const SetItem *it = &g_set_items[idx];
    int *v = SetInt(idx);
    double f = (double)(mx - SET_CTLX) / 220.0;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    *v = it->lo + (int)(f * (double)(it->hi - it->lo) + 0.5);
    if (it->step > 1) *v = it->lo + ((*v - it->lo) / it->step) * it->step;
    if (*v < it->lo) *v = it->lo;
    if (*v > it->hi) *v = it->hi;
}

static void SetClick(int mx, int my)
{
    RECT rc;
    int  rowY, idx;

    if (g_setLog) {
        FILE *f = _wfopen(L"setlog.txt", L"a, ccs=UTF-8");
        if (f) { fwprintf(f, L"SetClick x=%d y=%d tab=%d\n", mx, my, g_setTab); fclose(f); }
    }

    if (mx < SET_TABW) {                       /* 左侧分页 */
        int t = (my - 64) / 40;
        if (my >= 57 && t >= 0 && t < 5) {
            g_setTab = t;
            if (g_setLog) { FILE *f = _wfopen(L"setlog.txt", L"a, ccs=UTF-8"); if (f) { fwprintf(f, L"  -> 切到分页 %d\n", t); fclose(f); } }
            InvalidateRect(g_setWnd, NULL, FALSE);
        }
        return;
    }
    GetClientRect(g_setWnd, &rc);
    if (g_setLog) { FILE *f = _wfopen(L"setlog.txt", L"a, ccs=UTF-8"); if (f) { fwprintf(f, L"  client=%dx%d 底栏在 y=[%d,%d]\n", rc.right, rc.bottom, rc.bottom - 48, rc.bottom - 16); fclose(f); } }

    if (my >= rc.bottom - 48 && my <= rc.bottom - 16) {       /* 底部按钮 */
        if (mx >= SET_TABW + 24 && mx <= SET_TABW + 154) {    /* 保存并应用 */
            g_cfg = g_set;
            SetSaveAll();
            FullReset();
            SaveState();
            InvalidateRect(g_setWnd, NULL, FALSE);
            return;
        }
        if (mx >= SET_TABW + 166 && mx <= SET_TABW + 266) {   /* 放弃 */
            g_set = g_cfg;
            InvalidateRect(g_setWnd, NULL, FALSE);
            return;
        }
    }
    if (g_setTab == 3) {
        if (mx >= SET_LBLX && mx <= SET_LBLX + 200 && my >= 470 && my <= 500) SetAct(200);
        return;
    }
    if (g_setTab == 4) {
        int yy = SET_ROWY + 26 + 22 + 22 + 40;
        if (mx >= SET_LBLX && mx <= SET_LBLX + 230 && my >= yy && my <= yy + 30) { SetAct(201); return; }
        yy += 40;
        if (mx >= SET_LBLX && mx <= SET_LBLX + 230 && my >= yy && my <= yy + 30) { SetAct(202); return; }
        return;
    }

    idx = SetFindRow(my, &rowY);
    if (g_setLog) { FILE *f = _wfopen(L"setlog.txt", L"a, ccs=UTF-8"); if (f) { fwprintf(f, L"  -> 命中行 idx=%d rowY=%d type=%d\n", idx, rowY, (idx >= 0) ? g_set_items[idx].type : -1); fclose(f); } }
    if (idx < 0) return;
    {
        const SetItem *it = &g_set_items[idx];
        if (it->type == ST_TOGGLE) {
            *SetInt(idx) = !*SetInt(idx);
            InvalidateRect(g_setWnd, NULL, FALSE);
        } else if (it->type == ST_SLIDE) {
            SetSliderFromX(idx, mx);
            g_dragIdx = idx;
            SetCapture(g_setWnd);
            InvalidateRect(g_setWnd, NULL, FALSE);
        } else if (it->type == ST_COLOR) {
            COLORREF c = *SetColor(idx);
            int k, nxt = 0;
            for (k = 0; k < 8; k++) { if (NEON[k] == c) { nxt = (k + 1) % 8; break; } }
            *SetColor(idx) = NEON[nxt];
            InvalidateRect(g_setWnd, NULL, FALSE);
        } else if (it->type == ST_CHOICE) {
            int k;
            for (k = 0; k < it->nnames; k++) {
                int bx = SET_CTLX + k * 82;
                if (mx >= bx && mx <= bx + 76) { *SetInt(idx) = it->vals[k]; break; }
            }
            InvalidateRect(g_setWnd, NULL, FALSE);
        }
    }
}

static LRESULT CALLBACK SetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        if (!g_setFont) {
            g_setFont  = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FF_DONTCARE, L"DengXian");
            g_setFontB = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FF_DONTCARE, L"DengXian");
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        SetPaint(h);
        return 0;

    case WM_LBUTTONDOWN:
        SetClick((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_MOUSEMOVE:
        if (g_dragIdx >= 0) {
            SetSliderFromX(g_dragIdx, (short)LOWORD(lp));
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_dragIdx >= 0) { g_dragIdx = -1; ReleaseCapture(); }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        g_setWnd = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowSettingsWindow(void)
{
    WNDCLASSEXW wc;

    if (g_setWnd) {
        SetForegroundWindow(g_setWnd);
        InvalidateRect(g_setWnd, NULL, FALSE);
        return;
    }
    g_set    = g_cfg;                  /* 编辑副本, 点"保存并应用"才写回 */
    g_setTab = 0;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SetProc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = g_iconBig;
    wc.hIconSm       = g_iconSmall;
    wc.lpszClassName = L"VitalsSetWnd";
    RegisterClassExW(&wc);

    g_setWnd = CreateWindowExW(0, L"VitalsSetWnd", L"vitals 设置",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                    CW_USEDEFAULT, CW_USEDEFAULT, SETC_W + 16, SETC_H + 39,
                    g_hwnd, NULL, g_inst, NULL);
    if (g_setWnd) {
        ShowWindow(g_setWnd, SW_SHOW);
        SetForegroundWindow(g_setWnd);
    }
}

static void EnableDpi(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL (WINAPI *PFN)(void *);
        PFN f = (PFN)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (f) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
            if (f((void *)(INT_PTR)-4)) return;
        }
    }
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    WNDCLASSEXW wc;
    MSG msg;

    (void)hPrev; (void)cmdLine; (void)nShow;

    g_inst = hInst;
    EnableDpi();
    LoadIcons();
    EnsureDefaultIni();          /* 第一次运行自动落地一份默认 vitals.ini */
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    /* 命令行开关: 方便脚本或快捷方式直接开关自启 */
    if (cmdLine) {
        if (wcsstr(cmdLine, L"--autostart-on"))  { SetAutoRun(1); return 0; }
        if (wcsstr(cmdLine, L"--autostart-off")) { SetAutoRun(0); return 0; }
    }
    LoadConfig();
    LoadState();

    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = WndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = hInst;
    wc.hIcon         = g_iconBig;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName  = NULL;
    wc.lpszClassName = L"VitalsStripWnd";
    wc.hIconSm       = g_iconSmall;
    if (!RegisterClassExW(&wc)) return 1;

    RebuildFont();         /* 需要先有 DC 才有意义, 但这里只是建字体 */
    InitBuffers();         /* 建 DC + 离屏 DIB, 并把字体选进去 */
    RebuildFont();         /* DC 好了, 重新测一次行高 */

    BuildRuns();
    UpdateValues();
    ComposeTexts();
    Layout();
    FitFont();
    ClampPos();

    if (cmdLine && wcsstr(cmdLine, L"--selftest")) {
        SelfTest();
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--devices")) g_showDev = 1;
    if (cmdLine && wcsstr(cmdLine, L"--setlog"))  g_setLog  = 1;
    if (cmdLine && wcsstr(cmdLine, L"--ftrace"))  g_ftrace  = 1;

    g_hwnd = CreateWindowExW(
                 WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                 (g_cfg.topmost ? WS_EX_TOPMOST : 0),
                 L"VitalsStripWnd", L"Vitals", WS_POPUP,
                 g_cfg.posX, g_cfg.posY, 64, 24,
                 NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 2;

    PaintAll();
    PushToScreen();
    ShowWindow(g_hwnd, SW_SHOWNA);
    TrayAdd();
    SetTimer(g_hwnd, 1, g_cfg.interval, NULL);
    if (g_ftrace) SetTimer(g_hwnd, 9, 6000, NULL);   /* --ftrace: 6 秒后正常退出, 好让日志落盘 */
    CreateFrameTimer();
    StartWatchThread();

    if (cmdLine && wcsstr(cmdLine, L"--bench")) { Bench(); return 0; }

    if (g_showDev) ShowDeviceWindow();

    if (g_hTimer) {
        ScheduleFrame(0.0);
        for (;;) {
            DWORD w = MsgWaitForMultipleObjectsEx(1, &g_hTimer, INFINITE,
                                                  QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (w == WAIT_OBJECT_0) {              /* 该出下一帧了 */
                LARGE_INTEGER ta, tb;
                QueryPerformanceCounter(&ta);
                if (g_prevFrameTValid) {
                    /* 直接估计"唤醒延迟" = 实测周期 - (上帧工作量 + 上帧等待时间).
                       它是外生量, 补偿它不会自我抵消, 所以环路稳定;
                       早先用"周期误差"本身当补偿量, 那是纯积分环, 会自己把自己抵消掉 */
                    double actual = (double)(ta.QuadPart - g_prevFrameT.QuadPart)
                                    / (double)g_qpcFreq.QuadPart;
                    double lat    = actual - (g_lastWork + g_lastWait);
                    if (actual > 0.004 && actual < 0.100 && lat > -0.005 && lat < 0.050)
                        g_latEwma += (lat - g_latEwma) * 0.10;
                }
                g_prevFrameT      = ta;
                g_prevFrameTValid = 1;
                g_lastMainFrameMs = GetTickCount64();
                ScrollTick(FrameDt());
                QueryPerformanceCounter(&tb);
                ScheduleFrame((double)(tb.QuadPart - ta.QuadPart) / (double)g_qpcFreq.QuadPart);
                continue;
            }
            if (w == WAIT_OBJECT_0 + 1) {          /* 有消息 */
                BOOL quit = FALSE;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) { quit = TRUE; break; }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (quit) break;
                continue;
            }
            break;
        }
    } else {
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
