/*
 * ASCII Art Player — 控制台 ASCII 艺术动画播放器
 * 平台: Windows (MSVC / MinGW)
 *
 * 功能特性:
 *   TXT/RTF 帧动画播放、多歌曲目录、MCI 音频 / BGM 简谱、
 *   暂停跳转、进度条、SRT 字幕、帧覆盖、跳帧统计、emoji 路径
 *
 * 歌曲内配置文件 config.ini 项 (默认):
 *   width=79 height=24 debug=0 fps=25 use_bgm=0
 *   total_frames=7739 font_size=10 subtitle_offset=0
 *
 * 帧字符覆盖 override.txt:
 *   帧 行 列 字符 [protected] [color]；帧/行/列支持
 *   单值/区间/取模/取模+区间；字符可用""含空格；RGB 颜色
 *
 * exe 同目录全局配置:
 *   override.txt 全局覆盖 (单独配置优先)；autoplay.txt 0=选歌/N=直播
 *
 * 编译 (MinGW):
 *   g++ start.cpp -o start.exe -static -static-libgcc -static-libstdc++ -lwinmm -lpthread
 *
 * 运行:
 *   1. 将 start.exe 放在歌曲目录的父目录
 *   2. 双击运行，选择歌曲编号（或 autoplay.txt 直接播放）
 *   3. 空格/ESC 暂停，暂停后输入帧数+回车跳转
 *
 * 帮助: 选歌界面输入 0 可查看完整功能说明
 * 0 仅选歌界面  
 */


//使代码大小为114,514字节 >_<

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <wchar.h>
#include <windows.h>
#include <mmsystem.h>
#include "music.h"   /* BGM 乐谱播放器：BGM("music.txt") .play() .stop() */

/* ============================================================
 *  Unicode 辅助函数（支持 emoji 等非 ASCII 路径）
 * ============================================================ */

/* UTF-8 -> UTF-16 (宽字符)，返回转换后的字符数（不含终止符） */
static int utf8_to_wide(const char *utf8, wchar_t *wide, int wide_size)
{
    if (!utf8 || !wide || wide_size <= 0) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wide_size);
    return n > 0 ? n - 1 : 0;  /* 减去终止符 */
}

/* UTF-16 -> UTF-8，返回转换后的字节数（不含终止符） */
static int wide_to_utf8(const wchar_t *wide, char *utf8, int utf8_size)
{
    if (!wide || !utf8 || utf8_size <= 0) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, utf8_size, NULL, NULL);
    return n > 0 ? n - 1 : 0;
}

/* 用宽字符路径打开文件（支持 emoji 路径）*/
static FILE *wfopen(const char *path_utf8, const char *mode)
{
    wchar_t wpath[512];
    wchar_t wmode[32];
    utf8_to_wide(path_utf8, wpath, 512);
    utf8_to_wide(mode, wmode, 32);
    return _wfopen(wpath, wmode);
}

/* ============================================================
 *  全局状态
 * ============================================================ */

#define MAX_COLORS  16384
#define OUTBUF_SIZE (2 * 1024 * 1024)   /* 2MB 输出缓冲区，确保整帧一次性输出，避免分批更新造成断层 */

typedef struct { int r, g, b; } RGBColor;

/* 应用配置（从 config.ini 读取） */
typedef struct {
    int width;
    int height;
    int debug;        /* 1=输出调试信息(如帧格式检测)，0=静默 */
    int fps;          /* 播放帧率，默认25 */
    int use_bgm;      /* 1=用BGM类播放乐谱(music.txt)，0=用MCI播放音频文件，默认0 */
    int total_frames; /* 总帧数，默认7739 */
    int font_size;    /* 播放时字号，默认10 */
    float subtitle_offset; /* 字幕时间偏移(秒)，正数=延迟，负数=提前，默认0 */
} AppConfig;

static AppConfig   g_config = {79, 24, 0, 25, 0, 7739, 10, 0.0f};
static RGBColor    g_colortbl[MAX_COLORS];    /* RTF 颜色表 */
static int         g_num_colors = 0;           /* 颜色表中颜色数量 */
static HANDLE      g_hOut   = NULL;            /* 标准输出句柄 */
static HANDLE      g_hIn    = NULL;            /* 标准输入句柄 */

/* 播放控制全局状态 */
static int   g_paused       = 0;    /* 1=已暂停 */
static ULONGLONG g_play_base    = 0;    /* 播放开始的真实时间(毫秒) */
static ULONGLONG g_paused_accum = 0;    /* 累计暂停时长(毫秒) */

/* 跳帧记录（调试用）：记录连续跳过的帧区间 */
#define MAX_SKIP_RANGES 2048
static int g_skip_ranges[MAX_SKIP_RANGES][2];
static int g_skip_range_count = 0;
static int g_total_skipped = 0;
static int g_audio_pause_ms = 0;    /* 暂停时保存的音频位置（毫秒），恢复时精确跳转 */
static int g_audio_ok = 0;          /* MCI 音频是否成功打开（0=失败/无音频，1=成功） */

/* 预计算每种颜色的 VT100 真彩色转义序列，避免每次 sprintf + 单独系统调用 */
/* 最长序列 "\x1b[38;2;255;255;255m" = 18字节 + \0 = 19字节，设24留余量 */
static char g_color_esc[MAX_COLORS][24];
static int  g_color_esc_len[MAX_COLORS];

/* 帧渲染缓存：循环模式下帧会重复，缓存渲染结果避免重复解析 RTF */
static char  g_frame_cache[OUTBUF_SIZE];
static int   g_frame_cache_len = 0;
static int   g_frame_cache_idx = -1;  /* 缓存的帧索引，-1 表示无缓存 */

/* 颜色表缓存：同一批 RTF 帧的颜色表通常完全相同，
   缓存后避免每帧重复解析 + 预计算，防止帧率随时间下降 */
static char *g_cached_ctbl = NULL;
static int   g_cached_ctbl_len = 0;

/* 前向声明：write_console_utf8 定义在文件后部，apply_overrides 需要调用 */
static void write_console_utf8(const char *data, int len);

/* ============================================================
 *  帧字符覆盖配置（override.txt）
 *  格式：每行 "帧表达式 行表达式 列表达式 字符 [protected] [color]"（空格分隔），# 开头为注释
 *  帧/行/列表达式均支持：
 *    单值：100
 *    区间：100-200（含首尾）
 *    取模：%2==0（值模2等于0，如偶数帧/偶数行/偶数列）
 *  字符可用英文双引号包裹以包含空格，如 "hello world"（引号内空格原样保留）
 *  protected（可选）：1=该帧设为受保护帧（跳帧机制不跳过），0或不填=不保护
 *  color（可选）：覆盖字符的 RGB 颜色，格式 "R,G,B"（每个 0-255），如 255,0,0 为红色。
 *                 对多格宽覆盖（中文/表情/长文本）的所有字符格统一应用该颜色；
 *                 不填则继承原字符颜色。
 *  注意：若只想设颜色不想保护，请写作 "... 字符 0 R,G,B"（如 "100 5 10 ★ 0 255,0,0"）。
 * ============================================================ */
typedef struct {
    char expr[32];      /* 帧表达式：单帧/区间/取模 */
    char row_expr[32];  /* 行表达式（0-based）：单值/区间/取模，如 10-20 或 %10==0 */
    char col_expr[32];  /* 列表达式（0-based）：单值/区间/取模 */
    char ch[128];       /* 覆盖字符（UTF-8，支持中文/表情/较长文本；可用"引号"包裹以包含空格） */
    int  protected_frame; /* 1=受保护帧（不可跳过），0=普通覆盖 */
    int  color_set; /* 1=配置了 RGB 颜色 */
    int  color_r, color_g, color_b; /* RGB 颜色分量（0-255） */
} FrameOverride;
#define MAX_OVERRIDES 4096
static FrameOverride g_overrides[MAX_OVERRIDES];
static int g_override_count = 0;

/* 记录上一帧实际应用的覆盖位置、宽度以及该区域的原字符/属性，
   用于在新帧中恢复原字符，避免每一帧重渲整屏导致覆盖位置闪烁。
   覆盖字符可能占多格（中文/表情），因此记录 width 格的原字符，恢复时全部写回。 */
#define MAX_OVERRIDE_WIDTH 64
typedef struct {
    int row; int col; int width;   /* width = 覆盖字符占的控制台格数 */
    WCHAR orig_chars[MAX_OVERRIDE_WIDTH];  /* 覆盖区域原字符（按格） */
    WORD  orig_attrs[MAX_OVERRIDE_WIDTH];  /* 覆盖区域原属性（按格） */
} OverridePos;
static OverridePos g_last_override_pos[MAX_OVERRIDES];
static int g_last_override_count = 0;

/* 屏幕当前显示的帧号：>0 表示该帧内容已渲染到屏幕；-1 表示未知/已失效 */
static int g_screen_frame = -1;

/* 前向声明：utf8_display_width 定义在文件后部（第568行），clear_overrides 需要调用 */
static int utf8_display_width(const char *s);

static int frame_matches(const char *expr, int frame)
{
    if (!expr || !expr[0]) return 0;
    /* 帧号超出有效范围时不匹配 */
    if (frame < 0 || frame > g_config.total_frames) return 0;
    /* 取模运算：%N==V 或 %N==V(START-END)（后者限定区间，如 %2==0(10-20) 表示
       10~20 中所有偶数帧） */
    if (expr[0] == '%') {
        int mod, val, lo, hi;
        if (sscanf(expr, "%%%d==%d(%d-%d)", &mod, &val, &lo, &hi) == 4 && mod > 0 && lo <= hi) {
            return frame % mod == val && frame >= lo && frame <= hi;
        }
        if (sscanf(expr, "%%%d==%d", &mod, &val) == 2 && mod > 0) {
            return frame % mod == val;
        }
        return 0;
    }
    /* 区间：START-END */
    const char *dash = strchr(expr, '-');
    if (dash && dash > expr) {
        int start, end;
        if (sscanf(expr, "%d-%d", &start, &end) == 2) {
            return frame >= start && frame <= end;
        }
    }
    /* 单帧 */
    int f = atoi(expr);
    return frame == f;
}

/* 判断值是否匹配位置表达式（用于行/列，无范围上限限制）
   支持：单值(10)、区间(10-20)、取模(%10==0)、取模+区间(%2==0(10-20)) */
static int pos_matches(const char *expr, int val)
{
    if (!expr || !expr[0]) return 0;
    /* 取模运算：%N==V 或 %N==V(START-END) */
    if (expr[0] == '%') {
        int mod, v, lo, hi;
        if (sscanf(expr, "%%%d==%d(%d-%d)", &mod, &v, &lo, &hi) == 4 && mod > 0 && lo <= hi) {
            return val % mod == v && val >= lo && val <= hi;
        }
        if (sscanf(expr, "%%%d==%d", &mod, &v) == 2 && mod > 0) {
            return val % mod == v;
        }
        return 0;
    }
    /* 区间：START-END */
    const char *dash = strchr(expr, '-');
    if (dash && dash > expr) {
        int start, end;
        if (sscanf(expr, "%d-%d", &start, &end) == 2 && start <= end) {
            return val >= start && val <= end;
        }
    }
    /* 单值 */
    int f = atoi(expr);
    return val == f;
}

/* 检查某帧是否为受保护帧（protected=1 的覆盖规则，不可跳过）*/
static int is_frame_protected(int frame)
{
    for (int i = 0; i < g_override_count; i++) {
        if (g_overrides[i].protected_frame &&
            frame_matches(g_overrides[i].expr, frame)) return 1;
    }
    return 0;
}

/* 判断字符串是否全为数字（用于识别 protected / color 数字字段） */
static int all_digits(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* 加载 override.txt（条目以换行分割，每行一个条目）
   格式：帧表达式 行表达式 列表达式 字符 [protected] [color]
   帧/行/列表达式均支持：单值(100)、区间(100-200)、取模(%2==0)
   字符字段可含空格、可较长；若以英文双引号开头，则解析到下一个双引号结束
   （此时字符内可含空格，如 "hello world"）。尾部的 protected（纯数字）和
   color（R,G,B）为可选参数，未传 color 时覆盖字符使用原始帧的颜色。
   clear_first=1 时先清空已有条目（用于加载全局 override.txt）；
   clear_first=0 时追加合并——若新条目与已有条目的(帧表达式,行表达式,列表达式)相同，
   则替换旧条目（歌曲单独 override.txt，单独配置覆盖全局配置）。
   全局与单独条目都受 protected 参数控制。*/
static void load_overrides(const char *filename, int clear_first)
{
    if (clear_first) g_override_count = 0;
    FILE *f = fopen(filename, "r");
    if (!f) {
        return;  /* 文件不存在则无覆盖 */
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* 跳过空行和注释 */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;

        /* 前三个字段：帧表达式 行表达式 列表达式 */
        char expr[32] = "", row_expr[32] = "", col_expr[32] = "", rest[256] = "";
        int n = sscanf(p, "%31s %31s %31s %[^\r\n]", expr, row_expr, col_expr, rest);
        if (n < 3 || !expr[0] || !row_expr[0] || !col_expr[0]) continue;

        /* rest = "字符 [protected] [color]" */
        char ch[128] = "";
        int protected_frame = 0, color_set = 0, cr = -1, cg = -1, cb = -1;
        char tmp[256] = "";

        if (rest[0] == '"') {
            /* 引号包裹：解析到下一个双引号结束，支持空格 */
            char *closing = strchr(rest + 1, '"');
            if (closing) {
                int cl = (int)(closing - (rest + 1));
                if (cl > 127) cl = 127;
                memcpy(ch, rest + 1, (size_t)cl);
                ch[cl] = '\0';
                /* 闭合引号之后的部分用于解析 protected/color */
                strcpy(tmp, closing + 1);
            } else {
                /* 无闭合引号：引号之后全部作为字符 */
                strcpy(ch, rest + 1);
                tmp[0] = '\0';
            }
        } else {
            strcpy(tmp, rest);
        }

        /* 从 tmp 拆 token，从尾部识别 protected（纯数字）和 color（R,G,B）*/
        char tokens[8][128];
        int ntok = 0;
        char *tk = strtok(tmp, " \t");
        while (tk && ntok < 8) {
            strncpy(tokens[ntok], tk, 127);
            tokens[ntok][127] = '\0';
            ntok++;
            tk = strtok(NULL, " \t");
        }
        if (ch[0] == '\0' && ntok > 0) {
            /* 非引号模式：字符 = 除 color_tok / prot_tok 外的 token 用空格连接 */
            int color_tok = -1, prot_tok = -1;
            if (ntok > 0 && strchr(tokens[ntok - 1], ',')) {
                color_tok = ntok - 1;
                if (ntok >= 2 && all_digits(tokens[ntok - 2]))
                    prot_tok = ntok - 2;
            } else if (ntok > 0 && all_digits(tokens[ntok - 1])) {
                prot_tok = ntok - 1;
            }
            char *d = ch;
            int first = 1;
            for (int k = 0; k < ntok; k++) {
                if (k == color_tok || k == prot_tok) continue;
                if (!first && (int)(d - ch) < 126) *d++ = ' ';
                first = 0;
                int tl = (int)strlen(tokens[k]);
                if ((int)(d - ch) + tl >= 127) tl = 127 - (int)(d - ch);
                if (tl > 0) { memcpy(d, tokens[k], (size_t)tl); d += tl; }
            }
            *d = '\0';
            if (prot_tok >= 0) protected_frame = atoi(tokens[prot_tok]);
            if (color_tok >= 0) {
                int r, g, b;
                if (sscanf(tokens[color_tok], "%d,%d,%d", &r, &g, &b) == 3 &&
                    r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                    color_set = 1; cr = r; cg = g; cb = b;
                }
            }
        } else if (ch[0] != '\0') {
            /* 引号模式：从 tmp 剩余 token 识别 protected/color */
            int color_tok = -1, prot_tok = -1;
            if (ntok > 0 && strchr(tokens[ntok - 1], ',')) {
                color_tok = ntok - 1;
                if (ntok >= 2 && all_digits(tokens[ntok - 2]))
                    prot_tok = ntok - 2;
            } else if (ntok > 0 && all_digits(tokens[ntok - 1])) {
                prot_tok = ntok - 1;
            }
            if (prot_tok >= 0) protected_frame = atoi(tokens[prot_tok]);
            if (color_tok >= 0) {
                int r, g, b;
                if (sscanf(tokens[color_tok], "%d,%d,%d", &r, &g, &b) == 3 &&
                    r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                    color_set = 1; cr = r; cg = g; cb = b;
                }
            }
        }

        if (ch[0]) {
            /* 合并逻辑：若已有同 (帧表达式,行表达式,列表达式) 条目，则替换它（单独覆盖全局）；
               否则新增。*/
            int replace_idx = -1;
            for (int k = 0; k < g_override_count; k++) {
                if (strcmp(g_overrides[k].expr, expr) == 0 &&
                    strcmp(g_overrides[k].row_expr, row_expr) == 0 &&
                    strcmp(g_overrides[k].col_expr, col_expr) == 0) {
                    replace_idx = k;
                    break;
                }
            }
            if (replace_idx < 0) {
                if (g_override_count >= MAX_OVERRIDES) continue;
                replace_idx = g_override_count++;
            }
            FrameOverride *ov = &g_overrides[replace_idx];
            strncpy(ov->expr, expr, 31);
            ov->expr[31] = '\0';
            strncpy(ov->row_expr, row_expr, 31);
            ov->row_expr[31] = '\0';
            strncpy(ov->col_expr, col_expr, 31);
            ov->col_expr[31] = '\0';
            strncpy(ov->ch, ch, 127);
            ov->ch[127] = '\0';
            ov->protected_frame = protected_frame ? 1 : 0;
            ov->color_set = color_set;
            ov->color_r = cr;
            ov->color_g = cg;
            ov->color_b = cb;
        }
    }
    fclose(f);
}

/* 注：以下 clear_overrides / apply_overrides 已被"嵌入帧渲染数据流"方案取代，
   当前播放/暂停/跳转路径均不再调用（嵌入方案在 play_frame 内把覆盖字符写入
   帧渲染输出，与帧内容同一 WriteConsoleW 通道输出，从根本避免"写入但不可见"
   以及"嵌入与直写双通道位置错位导致覆盖渲染两遍"的问题）。
   函数保留以便必要时回退。*/

/* 恢复上一帧的覆盖位置为原字符（从屏幕缓冲区读回的原始字符和属性）。
   只在帧内容保持不变（如全部用 frame(0) 代替）时使用，避免重渲整屏造成闪烁。
   按覆盖字符占的格数 width 全部写回，确保多格宽覆盖字符（中文/长文本）完全清除。*/
static void clear_overrides(void)
{
    if (g_last_override_count == 0) return;
    for (int i = 0; i < g_last_override_count; i++) {
        COORD pos = {(SHORT)g_last_override_pos[i].col, (SHORT)g_last_override_pos[i].row};
        DWORD written;
        int w = g_last_override_pos[i].width;
        if (w > MAX_OVERRIDE_WIDTH) w = MAX_OVERRIDE_WIDTH;
        /* 写回记录的原字符（按格） */
        WriteConsoleOutputCharacterW(g_hOut, g_last_override_pos[i].orig_chars, (DWORD)w, pos, &written);
        /* 写回记录的原属性（按格） */
        WriteConsoleOutputAttribute(g_hOut, g_last_override_pos[i].orig_attrs, (DWORD)w, pos, &written);
    }
    g_last_override_count = 0;
}

/* 对指定帧应用字符覆盖（在帧内容已渲染到屏幕后调用）。
   直接读写屏幕缓冲区：先读回覆盖区域（width 格）的原字符/属性并记录，再写入覆盖字符。
   下一帧用 clear_overrides() 恢复原字符，这样无需每帧重渲整屏即可动态切换覆盖。
   行/列支持表达式（单值/区间/取模），一条规则可覆盖多行多列。*/
static void apply_overrides(int frame)
{
    if (g_override_count == 0) return;
    int win_w = g_config.width + 10;
    int win_h = g_config.height + 10;
    for (int i = 0; i < g_override_count; i++) {
        if (!frame_matches(g_overrides[i].expr, frame)) continue;

        /* 先收集所有匹配的行和列，避免对每个组合重复判断表达式 */
        int rows[512], nrows_match = 0;
        int cols[512], ncols_match = 0;
        for (int row = 0; row < win_h && nrows_match < 512; row++)
            if (pos_matches(g_overrides[i].row_expr, row))
                rows[nrows_match++] = row;
        for (int col = 0; col < win_w && ncols_match < 512; col++)
            if (pos_matches(g_overrides[i].col_expr, col))
                cols[ncols_match++] = col;

        for (int ri = 0; ri < nrows_match; ri++) {
            int row = rows[ri];
            for (int ci = 0; ci < ncols_match; ci++) {
                int col = cols[ci];
                COORD pos = {(SHORT)col, (SHORT)row};
                /* 覆盖字符占的格数（中文/表情=2格，ASCII=1格） */
                int width = utf8_display_width(g_overrides[i].ch);
                if (width > MAX_OVERRIDE_WIDTH) width = MAX_OVERRIDE_WIDTH;

                /* 读回该区域的原字符和属性（按格），供下一帧恢复 */
                WCHAR orig_chars[MAX_OVERRIDE_WIDTH];
                WORD  orig_attrs[MAX_OVERRIDE_WIDTH];
                DWORD read;
                ReadConsoleOutputCharacterW(g_hOut, orig_chars, (DWORD)width, pos, &read);
                ReadConsoleOutputAttribute(g_hOut, orig_attrs, (DWORD)width, pos, &read);

                /* 将 UTF-8 覆盖字符转换为 UTF-16 */
                wchar_t wbuf[256];
                int wlen = MultiByteToWideChar(CP_UTF8, 0, g_overrides[i].ch, -1, wbuf, 256);
                int count = (wlen > 1) ? (wlen - 1) : 0;
                if (count > 255) count = 255;
                DWORD written = 0;

                if (count > 0) {
                    /* 双写加固：
                       1) 流式写入（SetConsoleCursorPosition + WriteConsoleW），
                          与整屏帧渲染使用完全相同的写入方式，不受直写坐标限制；
                       2) 直写兜底（WriteConsoleOutputCharacterW 绝对坐标），
                          确保即使光标/终端状态异常，覆盖字符也写入屏幕缓冲区。
                       两种写入任一成功，覆盖字符即显示。*/
                    SetConsoleCursorPosition(g_hOut, pos);
                    WriteConsoleW(g_hOut, wbuf, (DWORD)count, &written, NULL);
                    WriteConsoleOutputCharacterW(g_hOut, wbuf, (DWORD)count, pos, &written);
                    /* 无条件设置白色前景（黑底白字），确保覆盖字符在任何
                       属性状态下都可见（不依赖原位置的属性，也不依赖 color 参数） */
                    WORD attr = 0x07;  /* FOREGROUND_RED|GREEN|BLUE = 白色 */
                    WriteConsoleOutputAttribute(g_hOut, &attr, (DWORD)count, pos, &written);
                }
                if (g_overrides[i].color_set) {
                    /* 配置了 RGB：再用 VT 真彩序列重写，应用配置颜色
                       （直写不改属性，需流式写入上色；若上色失败，字符仍已直写显示）*/
                    COORD cpos = {(SHORT)col, (SHORT)row};
                    SetConsoleCursorPosition(g_hOut, cpos);
                    wchar_t vt[64];
                    swprintf(vt, 64, L"\x1b[38;2;%d;%d;%dm",
                             g_overrides[i].color_r, g_overrides[i].color_g, g_overrides[i].color_b);
                    WriteConsoleW(g_hOut, vt, (DWORD)wcslen(vt), &written, NULL);
                    if (count > 0)
                        WriteConsoleW(g_hOut, wbuf, (DWORD)count, &written, NULL);
                    /* 恢复默认前景色，避免影响后续渲染 */
                    WriteConsoleW(g_hOut, L"\x1b[39m", 4, &written, NULL);
                }

                /* 记录位置、宽度、原字符、原属性，供下一帧恢复 */
                if (g_last_override_count < MAX_OVERRIDES) {
                    g_last_override_pos[g_last_override_count].row = row;
                    g_last_override_pos[g_last_override_count].col = col;
                    g_last_override_pos[g_last_override_count].width = width;
                    for (int k = 0; k < width; k++) {
                        g_last_override_pos[g_last_override_count].orig_chars[k] = orig_chars[k];
                        g_last_override_pos[g_last_override_count].orig_attrs[k] = orig_attrs[k];
                    }
                    g_last_override_count++;
                }
            }
        }
    }
}

/* BGM 乐谱播放器全局指针（handle_console_input 中需要访问）*/
static BGM *g_bgm = NULL;

/* exe 所在目录（UTF-8，含结尾反斜杠或为空）：用于定位全局 override.txt */
static char g_exe_dir[1024] = "";
/* 获取 exe 所在目录，供全局 override.txt 使用 */
static void get_exe_dir(void)
{
    char buf[1024];
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)(sizeof(buf) - 1));
    if (n == 0 || n >= sizeof(buf)) { g_exe_dir[0] = '\0'; return; }
    buf[n] = '\0';
    char *slash = strrchr(buf, '\\');
    if (slash) *slash = '\0';
    else { /* 没有目录部分（如裸 exe 名），用空目录表示当前目录 */ buf[0] = '\0'; }
    strncpy(g_exe_dir, buf, sizeof(g_exe_dir) - 1);
    g_exe_dir[sizeof(g_exe_dir) - 1] = '\0';
}

/* 读取 exe 同目录的 autoplay.txt（第一个有效整数）：
   0 或文件不存在/无效 = 正常进入选歌界面；
   N（1~歌曲总数）= 直接进入第 N 首歌曲的播放界面。
   支持 # 开头的注释行和空行（跳过），读取第一个有效数字 */
static int read_autoplay_index(void)
{
    char path[1152];
    if (g_exe_dir[0]) {
        strcpy(path, g_exe_dir);
        strcat(path, "\\autoplay.txt");
    } else {
        strcpy(path, "autoplay.txt");
    }
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int val = 0;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;
        /* 第一个有效数字 */
        if (sscanf(p, "%d", &val) == 1) break;
    }
    fclose(f);
    return val;
}

/* MSVC 没有 memmem（GNU 扩展），自己实现一个 */
static const char *find_bytes(const char *haystack, int hlen,
                               const char *needle, int nlen)
{
    if (nlen <= 0 || hlen < nlen) return NULL;
    for (int i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, (size_t)nlen) == 0)
            return haystack + i;
    }
    return NULL;
}

/* ============================================================
 *  配置文件读取（config.ini，格式: key=value，支持 # 注释）
 * ============================================================ */
static void trim_str(char *s)
{
    /* 去除首尾空格、制表符、回车、换行 */
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 &&
           (s[len - 1] == ' '  || s[len - 1] == '\t' ||
            s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static void load_config(const char *filename)
{
    FILE *fp = wfopen(filename, "r");
    if (!fp) return;   /* 配置文件不存在，保持默认值 */

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        trim_str(line);
        if (line[0] == '\0' || line[0] == '#') continue;  /* 空行 / 注释 */

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key   = line;
        char *value = eq + 1;
        trim_str(key);
        trim_str(value);

        if (strcmp(key, "width") == 0)
            g_config.width = atoi(value);
        else if (strcmp(key, "height") == 0)
            g_config.height = atoi(value);
        else if (strcmp(key, "debug") == 0) {
            /* debug=1 / true / on 都算开启 */
            if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
                strcmp(value, "on") == 0 || strcmp(value, "yes") == 0)
                g_config.debug = 1;
            else
                g_config.debug = 0;
        }
        else if (strcmp(key, "fps") == 0) {
            g_config.fps = atoi(value);
            if (g_config.fps < 1)  g_config.fps = 25;   /* 合法性兜底 */
            if (g_config.fps > 120) g_config.fps = 120;  /* 上限120fps */
        }
        else if (strcmp(key, "use_bgm") == 0) {
            /* use_bgm=1：用 BGM 类播放乐谱(music.txt)；0：用 MCI 播放音频文件 */
            if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
                strcmp(value, "on") == 0 || strcmp(value, "yes") == 0)
                g_config.use_bgm = 1;
            else
                g_config.use_bgm = 0;
        }
        else if (strcmp(key, "total_frames") == 0) {
            g_config.total_frames = atoi(value);
            if (g_config.total_frames < 1) g_config.total_frames = 7739;  /* 合法性兜底 */
        }
        else if (strcmp(key, "font_size") == 0) {
            g_config.font_size = atoi(value);
            if (g_config.font_size < 6)  g_config.font_size = 10;   /* 合法性兜底 */
            if (g_config.font_size > 72) g_config.font_size = 72;   /* 上限 */
        }
        else if (strcmp(key, "subtitle_offset") == 0) {
            /* 字幕时间偏移(秒)，正数=字幕延迟，负数=字幕提前，支持小数如 0.5 */
            g_config.subtitle_offset = (float)atof(value);
        }
        /* 其他键忽略，方便以后扩展 */
    }
    fclose(fp);

    /* 合法性兜底：太小的值回退到默认 */
    if (g_config.width  < 10) g_config.width  = 79;
    if (g_config.height < 10) g_config.height = 24;

    /* debug 模式下输出配置摘要，方便排查 */
    if (g_config.debug) {
        printf("  [配置] %dx%d fps=%d frames=%d font=%d bgm=%d sub_offset=%.2fs\n",
               g_config.width, g_config.height, g_config.fps,
               g_config.total_frames, g_config.font_size,
               g_config.use_bgm, g_config.subtitle_offset);
    }
}

/* ============================================================
 *  多歌曲支持：扫描子目录、查找音频文件
 * ============================================================ */

#define MAX_SONGS  64
#define MAX_NAME   256

typedef struct {
    wchar_t names[MAX_SONGS][MAX_NAME];  /* 宽字符，支持 emoji 等 Unicode 目录名 */
    int count;
} SongList;

/* 扫描当前目录下所有子目录（排除 . 和 ..），作为歌曲列表 */
static int scan_song_dirs(SongList *list)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(L"*", &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    list->count = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                if (list->count < MAX_SONGS) {
                    wcsncpy(list->names[list->count], fd.cFileName, MAX_NAME - 1);
                    list->names[list->count][MAX_NAME - 1] = L'\0';
                    list->count++;
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return list->count;
}

/* 在当前目录下查找音频文件（按优先级：mp3 > wav > mid > ...）*/
static int find_audio_file(wchar_t *out, int out_size)
{
    static const wchar_t *exts[] = {
        L".mp3", L".wav", L".mid", L".midi", L".wma", L".ogg", L".flac", L".aac", L".m4a", NULL
    };
    WIN32_FIND_DATAW fd;

    for (int i = 0; exts[i] != NULL; i++) {
        wchar_t pattern[260];
        swprintf(pattern, 260, L"*%s", exts[i]);
        HANDLE hFind = FindFirstFileW(pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            wcsncpy(out, fd.cFileName, out_size - 1);
            out[out_size - 1] = L'\0';
            FindClose(hFind);
            return 1;
        }
    }
    return 0;
}

/* 在当前目录下查找 .srt 歌词文件 */
static int find_srt_file(wchar_t *out, int out_size)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(L"*.srt", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        wcsncpy(out, fd.cFileName, out_size - 1);
        out[out_size - 1] = L'\0';
        FindClose(hFind);
        return 1;
    }
    return 0;
}

/* ============================================================
 *  SRT 歌词解析与渲染
 * ============================================================ */

#define MAX_SRT_ENTRIES 800

typedef struct {
    int  start_ms;
    int  end_ms;
    char text[2][256];  /* 最多两行字幕（如英文+中文翻译） */
    int  line_count;    /* 实际行数（1或2）*/
} SrtEntry;

static SrtEntry g_srt[MAX_SRT_ENTRIES];
static int      g_srt_count = 0;

/* 解析 SRT 时间戳 "00:00:13,980" -> 毫秒 */
static int parse_srt_time(const char *s)
{
    int h, m, sec, ms;
    /* 同时支持逗号(00:00:13,980)和点号(00:00:13.980)作为毫秒分隔符 */
    if (sscanf(s, "%d:%d:%d,%d", &h, &m, &sec, &ms) == 4)
        return h * 3600000 + m * 60000 + sec * 1000 + ms;
    if (sscanf(s, "%d:%d:%d.%d", &h, &m, &sec, &ms) == 4)
        return h * 3600000 + m * 60000 + sec * 1000 + ms;
    return 0;
}

/* 加载并解析 SRT 文件 */
static int load_srt(const char *filename)
{
    FILE *f = wfopen(filename, "rb");  /* 用二进制模式，准确检测BOM，支持emoji路径 */
    if (!f) return 0;

    /* 检测并跳过 UTF-8 BOM (EF BB BF) */
    unsigned char bom[3];
    size_t nread = fread(bom, 1, 3, f);
    if (nread == 3) {
        if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
            fseek(f, 0, SEEK_SET);  /* 不是BOM，回到文件头 */
    } else {
        /* 文件小于3字节，回到文件头（即使内容很少也尝试解析）*/
        fseek(f, 0, SEEK_SET);
    }

    char line[512];
    int idx = 0;

    while (fgets(line, sizeof(line), f)) {
        /* 查找时间行（包含 -->）*/
        if (strstr(line, "-->")) {
            char start[24], end[24];
            if (sscanf(line, "%23s --> %23s", start, end) == 2) {
                g_srt[idx].start_ms = parse_srt_time(start);
                g_srt[idx].end_ms   = parse_srt_time(end);
                g_srt[idx].text[0][0] = '\0';
                g_srt[idx].text[1][0] = '\0';
                g_srt[idx].line_count = 0;

                /* 读取文本行（直到空行或文件结束），最多保留两行 */
                while (fgets(line, sizeof(line), f)) {
                    if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
                        break;
                    /* 去掉行尾换行符 */
                    int len = (int)strlen(line);
                    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                        line[--len] = '\0';
                    if (len == 0) break;
                    /* 分别存入各行（最多2行）*/
                    if (g_srt[idx].line_count < 2) {
                        strncpy(g_srt[idx].text[g_srt[idx].line_count], line, 255);
                        g_srt[idx].text[g_srt[idx].line_count][255] = '\0';
                        g_srt[idx].line_count++;
                    }
                }
                idx++;
                if (idx >= MAX_SRT_ENTRIES) break;
            }
        }
    }
    fclose(f);
    g_srt_count = idx;
    return idx;
}

/* 计算 UTF-8 字符串的显示宽度（中文占2列，ASCII占1列）*/
/* 返回 UTF-8 字符的实际显示宽度（控制台格数）与字节数。
   与 write_console_utf8 的渲染规则保持一致：
   - ASCII：1格
   - 2字节UTF-8：1格
   - U+FE0F 变体选择符：0格（零宽，渲染时跳过）
   - U+2764（❤）：1格（渲染时映射为窄心 ♥ U+2665）
   - BMP 表情/符号（渲染时会插入退格压缩到1格）：1格
   - 辅助平面 U+1F000-U+1FFFF（渲染时退格压缩到1格）：1格
   - 中文等其他 3 字节字符：2格
   - 其他辅助平面字符：2格 */
static int utf8_char_info(const unsigned char *s, int *b_len)
{
    unsigned char c = *s;
    if (c < 0x80) { *b_len = 1; return 1; }
    if (c < 0xE0) { *b_len = 2; return 1; }
    if (c < 0xF0) {
        *b_len = 3;
        unsigned int cp = ((unsigned int)(c & 0x0F) << 12) |
                          ((unsigned int)(s[1] & 0x3F) << 6) |
                          (unsigned int)(s[2] & 0x3F);
        if (cp == 0xFE0F) return 0;
        if (cp == 0x2764) return 1;
        /* BMP 表情/符号范围（与 write_console_utf8 的退格压缩范围一致） */
        if ((cp >= 0x203C && cp <= 0x2049) ||
            (cp >= 0x2122 && cp <= 0x2139) ||
            (cp >= 0x2194 && cp <= 0x2199) ||
            (cp >= 0x2300 && cp <= 0x23FF) ||
            (cp == 0x24C2) ||
            (cp >= 0x25AA && cp <= 0x25AB) ||
            (cp >= 0x25B6 && cp <= 0x25C0) ||
            (cp >= 0x25FB && cp <= 0x25FE) ||
            (cp >= 0x2600 && cp <= 0x26FF) ||
            (cp >= 0x2700 && cp <= 0x27BF) ||
            (cp >= 0x2934 && cp <= 0x2935) ||
            (cp >= 0x2B00 && cp <= 0x2BFF) ||
            (cp == 0x3030 || cp == 0x303D) ||
            (cp == 0x3297 || cp == 0x3299))
            return 1;
        return 2;  /* 中文、日文等宽字符 */
    }
    *b_len = 4;
    {
        unsigned int cp = 0x10000 +
                          ((unsigned int)(c & 0x07) << 18) +
                          ((unsigned int)(s[1] & 0x3F) << 12) +
                          ((unsigned int)(s[2] & 0x3F) << 6) +
                          (unsigned int)(s[3] & 0x3F);
        if (cp >= 0x1F000 && cp <= 0x1FFFF) return 1;  /* 辅助平面表情压缩到1格 */
        return 2;  /* 其他辅助平面字符按2格 */
    }
}

/* 计算 UTF-8 字符串的实际显示宽度（控制台格数），与渲染规则一致 */
static int utf8_display_width(const char *s)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int blen = 1;
        w += utf8_char_info(p, &blen);
        p += blen;
    }
    return w;
}

/* 根据当前播放时间（毫秒）查找对应的字幕条目 */
static const SrtEntry *get_current_entry(int ms)
{
    for (int i = 0; i < g_srt_count; i++) {
        if (ms >= g_srt[i].start_ms && ms <= g_srt[i].end_ms)
            return &g_srt[i];
    }
    return NULL;
}

/* ============================================================
 *  控制台初始化：窗口大小从配置读取、字体 Consolas、启用 VT 真彩色
 * ============================================================ */
static void init_console(void)
{
    SetConsoleTitleA("ASCII Art Player");

    /* 重新获取句柄（main 选歌阶段已初始化一次，此处确保 init_console 独立调用时也有效）*/
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    /* --- 1. 启用虚拟终端处理（支持 ANSI / 真彩色转义序列） --- */
    DWORD mode = 0;
    GetConsoleMode(g_hOut, &mode);
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(g_hOut, mode);

    /* --- 2. 设置字体为粗体 Consolas --- */
    CONSOLE_FONT_INFOEX fi;
    fi.cbSize        = sizeof(CONSOLE_FONT_INFOEX);
    fi.nFont         = 0;
    fi.dwFontSize.X  = 0;
    fi.dwFontSize.Y  = g_config.font_size;   /* 字号从配置文件读取 */
    fi.FontFamily    = TMPF_TRUETYPE;
    fi.FontWeight    = FW_BOLD;
    wcscpy(fi.FaceName, L"Consolas");
    SetCurrentConsoleFontEx(g_hOut, FALSE, &fi);

    /* --- 3. 设置屏幕缓冲区 ---
     * 实际窗口大小 = 配置值 + 10（留余量），缓冲区宽度必须 >= 窗口宽度，
     * 高度固定设大（200行），避免帧内容行数超过窗口导致滚动 */
    int win_w = g_config.width + 10;
    int win_h = g_config.height + 10;
    COORD buf = {(SHORT)win_w, 200};
    SetConsoleScreenBufferSize(g_hOut, buf);

    /* --- 4. 设置窗口大小（配置值 + 10） --- */
    SMALL_RECT rc = {0, 0, (SHORT)(win_w - 1), (SHORT)(win_h - 1)};
    SetConsoleWindowInfo(g_hOut, TRUE, &rc);

    /* 输入句柄 + 设置非行输入模式（空格键实时响应，不等待回车）*/
    g_hIn = GetStdHandle(STD_INPUT_HANDLE);
    {
        DWORD in_mode = 0;
        GetConsoleMode(g_hIn, &in_mode);
        in_mode &= ~ENABLE_LINE_INPUT;  /* 禁用行输入，单个按键立即产生事件 */
        in_mode &= ~ENABLE_ECHO_INPUT;  /* 禁用回显，避免按键字符显示在画面上 */
        SetConsoleMode(g_hIn, in_mode);
    }
}

/* 光标回到左上角 (0,0)，用于逐帧覆盖 */
static void recursurv(void)
{
    COORD pos = {0, 0};
    SetConsoleCursorPosition(g_hOut, pos);
}

/* 清空控制台输入缓冲区，确保 getchar 真正等待用户
   注意：此函数在选歌阶段（g_hIn 未初始化前）也会被调用，因此自己获取句柄而非使用全局 g_hIn */
static void clear_input_buffer(void)
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    FlushConsoleInputBuffer(hIn);
}

/* ============================================================
 *  播放控制：鼠标点击进度条跳转 + 空格键暂停/播放
 * ============================================================ */

/* 非阻塞处理控制台输入事件（键盘按键：空格/ESC 暂停） */
static void handle_console_input(void)
{
    INPUT_RECORD rec;
    DWORD count = 0;

    /* 循环读取所有待处理事件（非阻塞）*/
    while (PeekConsoleInputA(g_hIn, &rec, 1, &count) && count > 0) {
        if (!ReadConsoleInputA(g_hIn, &rec, 1, &count)) break;

        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            /* 空格键 / ESC 键：暂停/播放切换 */
            if (rec.Event.KeyEvent.wVirtualKeyCode == VK_SPACE ||
                rec.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
                g_paused = !g_paused;
                /* 同步暂停/恢复音乐（MCI 和 BGM 都支持） */
                if (g_config.use_bgm) {
                    if (g_bgm) {
                        if (g_paused) g_bgm->pause();
                        else g_bgm->resume();
                    }
                } else {
                    if (g_paused) {
                        /* 暂停：先保存当前精确位置，再 pause */
                        wchar_t pos_str[64] = L"";
                        mciSendStringW(L"status s1 position", pos_str, 64, NULL);
                        g_audio_pause_ms = _wtoi(pos_str);
                        mciSendStringW(L"pause s1", NULL, 0, NULL);
                    } else {
                        /* 恢复：从保存的精确位置开始播放，避免 pause 异步导致的偏移 */
                        wchar_t play_cmd[256];
                        swprintf(play_cmd, 256, L"play s1 from %d", g_audio_pause_ms);
                        mciSendStringW(play_cmd, NULL, 0, NULL);
                    }
                }
            }
        }
    }
}

/* 内部：构建一行居中歌词到宽字符缓冲区（UTF-8 -> UTF-16，支持中文），返回写入字符数 */
static int build_lyric_line(WCHAR *out, int out_size, const char *text, int win_w)
{
    /* 将 UTF-8 转换为 UTF-16 */
    static WCHAR wline[512];
    int wlen = 0;
    if (text && text[0] != '\0') {
        int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
        if (needed > 0 && needed < 256) {
            MultiByteToWideChar(CP_UTF8, 0, text, -1, wline, needed);
            wlen = needed - 1;
        }
    }

    /* 用显示宽度计算居中（中文占2列）*/
    int text_width = utf8_display_width(text ? text : "");
    int pad = (win_w - text_width) / 2;
    if (pad < 0) pad = 0;

    int idx = 0;
    for (int i = 0; i < pad && idx < out_size - 2; i++)
        out[idx++] = L' ';
    /* 歌词过长时按显示宽度截断，避免超出窗口导致换行 */
    int out_width = pad;
    for (int i = 0; i < wlen && idx < out_size - 2; i++) {
        int cw = (wline[i] > 127) ? 2 : 1;  /* 中文占2列，ASCII占1列 */
        if (out_width + cw > win_w - 1) break;
        out[idx++] = wline[i];
        out_width += cw;
    }
    while (out_width < win_w - 1 && idx < out_size - 2) {
        out[idx++] = L' ';
        out_width++;
    }
    return idx;
}

/* 在进度条上方渲染当前歌词（支持双行，两行合并一次性输出避免闪烁） */
static void draw_lyric(int current_ms)
{
    int win_w = g_config.width + 10;
    int win_h = g_config.height + 10;

    /* 显式设置白色前景色，确保歌词可见（RTF渲染可能改变了颜色）*/
    DWORD w;
    WriteConsoleA(g_hOut, "\x1b[38;2;255;255;255m", 19, &w, NULL);

    /* 应用字幕时间偏移：正数=延迟，负数=提前 */
    int adjusted_ms = current_ms + (int)(g_config.subtitle_offset * 1000.0f);

    const SrtEntry *entry = NULL;
    if (g_srt_count > 0)
        entry = get_current_entry(adjusted_ms);

    const char *line1 = "";
    const char *line2 = "";
    if (entry && entry->line_count >= 2) {
        line1 = entry->text[0];
        line2 = entry->text[1];
    } else if (entry && entry->line_count == 1) {
        line1 = "";
        line2 = entry->text[0];
    }

    /* 构建两行歌词到一个缓冲区，中间用换行分隔，一次性输出 */
    static WCHAR outbuf[2048];
    int idx = 0;
    idx += build_lyric_line(outbuf + idx, 2048 - idx, line1, win_w);
    if (idx < 2047) outbuf[idx++] = L'\n';  /* 换行到第二行，带边界检查 */
    if (idx < 2048)
        idx += build_lyric_line(outbuf + idx, 2048 - idx, line2, win_w);
    if (idx > 2048) idx = 2048;  /* 安全兜底 */

    /* 定位到第一行位置，一次性输出两行歌词 */
    COORD pos = {0, (SHORT)(win_h - 4)};
    SetConsoleCursorPosition(g_hOut, pos);
    WriteConsoleW(g_hOut, outbuf, (DWORD)idx, &w, NULL);

    /* 光标移回 (0,0) */
    COORD home = {0, 0};
    SetConsoleCursorPosition(g_hOut, home);
}

/* 前向声明：paused_interaction 中需要调用这两个函数，但它们定义在后面 */
static void draw_progress_bar(int current_frame);
static int  play_frame(const char *filepath);

/* 暂停后恢复音频播放（BGM / MCI 统一处理，从保存的精确位置继续）*/
static void resume_audio_after_pause(void)
{
    if (g_config.use_bgm) {
        if (g_bgm) g_bgm->resume();
    } else {
        wchar_t play_cmd[256];
        swprintf(play_cmd, 256, L"play s1 from %d", g_audio_pause_ms);
        mciSendStringW(play_cmd, NULL, 0, NULL);
    }
}

/* ============================================================
 *  暂停交互：空格恢复播放，输入数字+回车跳转
 *  全程使用非行输入模式，自己收集数字输入
 * ============================================================ */
static void paused_interaction(int *current_frame, const char *frame_ext,
                                const char *audio_file)
{
    int win_w = g_config.width + 10;
    int win_h = g_config.height + 10;
    int bar_row = win_h - 1;

    ULONGLONG pause_start = GetTickCount64();  /* 记录暂停开始时间 */

    FlushConsoleInputBuffer(g_hIn);

    char numbuf[16];   /* 用户输入的数字 */
    int  numlen = 0;
    numbuf[0] = '\0';

    while (g_paused) {
        /* 重绘进度条第二行：保留时间和百分比，追加暂停提示/输入内容 */
        int display_total = g_config.total_frames;
        int cur_ms = *current_frame * 1000 / g_config.fps;
        int cur_mm = cur_ms / 60000;
        int cur_ss = (cur_ms % 60000) / 1000;
        int total_ms = display_total * 1000 / g_config.fps;
        int total_mm = total_ms / 60000;
        int total_ss = (total_ms % 60000) / 1000;
        double ratio = (double)(*current_frame) / display_total;
        if (ratio < 0) ratio = 0;
        if (ratio > 1) ratio = 1;

        COORD pos = {0, (SHORT)bar_row};
        SetConsoleCursorPosition(g_hOut, pos);
        char line[512];
        if (numlen > 0)
            snprintf(line, sizeof(line),
                     "%02d:%02d/%02d:%02d  %d%% [已暂停] 帧数: %s (回车跳转, 退格删除)",
                     cur_mm, cur_ss, total_mm, total_ss,
                     (int)(ratio * 100), numbuf);
        else
            snprintf(line, sizeof(line),
                     "%02d:%02d/%02d:%02d  %d%% [已暂停 - 空格/ESC继续，输入帧数(0-%d)跳转]",
                     cur_mm, cur_ss, total_mm, total_ss,
                     (int)(ratio * 100), display_total);
        int llen = (int)strlen(line);
        if (llen > win_w - 1) llen = win_w - 1;
        while (llen < win_w - 1 && llen < (int)sizeof(line) - 2)
            line[llen++] = ' ';
        line[llen] = '\0';
        DWORD written = 0;
        WriteConsoleA(g_hOut, line, (DWORD)strlen(line), &written, NULL);

        /* 阻塞等待按键 */
        INPUT_RECORD rec;
        DWORD count = 0;
        if (!ReadConsoleInputA(g_hIn, &rec, 1, &count)) break;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
            continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        char ch = rec.Event.KeyEvent.uChar.AsciiChar;

        if (vk == VK_SPACE || vk == VK_ESCAPE) {
            /* 空格或ESC：恢复播放，从保存的精确位置开始 */
            g_paused = 0;
            resume_audio_after_pause();
            break;
        }
        else if (vk == VK_BACK) {
            /* 退格：删除最后一个数字 */
            if (numlen > 0) {
                numlen--;
                numbuf[numlen] = '\0';
            }
        }
        else if (vk == VK_RETURN) {
            /* 回车：有输入则跳转，无输入则恢复播放 */
            if (numlen > 0) {
                int target = atoi(numbuf);
                int jump_limit = g_config.total_frames;
                if (target >= 0 && target <= jump_limit) {
                    *current_frame = target;
                    /* 跳转前光标归位 (0,0)，确保帧从正确位置开始渲染 */
                    COORD home = {0, 0};
                    SetConsoleCursorPosition(g_hOut, home);
                    int frame_idx = target;
                    char filepath[256];
                    snprintf(filepath, sizeof(filepath), "resource\\frame(%d)%s", frame_idx, frame_ext);
                    int rendered = play_frame(filepath);
                    if (!rendered && g_screen_frame < 0) {
                        /* 屏幕还没有帧内容，找一个存在的帧渲染 */
                        for (int f = frame_idx - 1; f >= 0; f--) {
                            char prev_path[256];
                            snprintf(prev_path, sizeof(prev_path), "resource\\frame(%d)%s", f, frame_ext);
                            if (play_frame(prev_path)) { rendered = 1; g_screen_frame = f; break; }
                        }
                    }
                    if (rendered) {
                        g_screen_frame = frame_idx;
                        g_last_override_count = 0;  /* 整屏重渲，旧覆盖失效 */
                        /* 播放帧已嵌入覆盖字符，直接可见，无需再 apply */
                    } else {
                        /* 目标帧文件缺失：屏幕未变，保持上一帧（含其嵌入的覆盖），
                           不做 apply 直写，避免嵌入与直写位置错位造成覆盖两遍 */
                    }
                    /* mm:ss、百分数、音频都对应输入的帧数 target */
                    draw_lyric(target * 1000 / g_config.fps);
                    draw_progress_bar(target);

                    /* 同步跳转音频到输入帧对应的时间 */
                    if (g_config.use_bgm) {
                        if (g_bgm) g_bgm->seek(target * 1000 / g_config.fps);
                    } else if (audio_file[0] != '\0') {
                        int seek_ms = target * 1000 / g_config.fps;
                        wchar_t mci_cmd[256];
                        mciSendStringW(L"stop s1", NULL, 0, NULL);
                        swprintf(mci_cmd, 256, L"seek s1 to %d", seek_ms);
                        mciSendStringW(mci_cmd, NULL, 0, NULL);
                        g_audio_pause_ms = seek_ms;
                    }
                }
                /* 清空输入，继续保持暂停 */
                numlen = 0;
                numbuf[0] = '\0';
            } else {
                /* 无输入时回车也恢复播放，从保存的精确位置开始 */
                g_paused = 0;
                resume_audio_after_pause();
                break;
            }
        }
        else if (ch >= '0' && ch <= '9') {
            /* 数字键：添加到输入缓冲区 */
            if (numlen < 15) {
                numbuf[numlen++] = ch;
                numbuf[numlen] = '\0';
            }
        }
        /* 其他键忽略 */
    }

    /* 恢复播放前：不清屏，直接用下一帧覆盖，避免空白帧
       同步时间基准：累计暂停时长，并调整播放起始时间，使恢复后时间对应当前帧 */
    ULONGLONG now = GetTickCount64();
    g_paused_accum += now - pause_start;
    g_play_base = now - g_paused_accum -
                  (ULONGLONG)(*current_frame * 1000 / g_config.fps);

    FlushConsoleInputBuffer(g_hIn);
}

/* 在窗口最后两行绘制进度条（第一行进度条+帧数，第二行百分比+提示） */
static void draw_progress_bar(int current_frame)
{
    int win_w = g_config.width + 10;
    int win_h = g_config.height + 10;
    int bar_row = win_h - 2;   /* 进度条占两行：bar_row 和 bar_row+1 */

    /* 显式设置白色前景色，确保进度条文字可见（RTF渲染可能改变了颜色）*/
    DWORD w;
    WriteConsoleA(g_hOut, "\x1b[38;2;255;255;255m", 19, &w, NULL);

    /* 确保窗口视口顶部为0，进度条始终在窗口底部可见 */
    CONSOLE_SCREEN_BUFFER_INFO sbi;
    GetConsoleScreenBufferInfo(g_hOut, &sbi);
    if (sbi.srWindow.Top != 0) {
        SMALL_RECT rc = {0, 0, (SHORT)(win_w - 1), (SHORT)(win_h - 1)};
        SetConsoleWindowInfo(g_hOut, TRUE, &rc);
    }

    int display_total = g_config.total_frames;

    double ratio = (double)current_frame / display_total;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    /* --- 第一行：[=====>         ](current/total) --- */
    /* 进度条区域宽度：留出 ] 和 (x/x) 的空间 */
    char count_str[64];
    snprintf(count_str, sizeof(count_str), "(%d/%d)", current_frame, display_total);
    int count_len = (int)strlen(count_str);
    int bar_width = win_w - 2 - count_len;  /* 2 = '[' 和 ']' */
    if (bar_width < 5) bar_width = 5;

    int filled = (int)(ratio * bar_width);
    if (filled > bar_width) filled = bar_width;

    char line1[512];
    int idx = 0;
    line1[idx++] = '[';
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) line1[idx++] = '=';
        else if (i == filled) line1[idx++] = '>';
        else line1[idx++] = ' ';
    }
    line1[idx++] = ']';
    /* 帧计数紧跟在 ] 后面 */
    for (int i = 0; i < count_len && idx < (int)sizeof(line1) - 2; i++)
        line1[idx++] = count_str[i];
    /* 填充到行尾 */
    while (idx < win_w - 1 && idx < (int)sizeof(line1) - 2)
        line1[idx++] = ' ';
    line1[idx] = '\0';

    COORD pos1 = {0, (SHORT)bar_row};
    SetConsoleCursorPosition(g_hOut, pos1);
    DWORD written = 0;
    WriteConsoleA(g_hOut, line1, (DWORD)strlen(line1), &written, NULL);

    /* --- 第二行：时间 + 百分比 + 操作提示 --- */
    char line2[512];
    /* 计算当前播放时间 mm:ss */
    int cur_ms = current_frame * 1000 / g_config.fps;
    int cur_mm = cur_ms / 60000;
    int cur_ss = (cur_ms % 60000) / 1000;
    /* 总时长 */
    int total_ms = display_total * 1000 / g_config.fps;
    int total_mm = total_ms / 60000;
    int total_ss = (total_ms % 60000) / 1000;
    const char *hint = g_paused
        ? "[已暂停 - 空格/ESC继续，输入帧数跳转]"
        : "[按下空格暂停，暂停后输入帧数进行跳转]";
    snprintf(line2, sizeof(line2), "%02d:%02d/%02d:%02d  %d%% %s",
             cur_mm, cur_ss, total_mm, total_ss, (int)(ratio * 100), hint);
    int len2 = (int)strlen(line2);
    if (len2 > win_w - 1) len2 = win_w - 1;
    while (len2 < win_w - 1 && len2 < (int)sizeof(line2) - 2)
        line2[len2++] = ' ';
    line2[len2] = '\0';

    COORD pos2 = {0, (SHORT)(bar_row + 1)};
    SetConsoleCursorPosition(g_hOut, pos2);
    WriteConsoleA(g_hOut, line2, (DWORD)strlen(line2), &written, NULL);

    /* 关键：光标移回 (0,0)，确保下一帧从左上角开始覆盖 */
    COORD home = {0, 0};
    SetConsoleCursorPosition(g_hOut, home);
}

/* ============================================================
 *  真彩色输出（VT100 转义序列，Windows 10+）
 * ============================================================ */
static inline void set_color(int r, int g, int b)
{
    char esc[32];
    int n = sprintf(esc, "\x1b[38;2;%d;%d;%dm", r & 0xFF, g & 0xFF, b & 0xFF);
    DWORD w;
    WriteConsoleA(g_hOut, esc, (DWORD)n, &w, NULL);
}

static inline void reset_color(void)
{
    DWORD w;
    WriteConsoleA(g_hOut, "\x1b[0m", 4, &w, NULL);
}

/* ============================================================
 *  文件格式自动检测
 * ============================================================ */
static int is_rtf_file(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return 0;
    /* 不区分大小写比较 ".rtf" */
    return (dot[1] == 'r' || dot[1] == 'R') &&
           (dot[2] == 't' || dot[2] == 'T') &&
           (dot[3] == 'f' || dot[3] == 'F') &&
           dot[4] == '\0';
}

/* ============================================================
 *  RTF 颜色表解析
 *  格式: {\colortbl ;\redR\greenG\blueB;\redR\greenG\blueB;...}
 *  索引 0 为默认色（无定义），索引从 1 开始对应 \cf1 \cf2 ...
 * ============================================================ */
static void parse_colortbl(const char *data, int len)
{
    g_num_colors = 0;

    const char *p = find_bytes(data, len, "{\\colortbl", 10);
    if (!p) return;
    p += 10;   /* 跳过 "{\colortbl" */

    /* 索引 0：默认白色（控制台默认前景色） */
    g_colortbl[0].r = 255;
    g_colortbl[0].g = 255;
    g_colortbl[0].b = 255;
    g_num_colors = 1;

    int r = 0, g = 0, b = 0;
    int have_color = 0;

    while (*p && *p != '}' && g_num_colors < MAX_COLORS) {
        if (strncmp(p, "\\red", 4) == 0) {
            p += 4;
            r = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
            have_color = 1;
        } else if (strncmp(p, "\\green", 6) == 0) {
            p += 6;
            g = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else if (strncmp(p, "\\blue", 5) == 0) {
            p += 5;
            b = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else if (*p == ';') {
            if (have_color) {
                g_colortbl[g_num_colors].r = r;
                g_colortbl[g_num_colors].g = g;
                g_colortbl[g_num_colors].b = b;
                g_num_colors++;
                have_color = 0;
            }
            p++;
        } else {
            p++;
        }
    }
}

/* 预计算每种颜色的 VT100 转义序列，渲染时直接 memcpy，不用每次 sprintf */
static void precompute_color_escapes(void)
{
    for (int i = 0; i < g_num_colors; i++) {
        g_color_esc_len[i] = sprintf(g_color_esc[i],
            "\x1b[38;2;%d;%d;%dm",
            g_colortbl[i].r, g_colortbl[i].g, g_colortbl[i].b);
    }
}

/* ============================================================
 *  RTF 渲染（带真彩色）
 *  支持: \cfN 切换颜色, \par/\line 换行, \tab 制表符,
 *        \\ \{ \} \~ 转义, {\fonttbl...} {\colortbl...} 组跳过
 *  性能优化: 文本和颜色转义序列都写入同一缓冲区，整帧只做少数次系统调用
 * ============================================================ */
static void render_rtf(const char *data, int len)
{
    /* ===== 颜色表缓存：同一批帧颜色表通常相同，避免每帧重复解析 ===== */
    const char *ctbl_start = find_bytes(data, len, "{\\colortbl", 10);
    int ctbl_len = 0;
    if (ctbl_start) {
        /* 找到颜色表结束的 }（处理嵌套） */
        const char *q = ctbl_start + 10;
        int d = 1;
        while (q < data + len && d > 0) {
            if (*q == '{') d++;
            else if (*q == '}') d--;
            q++;
        }
        ctbl_len = (int)(q - ctbl_start);

        /* 和缓存的颜色表比较，相同则跳过解析 */
        int ctbl_same = (g_cached_ctbl != NULL &&
                          g_cached_ctbl_len == ctbl_len &&
                          memcmp(g_cached_ctbl, ctbl_start, (size_t)ctbl_len) == 0);

        if (!ctbl_same) {
            parse_colortbl(data, len);
            precompute_color_escapes();
            /* 更新缓存：先分配新内存，成功后再 free 旧的，避免 malloc 失败时丢失旧缓存 */
            char *new_ctbl = (char *)malloc((size_t)ctbl_len);
            if (new_ctbl) {
                memcpy(new_ctbl, ctbl_start, (size_t)ctbl_len);
                if (g_cached_ctbl) free(g_cached_ctbl);
                g_cached_ctbl = new_ctbl;
                g_cached_ctbl_len = ctbl_len;
            }
            /* malloc 失败时保留旧缓存，下次仍可比较（虽然颜色表已变，但避免崩溃） */
        }
    } else {
        /* 没有颜色表，直接解析（会用默认色） */
        parse_colortbl(data, len);
        precompute_color_escapes();
    }

    /* 优化：跳过 RTF 头部，直接从颜色表结束后开始解析内容
       避免每帧重复扫描 {\rtf1{\fonttbl{\colortbl...} 等头部 */
    const char *p   = data;
    const char *end = data + len;
    if (ctbl_start) {
        /* 从颜色表结束位置开始（ctbl_start + ctbl_len 指向 } 之后） */
        p = ctbl_start + ctbl_len;
        /* 跳过头部结束后可能的空格和换行 */
        while (p < end && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t'))
            p++;
    }
    int depth       = 0;
    int cur_color   = 0;
    int last_output_color = -1;  /* 上一个实际输出的颜色，用于避免重复输出颜色序列 */
    int skip_file_nl = 0;   /* \par 输出换行后，跳过紧跟的文件 \r\n，避免双重换行 */

    static char outbuf[OUTBUF_SIZE];
    int outpos = 0;

    /* 渲染结果写入全局缓存 g_frame_cache，由 play_frame 统一输出（支持帧缓存命中） */
    #define FLUSH_BUF() do {                              \
        if (outpos > 0 && g_frame_cache_len + outpos < OUTBUF_SIZE) { \
            memcpy(g_frame_cache + g_frame_cache_len, outbuf, (size_t)outpos); \
            g_frame_cache_len += outpos;                  \
        }                                                 \
        outpos = 0;                                       \
    } while (0)

    /* 重置颜色 */
    #define RESET_COLOR() do {                            \
        memcpy(outbuf + outpos, "\x1b[0m", 4);            \
        outpos += 4;                                      \
    } while (0)

    /* 换行后重新输出当前颜色的 VT100 序列，防止控制台在换行时重置颜色导致断层 */
    #define EMIT_NEWLINE() do {                           \
        outbuf[outpos++] = '\n';                          \
        last_output_color = -1;  /* 换行后强制重设颜色 */ \
    } while (0)

    /* 输出当前颜色序列（仅当和上一个输出颜色不同时，减少冗余） */
    #define EMIT_COLOR() do {                             \
        if (cur_color != last_output_color &&             \
            cur_color < g_num_colors &&                   \
            g_color_esc_len[cur_color] > 0) {             \
            memcpy(outbuf + outpos, g_color_esc[cur_color], \
                   (size_t)g_color_esc_len[cur_color]);   \
            outpos += g_color_esc_len[cur_color];         \
            last_output_color = cur_color;                \
        }                                                 \
    } while (0)

    while (p < end) {
        /* ---- 花括号组处理 ---- */
        if (*p == '{') {
            /* 跳过 {\*\...} 忽略目标组（generator、picprop、xmlnstbl 等元数据） */
            if (p + 3 < end && p[1] == '\\' && p[2] == '*') {
                int d = 1; p++;
                while (p < end && d > 0) {
                    if (*p == '{') d++;
                    else if (*p == '}') d--;
                    p++;
                }
                continue;
            }
            /* 跳过 {\fonttbl ...} 整组 */
            if (p + 9 < end && strncmp(p, "{\\fonttbl", 9) == 0) {
                int d = 1; p++;
                while (p < end && d > 0) {
                    if (*p == '{') d++;
                    else if (*p == '}') d--;
                    p++;
                }
                continue;
            }
            /* 跳过 {\colortbl ...} 整组（已单独解析） */
            if (p + 10 < end && strncmp(p, "{\\colortbl", 10) == 0) {
                int d = 1; p++;
                while (p < end && d > 0) {
                    if (*p == '{') d++;
                    else if (*p == '}') d--;
                    p++;
                }
                continue;
            }
            depth++;
            p++;
            continue;
        }
        if (*p == '}') {
            if (depth > 0) depth--;
            p++;
            continue;
        }

        /* ---- 反斜杠控制字 / 转义 ---- */
        if (*p == '\\') {
            p++;
            if (p >= end) break;

            /* 简单转义字符 */
            if (*p == '\\') { outbuf[outpos++] = '\\'; p++; goto check_flush; }
            if (*p == '{')  { outbuf[outpos++] = '{';  p++; goto check_flush; }
            if (*p == '}')  { outbuf[outpos++] = '}';  p++; goto check_flush; }
            if (*p == '~')  { outbuf[outpos++] = ' ';  p++; goto check_flush; }
            if (*p == '_')  { outbuf[outpos++] = '-';  p++; goto check_flush; }

            /* 解析控制字名称（字母序列） */
            char ctrl[32];
            int ci = 0;
            while (p < end &&
                   (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) &&
                   ci < 31) {
                ctrl[ci++] = *p++;
            }
            ctrl[ci] = '\0';

            /* 处理 \* （RTF 忽略标记，常见于 {\*\generator ...} 注释组） */
            if (ci == 0 && p < end && *p == '*') {
                p++;
                goto check_flush;
            }

            /* 手动解析控制字参数（避免 atoi 函数调用开销），带上界检查防止溢出 */
            int param = -1;
            if (p < end && *p >= '0' && *p <= '9') {
                param = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    if (param > 1000000) { param = 1000000; break; }  /* 上界保护，防止 int 溢出 */
                    param = param * 10 + (*p - '0');
                    p++;
                }
            }
            /* 控制字后的分隔空格（属于控制字一部分，不输出）*/
            if (p < end && *p == ' ') p++;

            /* ---- 处理已知控制字（首字符快速过滤，减少 strcmp） ---- */
            if (ci == 2 && ctrl[0] == 'c' && ctrl[1] == 'f' && param >= 0) {
                /* \cfN：切换前景色（延迟到输出字符时才写入，减少冗余颜色序列）*/
                cur_color = param;
            } else if (ci == 3 && ctrl[0] == 'p' && ctrl[1] == 'a' && ctrl[2] == 'r') {
                /* \par：段落换行（换行后重设当前颜色，防止断层）*/
                EMIT_NEWLINE();
                skip_file_nl = 1;
            } else if (ci == 4 && ctrl[0] == 'l' && ctrl[1] == 'i' &&
                       ctrl[2] == 'n' && ctrl[3] == 'e') {
                /* \line：换行（换行后重设当前颜色，防止断层）*/
                EMIT_NEWLINE();
                skip_file_nl = 1;
            } else if (ci == 3 && ctrl[0] == 't' && ctrl[1] == 'a' && ctrl[2] == 'b') {
                /* \tab：制表符 */
                outbuf[outpos++] = '\t';
            } else if (ci == 4 && ctrl[0] == 'p' && ctrl[1] == 'a' &&
                       ctrl[2] == 'r' && ctrl[3] == 'd') {
                /* \pard：段落属性重置，恢复默认颜色 */
                RESET_COLOR();
                cur_color = 0;
                last_output_color = -1;  /* 强制下次输出字符时重设颜色 */
                skip_file_nl = 0;   /* 新段落开始，清除上一个 \par 的换行标记 */
            } else if (ci == 9 && ctrl[0] == 'g' && ctrl[1] == 'e' &&
                       ctrl[2] == 'n' && ctrl[3] == 'e' && ctrl[4] == 'r' &&
                       ctrl[5] == 'a' && ctrl[6] == 't' && ctrl[7] == 'o' &&
                       ctrl[8] == 'r') {
                /* \generator：ASCII Generator 工具的水印，跳过后面的文本直到 } 或控制字 */
                while (p < end && *p != '}' && *p != '\\' &&
                       *p != '\r' && *p != '\n')
                    p++;
            }
            /* 其他控制字（\f0, \fs, \b, \i 等）直接忽略 */
            goto check_flush;
        }

        /* ---- 普通文本字符 ---- */
        /* 智能处理文件本身的 \r\n：
           - 如果刚由 \par 输出了换行(skip_file_nl=1)，跳过紧跟的 \r\n，避免双重换行
           - 如果没有 \par（靠文件换行分行），把 \r\n 当作内容换行输出
           - \r\n 是两个字符，\r 处理时设标记，\n 跟着跳过 */
        if (*p == '\r') {
            if (skip_file_nl) {
                p++;   /* 已由 \par 换过行，跳过此 \r，\n 也会跟着跳过 */
            } else {
                EMIT_NEWLINE();   /* 没有 \par，文件换行当作内容换行，换行后重设颜色 */
                skip_file_nl = 1;          /* 避免紧跟的 \n 再换一行 */
                p++;
            }
            goto check_flush;
        }
        if (*p == '\n') {
            if (skip_file_nl) {
                skip_file_nl = 0;   /* 消耗掉 \r 留下的标记，恢复正常 */
            } else {
                EMIT_NEWLINE();   /* 单独的 \n（没有 \r），当作换行，换行后重设颜色 */
            }
            p++;
            goto check_flush;
        }
        skip_file_nl = 0;   /* 遇到普通字符，重置标记 */

        /* 输出当前颜色序列（仅当颜色变化时），再输出字符 */
        EMIT_COLOR();
        outbuf[outpos++] = *p;
        p++;

    check_flush:
        if (outpos >= OUTBUF_SIZE - 1024) FLUSH_BUF();   /* 512KB 缓冲区，留 1KB 余量 */
    }

    /* 帧结束：重置颜色（重置后立即恢复背景色） + 保存到全局缓存 */
    RESET_COLOR();
    FLUSH_BUF();

    #undef FLUSH_BUF
    #undef EMIT_NEWLINE
    #undef EMIT_COLOR
    #undef RESET_COLOR
}

/* ============================================================
 *  TXT 纯文本渲染（写入全局缓存，由 play_frame 统一输出）
 * ============================================================ */
static void render_txt(const char *data, int len)
{
    if (len > OUTBUF_SIZE - 4 - g_frame_cache_len)
        len = OUTBUF_SIZE - 4 - g_frame_cache_len;
    if (len > 0) {
        memcpy(g_frame_cache + g_frame_cache_len, data, (size_t)len);
        g_frame_cache_len += len;
    }
}

/* 把 UTF-8 缓冲区（含 VT100 序列和中文/表情）转换为 UTF-16 并用 WriteConsoleW 输出
   表情符号后插入退格符，使其只占1格宽度；转换失败时回退 WriteConsoleA */
static void write_console_utf8(const char *data, int len)
{
    if (len <= 0) return;
    /* 计算需要的 UTF-16 缓冲区大小 */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data, len, NULL, 0);
    if (wlen <= 0) {
        /* 转换失败（可能含无效UTF-8序列），回退 WriteConsoleA 直接输出 */
        DWORD w;
        WriteConsoleA(g_hOut, data, (DWORD)len, &w, NULL);
        return;
    }
    static WCHAR wbuf[1048576];  /* 2MB 静态缓冲区 */
    if (wlen > 1048575) wlen = 1048575;
    MultiByteToWideChar(CP_UTF8, 0, data, len, wbuf, wlen);

    /* 遍历 UTF-16，检测表情符号（含代理对和BMP表情），在其后插入退格符 \b 使光标只前进1格 */
    static WCHAR outbuf[1048576];
    int out_len = 0;
    for (int i = 0; i < wlen && out_len < 1048574; i++) {
        unsigned int cp = wbuf[i];
        int is_emoji = 0;
        /* 检测高代理项 (U+D800-U+DBFF)，后面紧跟低代理项构成辅助平面字符 */
        if (wbuf[i] >= 0xD800 && wbuf[i] <= 0xDBFF && i + 1 < wlen) {
            cp = 0x10000 + ((wbuf[i] - 0xD800) << 10) + (wbuf[i+1] - 0xDC00);
            /* 辅助平面表情范围：U+1F000-U+1FFFF（杂项符号、表情、交通、补充符号）*/
            if (cp >= 0x1F000 && cp <= 0x1FFFF) {
                is_emoji = 1;
            }
            /* 一次性写入高低代理项（WriteConsoleW 会组合为辅助平面字符） */
            outbuf[out_len++] = wbuf[i];      /* 高代理项 */
            outbuf[out_len++] = wbuf[i + 1];  /* 低代理项 */
            i++;  /* 跳过已处理的低代理项，for 循环的 i++ 会再前进一格 */
        } else {
            /* U+FE0F 变体选择符（emoji 呈现指示）：零宽度，跳过不输出 */
            if (cp == 0xFE0F) continue;
            /* U+2764 重黑心在控制台是宽字符（占2格）。若压缩成1格会被下一字符截断
               （画面中的心只剩左半、几乎不可见，只在行末才完整）。
               为保持 ASCII art 逐列对齐且心形完整显示，改用窄的心形 U+2665
               （等宽字体 Consolas 下占1格）。 */
            if (cp == 0x2764) {
                outbuf[out_len++] = 0x2665;
                continue;  /* 不插退格，正常占1格 */
            }
            /* BMP 字符：直接写入 */
            outbuf[out_len++] = wbuf[i];
            /* BMP 中的表情/符号范围（控制台双列宽字符，需插入退格符） */
            if ((cp >= 0x203C && cp <= 0x2049) ||
                (cp >= 0x2122 && cp <= 0x2139) ||
                (cp >= 0x2194 && cp <= 0x2199) ||
                (cp >= 0x2300 && cp <= 0x23FF) ||
                (cp == 0x24C2) ||
                (cp >= 0x25AA && cp <= 0x25AB) ||
                (cp >= 0x25B6 && cp <= 0x25C0) ||
                (cp >= 0x25FB && cp <= 0x25FE) ||
                (cp >= 0x2600 && cp <= 0x26FF) ||
                (cp >= 0x2700 && cp <= 0x27BF) ||
                (cp >= 0x2934 && cp <= 0x2935) ||
                (cp >= 0x2B00 && cp <= 0x2BFF) ||
                (cp == 0x3030 || cp == 0x303D) ||
                (cp == 0x3297 || cp == 0x3299)) {
                is_emoji = 1;
            }
        }
        if (is_emoji && out_len < 1048574) {
            outbuf[out_len++] = L'\b';  /* 退格，抵消表情的第二格宽度 */
        }
    }
    DWORD w;
    WriteConsoleW(g_hOut, outbuf, (DWORD)out_len, &w, NULL);
}

/* ============================================================
 *  把匹配帧号的覆盖字符嵌入渲染输出缓冲区（副本）
 *  覆盖字符作为帧内容的一部分输出（同一 WriteConsoleW 通道），
 *  从根本上避免"覆盖已写入屏幕缓冲区但显示层不可见"的问题。
 *  buf 是渲染输出的 UTF-8 文本：行以 \n 结尾，行内可含 \x1b[...m VT 颜色序列。
 *  只修改副本，不污染 g_frame_cache（缓存保持原始帧内容，可重复使用）。
 *  ------------------------------------------------------------
 *  行/列/帧表达式为 0-based 
 *  首行/首列 = 0 
 * ============================================================ */
static void embed_overrides_into(char *buf, int *len, int frame)
{
    if (!buf || *len <= 0 || g_override_count == 0) return;
    if (*len >= OUTBUF_SIZE - 4096) return;  /* 空间不足，放弃嵌入 */

    /* 解析一次行索引（嵌入会改变字节长度，后续按行维护偏移）
       行数 = \n 数量 + 1（帧内容可能不以 \n 结尾，最后一行也要计入；
       若以 \n 结尾则最后一个空行不计入） */
    #define MAX_EMBED_ROWS 1024
    static char *row_start[MAX_EMBED_ROWS];
    int nrows = 1;  /* 至少一行 */
    row_start[0] = buf;
    for (int i = 0; i < *len; i++) {
        if (buf[i] == '\n') {
            if (nrows < MAX_EMBED_ROWS) row_start[nrows] = buf + i + 1;
            nrows++;
            if (nrows >= MAX_EMBED_ROWS) break;
        }
    }
    if (nrows >= MAX_EMBED_ROWS) return;  /* 行过多，放弃嵌入 */
    /* 若最后一行是空行（buf 以 \n 结尾），不计入显示行 */
    if (nrows > 1 && row_start[nrows - 1] >= buf + *len)
        nrows--;

    int win_w = g_config.width + 10;
    int win_h = g_config.height + 10;

    for (int oi = 0; oi < g_override_count; oi++) {
        if (!frame_matches(g_overrides[oi].expr, frame)) continue;

        /* 收集匹配的行（限于帧内实际行数）和列 */
        int rows[512], nrows_match = 0;
        int cols[512], ncols_match = 0;
        int rmax = (nrows < win_h) ? nrows : win_h;
        for (int r = 0; r < rmax && nrows_match < 512; r++)
            if (pos_matches(g_overrides[oi].row_expr, r))
                rows[nrows_match++] = r;
        for (int c = 0; c < win_w && ncols_match < 512; c++)
            if (pos_matches(g_overrides[oi].col_expr, c))
                cols[ncols_match++] = c;

        for (int ri = 0; ri < nrows_match; ri++) {
            int row = rows[ri];
            if (row < 0 || row >= nrows) continue;
            char *rp = row_start[row];

            for (int ci = 0; ci < ncols_match; ci++) {
                int col = cols[ci];
                /* 每列重新计算行尾（行内嵌入会改变行尾偏移） */
                char *re = (row + 1 < nrows) ? row_start[row + 1] : buf + *len;
                if (re > rp && re[-1] == '\n') re--;
                if (re <= rp) continue;

                /* 定位第 col 个显示列（跳过 VT 颜色序列；字符显示宽度
                   与 utf8_char_info / write_console_utf8 渲染规则一致，
                   宽字符 2 格、表情 1 格、变体选择符 0 格） */
                int dcol = 0;
                char *ins = NULL;
                char *q = rp;
                while (q < re && dcol < col) {
                    if (*q == '\x1b') {  /* VT 序列：跳过到 m */
                        q += 2;
                        while (q < re && *q != 'm') q++;
                        if (q < re) q++;
                        continue;
                    }
                    int blen = 1;
                    int w = utf8_char_info((const unsigned char *)q, &blen);
                    if (dcol + w > col) break;  /* 目标列落在此字符内：停在其起始 */
                    dcol += w;
                    q += blen;
                }
                /* dcol == col 恰好对齐；dcol < col 说明目标列落在宽字符中间，
                   从该字符起始处嵌入（替换整个字符，不切开它） */
                if (dcol == col) ins = q;
                else if (q < re) ins = q;
                if (!ins || ins >= re) continue;
                /* 若 ins 落在行首 VT 颜色序列上（col=0 且行首是 \x1b[...m），
                   跳到序列之后的显示字符再嵌入，避免覆盖 \x1b 导致颜色序列
                   被破坏并以文本形式显示 */
                while (ins < re && *ins == '\x1b') {
                    ins += 2;
                    while (ins < re && *ins != 'm') ins++;
                    if (ins < re) ins++;
                }
                if (ins >= re) continue;  /* 行尾，无法嵌入 */

                /* 覆盖字符（UTF-8）及其占用的显示列数 */
                const char *ch = g_overrides[oi].ch;
                int ch_len = (int)strlen(ch);
                if (ch_len == 0) continue;
                int test_w = utf8_display_width(ch);
                if (test_w < 1) test_w = 1;

                /* 配置颜色时：在覆盖字符前插入 VT 真彩序列；
                   test 之后插入 ins 前的颜色序列作为恢复，保证
                   该行其他字符颜色不变 */
                char pre[40] = "";
                int pre_len = 0;
                char restore[40] = "\x1b[39m";  /* 找不到则恢复默认色 */
                int restore_len = 4;
                if (g_overrides[oi].color_set) {
                    snprintf(pre, sizeof(pre), "\x1b[38;2;%d;%d;%dm",
                             g_overrides[oi].color_r, g_overrides[oi].color_g,
                             g_overrides[oi].color_b);
                    pre_len = (int)strlen(pre);
                }
                /* 扫描 ins 之前的最后一个 VT 颜色序列，作为恢复序列 */
                {
                    char *s = rp;
                    while (s < ins) {
                        if (*s == '\x1b') {
                            char *e = s + 2;
                            while (e < ins && *e != 'm') e++;
                            if (e < ins && *e == 'm') {
                                int len = (int)(e - s + 1);
                                if (len < (int)sizeof(restore)) {
                                    memcpy(restore, s, (size_t)len);
                                    restore[len] = '\0';
                                    restore_len = len;
                                }
                            }
                            s = (e < ins) ? (e + 1) : ins;
                        } else {
                            s++;
                        }
                    }
                }

                /* 计算 ins 起 test_w 列占用的字节数（跳过 VT 序列；
                   显示宽度与 utf8_char_info 一致） */
                int seg_len = 0;
                {
                    char *s = ins;
                    int dc = 0;
                    while (s < re && dc < test_w) {
                        if (*s == '\x1b') {
                            s += 2;
                            while (s < re && *s != 'm') s++;
                            if (s < re) s++;
                            continue;
                        }
                        int blen = 1;
                        int w = utf8_char_info((const unsigned char *)s, &blen);
                        if (s + blen > re) break;  /* 字符被行尾截断：不替换 */
                        if (dc + w > test_w) break;  /* 余下列数不足一格，不切开 */
                        dc += w;
                        s += blen;
                    }
                    seg_len = (int)(s - ins);
                }

                /* 新内容 = pre + ch + restore，覆盖替换 ins 起 test_w 列的原字符。
                   字节长度变化由 memmove 右移/左移吸收，但 VT 序列不占显示列，
                   所以屏幕上其他字符的显示位置不变 */
                int new_seg_len = pre_len + ch_len + restore_len;
                int delta = new_seg_len - seg_len;
                if (delta != 0) {
                    if (*len + delta >= OUTBUF_SIZE) continue;  /* 空间不足 */
                    char *src = ins + seg_len;
                    char *dst = ins + new_seg_len;
                    int move_len = (int)(buf + *len - src);
                    if (move_len > 0) memmove(dst, src, (size_t)move_len);
                    *len += delta;
                    /* 该行之后所有行的起始偏移随字节变化同步更新 */
                    for (int j = row + 1; j <= nrows; j++)
                        row_start[j] += delta;
                }
                if (pre_len > 0) memcpy(ins, pre, (size_t)pre_len);
                memcpy(ins + pre_len, ch, (size_t)ch_len);
                if (restore_len > 0)
                    memcpy(ins + pre_len + ch_len, restore, (size_t)restore_len);
            }
        }
    }
}

/* ============================================================
 *  读取并渲染一帧（自动检测 TXT / RTF，支持帧缓存命中）
 *  返回: 1=成功, 0=文件不存在或读取失败
 * ============================================================ */
static int play_frame(const char *filepath)
{
    /* 从文件路径中提取帧索引，用于缓存命中检查
       格式: resource\frame(N).txt 或 resource\frame(N).rtf */
    int frame_idx = -1;
    const char *p1 = strstr(filepath, "frame(");
    if (p1) {
        p1 += 6;  /* 跳过 "frame(" */
        frame_idx = atoi(p1);
    }

    /* 帧缓存命中：直接输出缓存，跳过文件读取和 RTF 解析
       注意：顺序播放时帧索引递增，缓存通常不命中；
       缓存主要在帧丢失回退（用上一帧代替）和暂停跳转时生效 */
    if (frame_idx >= 0 && frame_idx == g_frame_cache_idx && g_frame_cache_len > 0) {
        /* 嵌入覆盖到副本后输出（不污染缓存，缓存保持原始帧内容）*/
        static char embed_buf[OUTBUF_SIZE];
        int new_len = g_frame_cache_len;
        memcpy(embed_buf, g_frame_cache, (size_t)new_len);
        embed_overrides_into(embed_buf, &new_len, frame_idx);
        write_console_utf8(embed_buf, new_len);
        return 1;
    }

    FILE *fp = wfopen(filepath, "rb");
    if (!fp) return 0;

    /* 读取文件全部内容 */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0) { fclose(fp); return 0; }
    if (fsize > 1024 * 1024) fsize = 1024 * 1024;   /* 上限 1MB */

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(fp); return 0; }

    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    buf[nread] = '\0';
    fclose(fp);

    /* 重置全局缓存 */
    g_frame_cache_len = 0;

    /* ---- 自动检测格式并渲染（渲染结果写入 g_frame_cache） ---- */
    if (is_rtf_file(filepath)) {
        render_rtf(buf, (int)nread);
    } else {
        render_txt(buf, (int)nread);
    }

    free(buf);

    /* 更新缓存索引 */
    if (frame_idx >= 0)
        g_frame_cache_idx = frame_idx;

    /* 一次性输出整帧（UTF-8 -> UTF-16，支持中文和表情）
       覆盖字符嵌入到副本后与帧内容同一通道输出，确保覆盖显示 */
    if (g_frame_cache_len > 0) {
        static char embed_buf[OUTBUF_SIZE];
        int new_len = g_frame_cache_len;
        memcpy(embed_buf, g_frame_cache, (size_t)new_len);
        embed_overrides_into(embed_buf, &new_len, frame_idx);
        write_console_utf8(embed_buf, new_len);
    }
    return 1;
}

/* ============================================================
 *  主函数
 * ============================================================ */
/* ============================================================
 *  控制台关闭 / Ctrl+C 清理处理
 *  确保用户强制关闭时停止 MCI 音频和 BGM 乐谱，避免后台继续播放
 * ============================================================ */
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
{
    (void)ctrl_type;
    /* 注意：此回调可能在任意线程上被调用，与主线程存在潜在竞态。
       但由于进程即将退出，实际影响很小。std::atomic 保证单个操作原子性。 */
    /* 停止 MCI 音频 */
    mciSendStringW(L"stop s1", NULL, 0, NULL);
    mciSendStringW(L"close s1", NULL, 0, NULL);
    /* 停止 BGM 乐谱 */
    if (g_bgm) { g_bgm->stop(); delete g_bgm; g_bgm = NULL; }
    /* 释放 RTF 颜色表缓存 */
    if (g_cached_ctbl) { free(g_cached_ctbl); g_cached_ctbl = NULL; g_cached_ctbl_len = 0; }
    /* 恢复系统计时精度 */
    timeEndPeriod(1);
    /* 返回 TRUE 表示已处理，程序退出 */
    return TRUE;
}

/* ============================================================
 *  main 入口: 初始化 -> 选歌 -> 音频 -> 主循环
 * ============================================================ */
/* ============================================================
 * 选歌界面帮助：输入 0 查看（内容与 start.cpp 文件头注释相似）
 * 显示后按回车返回选歌界面；autoplay.txt 中的 0 不受影响，
 * 仍表示"正常进入选歌界面"。
 * ============================================================ */
static void show_help(void)
{
    printf(
        "\n-----ASCII Art Player 帮助-----\n"
        "\n  功能特性:\n"
        "    TXT/RTF 帧动画播放（RTF 彩色/水印自动隐藏）\n"
        "    多歌曲目录: exe 同目录下每个子文件夹为一首歌\n"
        "    MCI 音频 / BGM 简谱乐谱 两种播放模式\n"
        "    暂停/恢复（空格/ESC），暂停后输入帧号+回车跳转\n"
        "    进度条（帧号/百分比/mm:ss），SRT 字幕（双行/偏移）\n"
        "    真实时钟跳帧，帧丢失自动回退上一帧\n"
        "    emoji/中文路径全链路支持，粗体 Consolas\n"
        "\n  目录结构:\n"
        "    歌曲目录/\n"
        "      音乐.mp3       自动检测的音频文件\n"
        "      music.txt      use_bgm=1 时的简谱乐谱\n"
        "      歌词.srt       字幕（可选）\n"
        "      config.ini     播放配置\n"
        "      override.txt   帧覆盖（单独，优先于全局）\n"
        "      resource/      frame(N).txt 或 .rtf\n"
        "    exe 同目录/\n"
        "      override.txt   全局帧覆盖\n"
        "      autoplay.txt   0=选歌界面，N=直接播放第 N 首\n"
        "\n  config.ini 配置项:\n"
        "    width/height     窗口大小（实际 = 配置 + 10）\n"
        "    debug            调试模式（显示帧格式/跳帧统计）\n"
        "    fps              播放帧率\n"
        "    use_bgm          0=MCI 音频，1=BGM 简谱\n"
        "    total_frames     总帧数\n"
        "    font_size        播放字号\n"
        "    subtitle_offset  字幕偏移（正数=延迟，负数=提前）\n"
        "\n  override.txt 条目格式:\n"
        "    帧 行 列 字符 [protected] [颜色]\n"
        "    帧/行/列支持: 单值(100)、区间(100-200)、\n"
        "      取模(%%2==0)、取模+区间(%%2==0(10-20))\n"
        "    字符可用\"\"包裹以包含空格；颜色=R,G,B；\n"
        "    protected=1 使该帧不可被跳帧跳过\n"
        "\n  操作说明:\n"
        "    空格/ESC            暂停/恢复\n"
        "    暂停后输入帧号+回车  跳转到指定帧\n"
        "    进度条: [=====>         ](x/x)\n"
        "    x% mm:ss 显示百分比与已播放时间\n"
        "\n  override.txt 示例:\n"
        "    0-5700 82 148 \"2 25\" 0 210,20,20\n"
        "    100-200 5 100 ★ 1 255,0,0\n"
        "    含义: 100~200帧第5行第100列显示★，\n"
        "    红色且受保护不可跳过；颜色=R,G,B\n"
        "\n  调试与编译:\n"
        "    debug=1 时显示帧格式检测与跳帧统计\n"
        "    编译: g++ start.cpp -o start.exe\n"
        "          -static-libgcc -static-libstdc++\n"
        "          -lwinmm -lpthread\n"
        "\n  提示:\n"
        "    帮助仅在选歌界面输入 0 可用；\n"
        "    autoplay.txt 中的 0 表示正常选歌界面\n"
        "  播放中按 0 无效果\n"
        "\n  按回车键返回选歌界面\n");
}


int main(void)
{
    /* 注册控制台控制处理（Ctrl+C / 关闭按钮 / 注销 / 关机）*/
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    get_exe_dir();   /* 记录 exe 所在目录，用于定位全局 override.txt */

    /* 提高计时/睡眠精度：timeBeginPeriod(1) 使 Sleep(1) 实际精度约 1ms
       （默认约 15.6ms），保证高帧率下帧时序稳定；退出前 timeEndPeriod(1) */
    timeBeginPeriod(1);

    int  i          = 0;
    char filename[256];
    char filepath[256];
    char audio_file[512] = "";   /* 自动检测到的音频文件名（UTF-8，MCI模式）*/
    g_bgm = NULL;              /* BGM 乐谱播放器（use_bgm=1 时使用） */

    /* 提前初始化控制台句柄，选歌界面需要用 WriteConsoleW 输出 emoji 目录名 */
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hIn  = GetStdHandle(STD_INPUT_HANDLE);

    /* 设置控制台输入输出代码页为 UTF-8，确保 emoji 和中文正确显示 */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /* 选歌界面：设置字号为16，粗体 Consolas，窗口大小 100*40 */
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_FONT_INFOEX fi;
        fi.cbSize        = sizeof(CONSOLE_FONT_INFOEX);
        fi.nFont         = 0;
        fi.dwFontSize.X  = 0;
        fi.dwFontSize.Y  = 16;
        fi.FontFamily    = TMPF_TRUETYPE;
        fi.FontWeight    = FW_BOLD;
        wcscpy(fi.FaceName, L"Consolas");
        SetCurrentConsoleFontEx(hOut, FALSE, &fi);
        /* 设置窗口大小 100*40 */
        COORD buf = {100, 200};
        SetConsoleScreenBufferSize(hOut, buf);
        SMALL_RECT rc = {0, 0, 99, 39};
        SetConsoleWindowInfo(hOut, TRUE, &rc);
    }

    /* ===== 第一步：扫描 exe 同目录下的子目录，作为歌曲列表 ===== */
    SongList songs;
    int song_count = scan_song_dirs(&songs);

    if (song_count == 0) {
        puts("========================================");
        puts("  错误：未找到任何歌曲目录！");
        puts("========================================");
        puts("  请在 exe 同目录下创建子文件夹，每个子文件夹放一首歌：");
        puts("    歌曲名/");
        puts("      ├─ 音乐.mp3");
        puts("      ├─ config.ini");
        puts("      └─ resource/");
        puts("           ├─ frame(0).txt");
        puts("           └─ frame(1).txt ...");
        puts("");
        puts("  按回车键退出...");
        clear_input_buffer();
        getchar();
        return 1;
    }

    /* ===== 第二步：选择歌曲 =====
     * exe 同目录 autoplay.txt：0=正常显示选歌界面；N=直接进入第 N 首 */
    int autoplay = read_autoplay_index();
    int choice = 0;

    if (autoplay >= 1 && autoplay <= song_count) {
        /* 自动播放：跳过选歌菜单，直接进入对应歌曲 */
        choice = autoplay;
    } else {
        /* 正常选歌界面 */
        puts("-----ASCII Art Player-----");
        for (;;) {
            puts("  请选择歌曲：");
            for (int s = 0; s < song_count; s++) {
                /* 宽字符目录名转 UTF-8，统一用 printf 输出，支持 emoji */
                char name_utf8[512];
                wide_to_utf8(songs.names[s], name_utf8, sizeof(name_utf8));
                printf("    [%d] %s\n", s + 1, name_utf8);
            }
            printf("  输入编号 (1-%d)，输入 0 查看帮助: ", song_count);
            clear_input_buffer();
            int tmp;
            if (scanf("%d", &tmp) != 1 || tmp < 0 || tmp > song_count) {
                /* 输入无效，默认选第一首 */
                choice = 1;
                break;
            }
            getchar();  /* 消耗 scanf 残留的换行 */
            if (tmp == 0) {
                /* 查看帮助：清屏显示帮助，按回车返回选歌界面 */
                system("cls");
                show_help();
                puts("  按回车键返回选歌界面...");
                clear_input_buffer();
                getchar();
                system("cls");
                continue;
            }
            choice = tmp;
            break;
        }
    }

    /* ===== 第三步：进入选中的歌曲目录（宽字符版本，支持 emoji 路径） ===== */
    if (!SetCurrentDirectoryW(songs.names[choice - 1])) {
        char name_utf8[512];
        wide_to_utf8(songs.names[choice - 1], name_utf8, sizeof(name_utf8));
        printf("  错误：无法进入目录 %s\n", name_utf8);
        puts("  按回车键退出...");
        clear_input_buffer();
        getchar();
        return 1;
    }

    /* ===== 第四步：在歌曲目录中自动查找音频文件（宽字符） ===== */
    wchar_t audio_file_w[256];
    if (!find_audio_file(audio_file_w, 256)) {
        puts("  警告：未找到音频文件，将无声播放");
        audio_file_w[0] = L'\0';
    }
    /* 转换为 UTF-8 供后续使用（MCI 命令等） */
    wide_to_utf8(audio_file_w, audio_file, sizeof(audio_file));

    /* 自动检测并加载 .srt 歌词文件（宽字符路径） */
    wchar_t srt_file_w[256];
    g_srt_count = 0;
    if (find_srt_file(srt_file_w, 256)) {
        char srt_file[512];
        wide_to_utf8(srt_file_w, srt_file, sizeof(srt_file));
        int n = load_srt(srt_file);
        if (g_config.debug)
            printf("  已加载歌词: %s (%d条)\n", srt_file, n);
    }

    /* ===== 第五步：读取该歌曲目录下的配置文件，初始化控制台 ===== */
    load_config("config.ini");
    /* 帧字符覆盖：先加载 exe 同目录的全局 override.txt（应用于所有歌曲），
       再加载歌曲目录的 override.txt（单独配置，覆盖全局相同位置）*/
    {
        char global_override[1100];
        if (g_exe_dir[0])
            snprintf(global_override, sizeof(global_override), "%s\\override.txt", g_exe_dir);
        else
            strcpy(global_override, "override.txt");
        load_overrides(global_override, 1);  /* 全局覆盖，清空后加载 */
    }
    load_overrides("override.txt", 0);  /* 歌曲单独覆盖，覆盖全局相同位置 */
    init_console();

    /* ===== 开场界面 ===== */
    system("cls");
    {
        /* 宽字符文件夹名转 UTF-8，显示在标题中 */
        char title_utf8[512];
        wide_to_utf8(songs.names[choice - 1], title_utf8, sizeof(title_utf8));
        printf("----- %s -----\n", title_utf8);
    }
    if (autoplay == 0) {
        /* 手动选歌：按回车开始播放 */
        puts("Press Enter to play.");
        clear_input_buffer();
        getchar();
    }
    /* autoplay != 0：自动播放，跳过按回车等待，直接开始 */

    /* ===== 播放背景音乐 ===== */
    if (g_config.use_bgm) {
        /* BGM 模式：播放歌曲目录下的乐谱 music.txt */
        FILE *ftest = wfopen("music.txt", "r");
        if (ftest) {
            fclose(ftest);
            g_bgm = new BGM("music.txt");
            g_bgm->play();
        } else {
            puts("  警告：未找到 music.txt 乐谱文件，将无声播放");
        }
    } else {
        /* MCI 模式：打开音频文件（宽字符版本，支持 emoji 文件名）
           根据扩展名选择 MCI 设备类型：.wav→waveaudio, .mid/.midi→sequencer, 其他→mpegvideo
           play 延迟到帧格式检测后，给设备初始化时间 */
        if (audio_file[0] != '\0') {
            wchar_t wname[512];
            utf8_to_wide(audio_file, wname, 512);
            /* 根据扩展名选择 MCI 设备类型 */
            const wchar_t *mci_type = L"mpegvideo";  /* 默认：mp3/wma/aac/flac/ogg 等 */
            const char *ext = strrchr(audio_file, '.');
            if (ext) {
                if (_stricmp(ext, ".wav") == 0) mci_type = L"waveaudio";
                else if (_stricmp(ext, ".mid") == 0 || _stricmp(ext, ".midi") == 0) mci_type = L"sequencer";
            }
            wchar_t mci_cmd[1024];
            swprintf(mci_cmd, 1024, L"open \"%s\" type %s alias s1", wname, mci_type);
            MCIERROR mci_err = mciSendStringW(mci_cmd, NULL, 0, NULL);
            if (mci_err != 0) {
                /* 指定类型失败，尝试不指定 type 让 MCI 自动检测 */
                swprintf(mci_cmd, 1024, L"open \"%s\" alias s1", wname);
                mci_err = mciSendStringW(mci_cmd, NULL, 0, NULL);
            }
            if (mci_err != 0) {
                wchar_t err_buf[256] = L"";
                mciGetErrorStringW(mci_err, err_buf, 256);
                printf("  警告：音频打开失败（错误码 %lu），将无声播放\n", (unsigned long)mci_err);
                g_audio_ok = 0;
            } else {
                g_audio_ok = 1;
                /* 显式打开左右声道，确保双声道输出 */
                mciSendStringW(L"set s1 audio left on", NULL, 0, NULL);
                mciSendStringW(L"set s1 audio right on", NULL, 0, NULL);
            }
        }
    }

    system("cls");

    /* ===== 播放前检查：自动检测帧格式（先试 .txt，再试 .rtf） ===== */
    const char *frame_ext = NULL;   /* 检测到的帧扩展名 */
    {
        char test_path[256];

        /* 先尝试 .txt */
        strcpy(test_path, "resource\\frame(0).txt");
        FILE *test = wfopen(test_path, "rb");
        if (test) {
            fclose(test);
            frame_ext = ".txt";
        }

        /* .txt 不存在，再尝试 .rtf */
        if (!frame_ext) {
            strcpy(test_path, "resource\\frame(0).rtf");
            test = wfopen(test_path, "rb");
            if (test) {
                fclose(test);
                frame_ext = ".rtf";
            }
        }

        /* 两种都找不到，报错退出 */
        if (!frame_ext) {
            if (g_bgm) { g_bgm->stop(); delete g_bgm; g_bgm = NULL; }
            else { mciSendStringW(L"close s1", NULL, 0, NULL); }
            puts("========================================");
            puts("  错误：找不到帧文件！");
            puts("========================================");
            puts("  已自动尝试以下路径：");
            puts("    resource\\frame(0).txt");
            puts("    resource\\frame(0).rtf");
            puts("");
            puts("  可能原因：");
            puts("    1. resource 文件夹不存在");
            puts("    2. 帧文件名不对（应为 frame(0).txt 或 frame(0).rtf）");
            puts("    3. 帧文件放在了其他目录");
            puts("");
            puts("  请检查后重新运行。");
            puts("  按回车键退出...");
            clear_input_buffer();
            getchar();
            return 1;
        }

        if (g_config.debug)
            printf("  检测到帧格式: %s\n", frame_ext + 1);  /* 跳过开头的点 */
    }

    /* 此时音频设备已在后台完成初始化，开始播放 */
    if (!g_config.use_bgm && audio_file[0] != '\0' && g_audio_ok) {
        if (g_audio_ok) mciSendStringW(L"play s1", NULL, 0, NULL);  /* 音频只播放一次 */
    }

    /* 基于真实时钟的时间基准：elapsed = now - g_play_base - g_paused_accum */
    g_play_base = GetTickCount64();
    g_paused_accum = 0;
    g_skip_range_count = 0;
    g_total_skipped = 0;

    /* === 立即渲染第 0 帧，避免主循环起始处 target_frame<=i 导致黑屏 === */
    {
        char first_path[256];
        snprintf(first_path, sizeof(first_path), "resource\\frame(0)%s", frame_ext);
        play_frame(first_path);  /* 覆盖字符已随帧内容嵌入输出 */
        g_screen_frame = 0;  /* 屏幕当前显示 frame(0) */
        draw_lyric(0);
        draw_progress_bar(0);
    }

    /* ============================================================
     *  播放主循环（基于真实时钟，自动跳帧保证时间准确）
     *  ------------------------------------------------------------
     *  每轮迭代步骤:
     *    a. 检查是否播完全部帧（i > total_frames）-> 进入结束画面
     *    b. 非阻塞处理控制台输入（空格/ESC 触发暂停）
     *    c. 暂停则进入 paused_interaction（阻塞式，输入帧号跳转）
     *    d. 用 GetTickCount64 计算经过毫秒，换算成目标帧号
     *       target_frame = elapsed * fps / 1000
     *    e. target_frame <= i 说明还没到下一帧时间：精确 Sleep 等待
     *    f. target_frame 超前则直接跳到该帧渲染（跳帧追赶，保证
     *       画面时刻与音频位置同步，不拖帧）
     *    g. 构建 frame(N) 路径 -> play_frame（内部 TXT/RTF 检测 +
     *       覆盖字符嵌入）-> draw_lyric -> draw_progress_bar
     *    h. 帧文件缺失时回退上一帧画面，保护帧不被跳过
     *
     *  时间基准: g_play_base 记录开始播放的 GetTickCount64 值，
     *  g_paused_accum 累计暂停时长，elapsed = now - base - paused，
     *  保证暂停后时间不虚增。
     * ============================================================ */
    while (1) {
        /* 播放完所有帧结束，进入结束画面 */
        if (i > g_config.total_frames)
            break;

        /* --- 处理输入事件（空格/ESC 暂停） --- */
        handle_console_input();

        /* --- 暂停状态：进入交互模式，不刷新屏幕 --- */
        if (g_paused) {
            paused_interaction(&i, frame_ext, audio_file);
            continue;
        }

        /* --- 基于真实时钟计算当前应显示的帧 --- */
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsed = now - g_play_base - g_paused_accum;
        int target_frame = (int)(elapsed * g_config.fps / 1000);

        /* 还没到下一帧：精确计算还需等待多久，避免 Sleep(1) 频繁轮询 */
        if (target_frame <= i) {
            ULONGLONG next_frame_ms = (ULONGLONG)((i + 1) * 1000 / g_config.fps);
            if (next_frame_ms > elapsed) {
                ULONGLONG wait_ms = next_frame_ms - elapsed;
                if (wait_ms > 100) wait_ms = 100;  /* 最长睡 100ms，防止暂停后睡太久 */
                Sleep((DWORD)wait_ms);
            } else {
                Sleep(1);
            }
            continue;
        }

        /* 跳帧：如果渲染慢导致落后，直接跳到目标帧
           跳帧统计仅在 debug 模式下记录，避免影响正常播放速度
           注意：受保护帧（有覆盖规则的帧）不可跳过，会强制渲染 */
        if (target_frame > i + 1) {
            /* 检查跳过范围内是否有受保护帧，有则限制跳转范围 */
            int actual_target = target_frame;
            int first_protected = -1;
            for (int f = i + 1; f < target_frame; f++) {
                if (is_frame_protected(f)) {
                    first_protected = f;
                    break;
                }
            }
            if (first_protected >= 0) {
                /* 限制跳转到受保护帧（下一循环会渲染它）*/
                actual_target = first_protected;
            }
            if (g_config.debug && actual_target > i + 1) {
                int skip_start = i + 1;
                int skip_end = actual_target - 1;
                g_total_skipped += skip_end - skip_start + 1;
                if (g_skip_range_count < MAX_SKIP_RANGES) {
                    if (g_skip_range_count > 0 &&
                        g_skip_ranges[g_skip_range_count - 1][1] + 1 == skip_start) {
                        g_skip_ranges[g_skip_range_count - 1][1] = skip_end;
                    } else {
                        g_skip_ranges[g_skip_range_count][0] = skip_start;
                        g_skip_ranges[g_skip_range_count][1] = skip_end;
                        g_skip_range_count++;
                    }
                }
            }
            target_frame = actual_target;
        }
        i = target_frame;

        /* 每帧渲染前光标归位到 (0,0)，直接覆盖上一帧内容 */
        COORD frame_home = {0, 0};
        SetConsoleCursorPosition(g_hOut, frame_home);

        /* --- 构造文件名: resource\frame(N) + 自动检测到的扩展名 --- */
        int frame_idx = i;
        /* 用 snprintf 安全构建路径，避免溢出 */
        snprintf(filepath, sizeof(filepath), "resource\\frame(%d)%s", frame_idx, frame_ext);

        /* 播放当前帧（内部自动检测 TXT / RTF） */
        if (play_frame(filepath)) {
            g_screen_frame = frame_idx;
        } else {
            /* 当前帧文件丢失 */
            if (g_screen_frame < 0) {
                /* 屏幕还没有任何帧内容，必须找一个存在的帧渲染上去 */
                for (int f = frame_idx - 1; f >= 0; f--) {
                    char prev_path[256];
                    snprintf(prev_path, sizeof(prev_path), "resource\\frame(%d)%s", f, frame_ext);
                    if (play_frame(prev_path)) {
                        g_screen_frame = f;
                        break;
                    }
                }
            }
            /* 若屏幕已有帧内容（g_screen_frame>=0），则不重渲整屏，
               屏幕保持上一帧的画面（含其嵌入的覆盖），覆盖切换延迟到
               下一帧实际重渲时生效 */
        }

        /* 应用帧字符覆盖。
           覆盖字符已在 play_frame 内嵌入帧渲染数据流输出（整屏重渲路径直接可见；
           帧丢失路径屏幕保持上一帧，含其嵌入的覆盖，不做 apply 直写——嵌入按显示
           列定位、直写按绝对坐标，若列落在宽字符中间两者会错位导致覆盖两遍，
           覆盖切换延迟到下一帧实际重渲时生效）。 */
        g_last_override_count = 0;    /* 旧覆盖位置失效（嵌入方案无需直写恢复） */
        recursurv();                  /* 光标回左上角，下一帧覆盖 */

        /* 在画面和进度条之间渲染当前歌词 */
        {
            int current_ms = i * 1000 / g_config.fps;
            draw_lyric(current_ms);
        }

        /* 绘制底部进度条（包含帧号、百分比、操作提示） */
        draw_progress_bar(i);

        /* 覆盖应用后强制视口顶部归 0：
           帧渲染输出可能使屏幕缓冲区滚动（视口顶部 > 0），
           (row,col) 覆盖字符写入缓冲区但会显示在视口外的行。
           强制视口显示 (0,0) 起，确保覆盖字符在可见区域内。 */
        {
            CONSOLE_SCREEN_BUFFER_INFO sbi;
            GetConsoleScreenBufferInfo(g_hOut, &sbi);
            if (sbi.srWindow.Top != 0) {
                SMALL_RECT rc = {0, 0, (SHORT)(g_config.width + 9),
                                 (SHORT)(g_config.height + 9)};
                SetConsoleWindowInfo(g_hOut, TRUE, &rc);
            }
        }
    }

    /* 停止并关闭背景音乐 */
    if (g_bgm) { g_bgm->stop(); delete g_bgm; g_bgm = NULL; }
    else { mciSendStringW(L"stop s1", NULL, 0, NULL);
           mciSendStringW(L"close s1", NULL, 0, NULL); }

    /* ===== 结束画面 ===== */
    system("cls");
    puts("-----ASCII Art Player-----");
    puts("Thanks for watching!");
    puts("Made by lmx.");
    puts("Press Enter to Exit.");

    /* ===== 调试模式：输出跳帧统计（在结束画面之后） ===== */
    if (g_config.debug && g_total_skipped > 0) {
        printf("\n===== 跳帧统计 =====\n");
        printf("  总跳过帧数: %d / %d (%.1f%%)\n",
               g_total_skipped, g_config.total_frames,
               (double)g_total_skipped * 100.0 / g_config.total_frames);
        printf("  跳过区间数: %d\n", g_skip_range_count);
        printf("  跳过的帧:\n");
        for (int si = 0; si < g_skip_range_count; si++) {
            if (g_skip_ranges[si][0] == g_skip_ranges[si][1])
                printf("    %d\n", g_skip_ranges[si][0]);
            else
                printf("    %d - %d\n", g_skip_ranges[si][0], g_skip_ranges[si][1]);
        }
        printf("====================\n");
    }

    clear_input_buffer();
    getchar();

    /* 释放 RTF 颜色表缓存（正常退出路径） */
    if (g_cached_ctbl) { free(g_cached_ctbl); g_cached_ctbl = NULL; g_cached_ctbl_len = 0; }

    /* 恢复系统计时精度 */
    timeEndPeriod(1);

    return 0;
}
