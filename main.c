/*A Mini Terminal using SDL2*/
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_error.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_render.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_video.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/*constants*/
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define WINDOW_DELAY 3000
#define FONT_PATH "/usr/share/fonts/jetbrains-mono/JetBrainsMonoNL-Medium.ttf" /*can change this*/
#define FONT_SIZE 14
#define INPUT_MAX 1024
#define OUTPUT_MAX 65536

/*append a string to a bounded buffer*/
static void ps1_append(char *out, size_t outsz, size_t *len, const char *s)
{
    if (*len >= outsz - 1)
        return;
    size_t n = strlen(s);
    if (*len + n > outsz - 1)
        n = outsz - 1 - *len;
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
}

/*skip an ANSI CSI sequence starting at raw[i] (the byte after ESC)*/
static size_t ps1_skip_csi(const char *raw, size_t i)
{
    if (raw[i] != '[')
        return i;
    i++;
    while (raw[i] && !(raw[i] >= '@' && raw[i] <= '~'))
        i++;
    if (raw[i])
        i++;
    return i;
}

/*turn a raw bash PS1 (escapes + ANSI codes) into printable text*/
static void expand_ps1(const char *raw, char *out, size_t outsz)
{
    size_t o = 0;
    char tmp[PATH_MAX];
    out[0] = '\0';
    for (size_t i = 0; raw[i]; i++)
    {
        if (raw[i] == '\033') /*actual ESC*/
        {
            i = ps1_skip_csi(raw, i + 1) - 1;
            continue;
        }
        if (raw[i] != '\\')
        {
            char s[2] = {raw[i], '\0'};
            ps1_append(out, outsz, &o, s);
            continue;
        }
        char c = raw[++i];
        if (!c)
            break;
        switch (c)
        {
        case 'u': {
            const char *u = getenv("USER");
            ps1_append(out, outsz, &o, u ? u : "user");
            break;
        }
        case 'h':
        case 'H':
            if (gethostname(tmp, sizeof tmp) != 0)
                tmp[0] = '\0';
            if (c == 'h')
            {
                char *dot = strchr(tmp, '.');
                if (dot)
                    *dot = '\0';
            }
            ps1_append(out, outsz, &o, tmp);
            break;
        case 'w':
        case 'W': {
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof cwd))
                cwd[0] = '\0';
            const char *home = getenv("HOME");
            if (c == 'W')
            {
                const char *base = strrchr(cwd, '/');
                ps1_append(out, outsz, &o, base ? base + 1 : cwd);
            }
            else if (home && strncmp(cwd, home, strlen(home)) == 0 &&
                     (cwd[strlen(home)] == '/' || cwd[strlen(home)] == '\0'))
            {
                char disp[PATH_MAX + 2];
                snprintf(disp, sizeof disp, "~%s", cwd + strlen(home));
                ps1_append(out, outsz, &o, disp);
            }
            else
                ps1_append(out, outsz, &o, cwd);
            break;
        }
        case '$':
            ps1_append(out, outsz, &o, geteuid() == 0 ? "#" : "$");
            break;
        case 'n':
            ps1_append(out, outsz, &o, "\n");
            break;
        case 't':
        case 'T': {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char b[16];
            strftime(b, sizeof b, c == 't' ? "%H:%M:%S" : "%I:%M:%S", tm);
            ps1_append(out, outsz, &o, b);
            break;
        }
        case 'd': {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char b[64];
            strftime(b, sizeof b, "%a %b %d", tm);
            ps1_append(out, outsz, &o, b);
            break;
        }
        case 's':
            ps1_append(out, outsz, &o, "bash");
            break;
        case '\\':
            ps1_append(out, outsz, &o, "\\");
            break;
        case 'e':
            i = ps1_skip_csi(raw, i + 1) - 1;
            break;
        case '[':
        case ']':
            break;
        default:
            if (c >= '0' && c <= '7')
            {
                int val = c - '0';
                for (int k = 0; k < 2 && raw[i + 1] >= '0' && raw[i + 1] <= '7'; k++)
                    val = val * 8 + (raw[++i] - '0');
                if (val == 27)
                    i = ps1_skip_csi(raw, i + 1) - 1;
                else
                {
                    char s[2] = {(char)val, '\0'};
                    ps1_append(out, outsz, &o, s);
                }
            }
            else
            {
                char s[2] = {c, '\0'};
                ps1_append(out, outsz, &o, s);
            }
        }
    }
}

/*move to the start of the previous UTF-8 character*/
static size_t utf8_prev(const char *s, size_t pos)
{
    if (pos == 0)
        return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

/*move past the UTF-8 character starting at pos*/
static size_t utf8_next(const char *s, size_t len, size_t pos)
{
    if (pos >= len)
        return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

#define ANSI_DEFAULT_FG 0xFFFFFFu
#define ANSI_DEFAULT_BG 0x000000u

/*the usual 16 ANSI colors, picked to stay readable on black*/
static uint32_t ansi_palette_color(int index)
{
    static const uint32_t colors[16] = {
        0x555555, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4,
        0x7F848E, 0xFF7B86, 0xB5E890, 0xFFD68A, 0x7CC4FF, 0xE29BF0, 0x7FE3F0, 0xFFFFFF};
    if (index < 0 || index > 15)
        return ANSI_DEFAULT_FG;
    return colors[index];
}

static uint32_t ansi_256_color(int index)
{
    if (index < 0)
        index = 0;
    if (index < 16)
        return ansi_palette_color(index);
    if (index < 232)
    {
        static const int levels[6] = {0, 95, 135, 175, 215, 255};
        index -= 16;
        int r = levels[index / 36];
        int g = levels[(index % 36) / 6];
        int b = levels[index % 6];
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    int v = 8 + (index - 232) * 10;
    if (v > 255)
        v = 255;
    return ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
}

static uint32_t ansi_rgb_color(int r, int g, int b)
{
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static SDL_Color output_color(uint32_t rgb)
{
    SDL_Color c = {(Uint8)((rgb >> 16) & 0xFF), (Uint8)((rgb >> 8) & 0xFF), (Uint8)(rgb & 0xFF), 255};
    return c;
}

/*a text screen with a write cursor, so control sequences can move around*/
typedef struct
{
    char data[OUTPUT_MAX];
    uint32_t fg[OUTPUT_MAX];
    uint32_t bg[OUTPUT_MAX];
    size_t len;
    size_t pos;
    size_t saved_pos;
    uint32_t fg_color;
    uint32_t bg_color;
} OutputBuffer;

/*throw away the oldest text when the buffer is full*/
static void output_drop_front(OutputBuffer *out, size_t drop)
{
    if (drop > out->len)
        drop = out->len;
    size_t left = out->len - drop;
    memmove(out->data, out->data + drop, left + 1);
    memmove(out->fg, out->fg + drop, left * sizeof(uint32_t));
    memmove(out->bg, out->bg + drop, left * sizeof(uint32_t));
    out->len = left;
    out->pos = out->pos > drop ? out->pos - drop : 0;
    out->saved_pos = out->saved_pos > drop ? out->saved_pos - drop : 0;
}

static void output_clear(OutputBuffer *out)
{
    out->len = 0;
    out->pos = 0;
    out->saved_pos = 0;
    out->data[0] = '\0';
}

/*append text at the end of the buffer (used for the echoed prompt and commands)*/
static void output_append(OutputBuffer *out, const char *s)
{
    size_t n = strlen(s);
    out->pos = out->len;
    if (n >= OUTPUT_MAX - 1)
    {
        memcpy(out->data, s + n - (OUTPUT_MAX - 1), OUTPUT_MAX - 1);
        for (size_t i = 0; i < OUTPUT_MAX - 1; i++)
        {
            out->fg[i] = ANSI_DEFAULT_FG;
            out->bg[i] = ANSI_DEFAULT_BG;
        }
        out->len = OUTPUT_MAX - 1;
        out->pos = out->len;
        out->data[out->len] = '\0';
        return;
    }
    if (out->len + n > OUTPUT_MAX - 1)
        output_drop_front(out, out->len + n - (OUTPUT_MAX - 1));
    memcpy(out->data + out->len, s, n);
    for (size_t i = 0; i < n; i++)
    {
        out->fg[out->len + i] = ANSI_DEFAULT_FG;
        out->bg[out->len + i] = ANSI_DEFAULT_BG;
    }
    out->len += n;
    out->pos = out->len;
    out->data[out->len] = '\0';
}

/*write one byte at the cursor, inserting before a line break*/
static void output_putc(OutputBuffer *out, char c)
{
    if (out->pos > out->len)
        out->pos = out->len;
    if (out->pos < out->len && out->data[out->pos] == '\n')
    {
        if (out->len >= OUTPUT_MAX - 1)
            output_drop_front(out, out->len - (OUTPUT_MAX - 2));
        memmove(out->data + out->pos + 1, out->data + out->pos, out->len - out->pos + 1);
        memmove(out->fg + out->pos + 1, out->fg + out->pos, (out->len - out->pos) * sizeof(uint32_t));
        memmove(out->bg + out->pos + 1, out->bg + out->pos, (out->len - out->pos) * sizeof(uint32_t));
        out->data[out->pos] = c;
        out->fg[out->pos] = out->fg_color;
        out->bg[out->pos] = out->bg_color;
        out->len++;
        out->pos++;
        out->data[out->len] = '\0';
        return;
    }
    if (out->pos >= OUTPUT_MAX - 1)
        output_drop_front(out, out->pos - (OUTPUT_MAX - 2));
    out->data[out->pos] = c;
    out->fg[out->pos] = out->fg_color;
    out->bg[out->pos] = out->bg_color;
    out->pos++;
    if (out->pos > out->len)
        out->len = out->pos;
    out->data[out->len] = '\0';
}

static size_t output_line_start(const OutputBuffer *out, size_t pos)
{
    if (pos > out->len)
        pos = out->len;
    while (pos > 0 && out->data[pos - 1] != '\n')
        pos--;
    return pos;
}

static size_t output_line_end(const OutputBuffer *out, size_t pos)
{
    if (pos > out->len)
        pos = out->len;
    while (pos < out->len && out->data[pos] != '\n')
        pos++;
    return pos;
}

static void output_newline(OutputBuffer *out)
{
    size_t end = output_line_end(out, out->pos);
    if (end < out->len)
        out->pos = end + 1; /*there is already a line break to move past*/
    else
        output_putc(out, '\n');
}

static void output_backspace(OutputBuffer *out)
{
    size_t start = output_line_start(out, out->pos);
    if (out->pos > start)
        out->pos = utf8_prev(out->data, out->pos);
}

static void output_tab(OutputBuffer *out)
{
    size_t start = output_line_start(out, out->pos);
    size_t spaces = 8 - ((out->pos - start) % 8);
    while (spaces-- > 0)
        output_putc(out, ' ');
}

static void output_erase_line(OutputBuffer *out, int mode)
{
    size_t start = output_line_start(out, out->pos);
    size_t end = output_line_end(out, out->pos);
    size_t from = start;
    size_t to = end;
    if (mode == 0)
        from = out->pos;
    else if (mode == 1)
        to = out->pos;
    else if (mode != 2)
        return;
    for (size_t i = from; i < to; i++)
    {
        out->data[i] = ' ';
        out->fg[i] = ANSI_DEFAULT_FG;
        out->bg[i] = out->bg_color;
    }
}

static void output_erase_display(OutputBuffer *out, int mode)
{
    if (mode == 0)
    {
        if (out->pos < out->len)
        {
            out->len = out->pos;
            out->data[out->len] = '\0';
        }
    }
    else if (mode == 1)
    {
        for (size_t i = 0; i < out->pos && i < out->len; i++)
        {
            if (out->data[i] != '\n')
            {
                out->data[i] = ' ';
                out->fg[i] = ANSI_DEFAULT_FG;
                out->bg[i] = out->bg_color;
            }
        }
    }
    else if (mode == 2 || mode == 3)
    {
        output_clear(out);
    }
}

static void output_move_to(OutputBuffer *out, int row, int col)
{
    if (row < 1)
        row = 1;
    if (col < 1)
        col = 1;
    size_t pos = 0;
    for (int r = 1; r < row; r++)
    {
        size_t end = output_line_end(out, pos);
        if (end >= out->len)
        {
            pos = out->len;
            break;
        }
        pos = end + 1;
    }
    size_t end = output_line_end(out, pos);
    size_t target = (size_t)(col - 1);
    if (target > end - pos)
        target = end - pos;
    out->pos = pos + target;
}

static void output_move_vertical(OutputBuffer *out, int lines)
{
    size_t start = output_line_start(out, out->pos);
    size_t col = out->pos - start;
    size_t pos = start;
    while (lines < 0)
    {
        if (pos == 0)
            break;
        pos = output_line_start(out, pos - 1);
        lines++;
    }
    while (lines > 0)
    {
        size_t end = output_line_end(out, pos);
        if (end >= out->len)
            break;
        pos = end + 1;
        lines--;
    }
    size_t end = output_line_end(out, pos);
    if (col > end - pos)
        col = end - pos;
    out->pos = pos + col;
}

static void output_move_column(OutputBuffer *out, int columns)
{
    size_t start = output_line_start(out, out->pos);
    size_t end = output_line_end(out, out->pos);
    if (columns < 0)
    {
        size_t n = (size_t)(-columns);
        out->pos = n > out->pos - start ? start : out->pos - n;
    }
    else
    {
        size_t n = (size_t)columns;
        out->pos = n > end - out->pos ? end : out->pos + n;
    }
}

static void output_set_column(OutputBuffer *out, int col)
{
    size_t start = output_line_start(out, out->pos);
    size_t end = output_line_end(out, out->pos);
    if (col < 1)
        col = 1;
    size_t target = (size_t)(col - 1);
    if (target > end - start)
        target = end - start;
    out->pos = start + target;
}

/*read the n-th numeric argument of a CSI sequence*/
static int ansi_param(const char *params, size_t n, int index, int fallback)
{
    int current = 0;
    int value = 0;
    bool seen = false;
    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)params[i];
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + (c - '0');
            seen = true;
        }
        else if (c == ';')
        {
            if (current == index)
                return seen ? value : fallback;
            current++;
            value = 0;
            seen = false;
        }
    }
    if (current == index)
        return seen ? value : fallback;
    return fallback;
}

/*split an SGR parameter list into numbers*/
static size_t ansi_collect(const char *params, size_t n, int *values, size_t max)
{
    size_t count = 0;
    int value = 0;
    bool seen = false;
    for (size_t i = 0; i <= n; i++)
    {
        char c = i < n ? params[i] : ';';
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + (c - '0');
            seen = true;
        }
        else if (c == ';')
        {
            if (count < max)
                values[count++] = seen ? value : 0;
            value = 0;
            seen = false;
        }
    }
    return count;
}

/*apply colors from a Select Graphic Rendition sequence*/
static void ansi_sgr(OutputBuffer *out, const char *params, size_t n)
{
    int values[32];
    size_t count = ansi_collect(params, n, values, 32);
    if (count == 0)
    {
        out->fg_color = ANSI_DEFAULT_FG;
        out->bg_color = ANSI_DEFAULT_BG;
        return;
    }
    for (size_t i = 0; i < count; i++)
    {
        int v = values[i];
        if (v == 0)
        {
            out->fg_color = ANSI_DEFAULT_FG;
            out->bg_color = ANSI_DEFAULT_BG;
        }
        else if (v >= 30 && v <= 37)
            out->fg_color = ansi_palette_color(v - 30);
        else if (v == 39)
            out->fg_color = ANSI_DEFAULT_FG;
        else if (v >= 40 && v <= 47)
            out->bg_color = ansi_palette_color(v - 40);
        else if (v == 49)
            out->bg_color = ANSI_DEFAULT_BG;
        else if (v >= 90 && v <= 97)
            out->fg_color = ansi_palette_color(v - 90 + 8);
        else if (v >= 100 && v <= 107)
            out->bg_color = ansi_palette_color(v - 100 + 8);
        else if ((v == 38 || v == 48) && i + 1 < count)
        {
            uint32_t color = ANSI_DEFAULT_FG;
            bool have_color = false;
            if (values[i + 1] == 5 && i + 2 < count)
            {
                color = ansi_256_color(values[i + 2]);
                i += 2;
                have_color = true;
            }
            else if (values[i + 1] == 2 && i + 4 < count)
            {
                color = ansi_rgb_color(values[i + 2], values[i + 3], values[i + 4]);
                i += 4;
                have_color = true;
            }
            if (have_color)
            {
                if (v == 38)
                    out->fg_color = color;
                else
                    out->bg_color = color;
            }
        }
    }
}

static void ansi_csi(OutputBuffer *out, const char *params, size_t n, char final)
{
    int one = ansi_param(params, n, 0, 0);
    int two = ansi_param(params, n, 1, 1);
    switch (final)
    {
    case 'A':
        output_move_vertical(out, -(one ? one : 1));
        break;
    case 'B':
        output_move_vertical(out, one ? one : 1);
        break;
    case 'C':
        output_move_column(out, one ? one : 1);
        break;
    case 'D':
        output_move_column(out, -(one ? one : 1));
        break;
    case 'E':
        output_move_vertical(out, one ? one : 1);
        output_set_column(out, 1);
        break;
    case 'F':
        output_move_vertical(out, -(one ? one : 1));
        output_set_column(out, 1);
        break;
    case 'G':
        output_set_column(out, one ? one : 1);
        break;
    case 'H':
    case 'f':
        output_move_to(out, one ? one : 1, two ? two : 1);
        break;
    case 'd': {
        int col = (int)(out->pos - output_line_start(out, out->pos)) + 1;
        output_move_to(out, one ? one : 1, col);
        break;
    }
    case 'J':
        output_erase_display(out, one);
        break;
    case 'K':
        output_erase_line(out, one);
        break;
    case 's':
        out->saved_pos = out->pos;
        break;
    case 'u':
        out->pos = out->saved_pos;
        break;
    case 'm':
        ansi_sgr(out, params, n);
        break;
    default:
        /*modes (h/l) and everything else are ignored for now*/
        break;
    }
}

/*feed raw command output through the ANSI parser*/
static void output_feed(OutputBuffer *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n;)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b)
        {
            i++;
            if (i >= n)
                break;
            if (s[i] == '[')
            {
                i++;
                size_t start = i;
                while (i < n && (unsigned char)s[i] >= 0x20 && (unsigned char)s[i] <= 0x3f)
                    i++;
                if (i < n)
                {
                    char final = s[i];
                    if ((unsigned char)final >= 0x40 && (unsigned char)final <= 0x7e)
                        ansi_csi(out, s + start, i - start, final);
                    i++;
                }
                continue;
            }
            if (s[i] == ']')
            {
                /*OSC: skip until BEL or ST*/
                i++;
                while (i < n)
                {
                    if (s[i] == '\a')
                    {
                        i++;
                        break;
                    }
                    if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\')
                    {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
            if (s[i] == '7')
            {
                out->saved_pos = out->pos;
                i++;
                continue;
            }
            if (s[i] == '8')
            {
                out->pos = out->saved_pos;
                i++;
                continue;
            }
            if (s[i] == 'c')
            {
                output_clear(out);
                i++;
                continue;
            }
            /*two byte escape (with optional intermediates): just skip it*/
            while (i < n && (unsigned char)s[i] >= 0x20 && (unsigned char)s[i] <= 0x2f)
                i++;
            if (i < n)
                i++;
            continue;
        }
        switch (c)
        {
        case '\n':
            output_newline(out);
            break;
        case '\r':
            out->pos = output_line_start(out, out->pos);
            break;
        case '\b':
            output_backspace(out);
            break;
        case '\t':
            output_tab(out);
            break;
        case '\a':
            break;
        default:
            if (c >= 0x20 && c != 0x7f)
                output_putc(out, (char)c);
            break;
        }
        i++;
    }
}

/*count how many wrapped rows the output needs*/
static int output_row_count(const OutputBuffer *out, int columns)
{
    int rows = 0;
    for (size_t start = 0; start < out->len;)
    {
        size_t end = output_line_end(out, start);
        int cells = 0;
        for (size_t p = start; p < end; p = utf8_next(out->data, end, p))
            cells++;
        rows += cells == 0 ? 1 : (cells + columns - 1) / columns;
        if (end >= out->len)
            break;
        start = end + 1;
    }
    return rows;
}

/*draw colored, wrapped output and return the y position after it*/
static int output_render(SDL_Renderer *renderer, TTF_Font *font, OutputBuffer *out, int win_w, int win_h,
                         int margin, int line_skip)
{
    int cell_w = 0, cell_h = 0;
    if (TTF_SizeText(font, "M", &cell_w, &cell_h) != 0 || cell_w <= 0)
        cell_w = 8;
    int columns = (win_w - 2 * margin) / cell_w;
    if (columns < 1)
        columns = 1;
    int max_rows = (win_h - line_skip - 2 * margin) / line_skip;
    if (max_rows < 0)
        max_rows = 0;
    int total_rows = output_row_count(out, columns);
    int first_row = total_rows > max_rows ? total_rows - max_rows : 0;
    int row = 0;
    int drawn = 0;
    for (size_t start = 0; start < out->len;)
    {
        size_t end = output_line_end(out, start);
        size_t p = start;
        do
        {
            size_t row_start = p;
            int cells = 0;
            while (p < end && cells < columns)
            {
                p = utf8_next(out->data, end, p);
                cells++;
            }
            if (row >= first_row && drawn < max_rows)
            {
                int y = margin + drawn * line_skip;
                int col = 0;
                size_t q = row_start;
                while (q < p)
                {
                    uint32_t fg = out->fg[q];
                    uint32_t bg = out->bg[q];
                    size_t run_end = utf8_next(out->data, p, q);
                    while (run_end < p && out->fg[run_end] == fg && out->bg[run_end] == bg)
                        run_end = utf8_next(out->data, p, run_end);
                    int run_cells = 0;
                    for (size_t t = q; t < run_end; t = utf8_next(out->data, run_end, t))
                        run_cells++;
                    if (bg != ANSI_DEFAULT_BG)
                    {
                        SDL_Color back = output_color(bg);
                        SDL_Rect rect = {margin + col * cell_w, y, run_cells * cell_w, line_skip};
                        SDL_SetRenderDrawColor(renderer, back.r, back.g, back.b, 255);
                        SDL_RenderFillRect(renderer, &rect);
                    }
                    char saved = out->data[run_end];
                    out->data[run_end] = '\0';
                    SDL_Color color = output_color(fg);
                    SDL_Surface *text = TTF_RenderUTF8_Blended(font, out->data + q, color);
                    out->data[run_end] = saved;
                    if (text)
                    {
                        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, text);
                        if (texture)
                        {
                            SDL_Rect dst = {margin + col * cell_w, y, text->w, text->h};
                            SDL_RenderCopy(renderer, texture, NULL, &dst);
                            SDL_DestroyTexture(texture);
                        }
                        SDL_FreeSurface(text);
                    }
                    col += run_cells;
                    q = run_end;
                }
                drawn++;
            }
            row++;
        } while (p < end);
        if (end >= out->len)
            break;
        start = end + 1;
    }
    return margin + drawn * line_skip;
}

/*run a command with bash and capture stdout+stderr*/
static int run_command(const char *cmd, char *out, size_t outsz)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0)
    {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/bash", "bash", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t total = 0;
    char tmp[4096];
    ssize_t n;
    while ((n = read(fds[0], tmp, sizeof tmp)) > 0)
    {
        size_t copy = (size_t)n;
        if (total + copy > outsz - 1)
            copy = outsz - 1 - total;
        if (copy > 0)
        {
            memcpy(out + total, tmp, copy);
            total += copy;
        }
    }
    out[total] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

int main([[maybe_unused]]int argc, [[maybe_unused]]char *argv[])
{
    SDL_Window *window = NULL;
    /*initialize window video subsystem*/
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "SDL Failed to initialize: %s\n", SDL_GetError());
        return 1;
    }
    /*create a centered window*/
    window = SDL_CreateWindow("Materm", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH,
                              WINDOW_HEIGHT,
                              SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        fprintf(stderr, "SDL Failed to initialize: %s\n", SDL_GetError());
        SDL_Quit();
		return 1;
    }
    /*let's create the renderer*/
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
    {
        fprintf(stderr, "SDL Failed to initialize: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
    }
    /*initialize font rendering*/
    if (TTF_Init() != 0)
    {
        fprintf(stderr, "TTF Failed to initialize: %s\n", TTF_GetError());
        return 1;
    }
    TTF_Font *font = TTF_OpenFont(FONT_PATH, FONT_SIZE);
    if (!font)
    {
        fprintf(stderr, "Failed to open font '%s': %s\n", FONT_PATH, TTF_GetError());
        return 1;
    }
    SDL_StartTextInput(); /*start tetx input*/
    char buffer[INPUT_MAX] = "";
    size_t len = 0;
    size_t cursor = 0; /*byte offset into buffer*/
    static OutputBuffer output = {.fg_color = ANSI_DEFAULT_FG, .bg_color = ANSI_DEFAULT_BG};
    /*first of all let me get my PS1*/
    const char *raw_ps1 = getenv("PS1");
    if (!raw_ps1)
        raw_ps1 = "$ "; /*PS1 is a shell variable, so it usually isn't exported*/
    char ps1[512];
    expand_ps1(raw_ps1, ps1, sizeof ps1);
    /*lets's make the window stay open till we press quit*/
    bool running = true;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e) > 0)
        {
            if (e.type == SDL_QUIT)
                running = false;
            else if (e.type == SDL_TEXTINPUT)
            {
                size_t add = strlen(e.text.text);
                if (len + add < INPUT_MAX)
                {
                    /*insert at the cursor, not always at the end*/
                    memmove(buffer + cursor + add, buffer + cursor, len - cursor + 1);
                    memcpy(buffer + cursor, e.text.text, add);
                    len += add;
                    cursor += add;
                }
            }
            else if (e.type == SDL_KEYDOWN)
            {
                switch (e.key.keysym.sym)
                {
                case SDLK_BACKSPACE:
                    if (cursor > 0)
                    {
                        size_t start = utf8_prev(buffer, cursor);
                        size_t n = cursor - start;
                        memmove(buffer + start, buffer + cursor, len - cursor + 1);
                        len -= n;
                        cursor = start;
                    }
                    break;
                case SDLK_DELETE:
                    if (cursor < len)
                    {
                        size_t end = utf8_next(buffer, len, cursor);
                        size_t n = end - cursor;
                        memmove(buffer + cursor, buffer + end, len - end + 1);
                        len -= n;
                    }
                    break;
                case SDLK_LEFT:
                    cursor = utf8_prev(buffer, cursor);
                    break;
                case SDLK_RIGHT:
                    cursor = utf8_next(buffer, len, cursor);
                    break;
                case SDLK_HOME:
                    cursor = 0;
                    break;
                case SDLK_END:
                    cursor = len;
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    /*execute the line with bash and show its output*/
                    if (len > 0)
                    {
                        char cmd[INPUT_MAX];
                        memcpy(cmd, buffer, len);
                        cmd[len] = '\0';
                        output_append(&output, ps1);
                        output_append(&output, cmd);
                        output_append(&output, "\n");
                        static char result[OUTPUT_MAX];
                        run_command(cmd, result, sizeof result);
                        output.fg_color = ANSI_DEFAULT_FG;
                        output.bg_color = ANSI_DEFAULT_BG;
                        output_feed(&output, result, strlen(result));
                        output.fg_color = ANSI_DEFAULT_FG;
                        output.bg_color = ANSI_DEFAULT_BG;
                        if (output.len > 0 && output.data[output.len - 1] != '\n')
                            output_append(&output, "\n");
                    }
                    buffer[0] = '\0';
                    len = 0;
                    cursor = 0;
                    break;
                }
            }
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);/*surely this makes it black*/
        SDL_Color white = {255, 255, 255, 255};
        int line_skip = TTF_FontLineSkip(font);
        int margin = 8;
        int win_w = 0, win_h = 0;
        SDL_GetWindowSize(window, &win_w, &win_h);
        /*draw the colored command output from the top, keeping the newest lines visible*/
        int input_y = output_render(renderer, font, &output, win_w, win_h, margin, line_skip);
        /*current input line flows right after the output*/
        char line[INPUT_MAX + 512];
        snprintf(line, sizeof(line), "%s%s", ps1, buffer);
        SDL_Surface *text = TTF_RenderUTF8_Blended(font, line, white);
        if (text)
        {
            SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, text);
            if (texture)
            {
                SDL_Rect dst = {margin, input_y, text->w, text->h};
                SDL_RenderCopy(renderer, texture, NULL, &dst);
                SDL_DestroyTexture(texture);
            }
            SDL_FreeSurface(text);
        }
        /*blinking block cursor at the editing position*/
        char before[INPUT_MAX + 512];
        snprintf(before, sizeof before, "%s%.*s", ps1, (int)cursor, buffer);
        int before_w = 0, before_h = 0, cell_w = 0, cell_h = 0;
        if (TTF_SizeText(font, before, &before_w, &before_h) == 0 &&
            TTF_SizeText(font, "M", &cell_w, &cell_h) == 0 &&
            (SDL_GetTicks() / 500) % 2 == 0)
        {
            SDL_Rect cursor_rect = {margin + before_w, input_y, cell_w, TTF_FontHeight(font)};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer, &cursor_rect);
        }
        SDL_RenderPresent(renderer);
    }
    SDL_StopTextInput();
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
