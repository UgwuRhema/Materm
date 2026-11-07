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
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

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

/*append text to the scrollback buffer*/
static void output_append(char *out, size_t outsz, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (n >= outsz - 1)
    {
        memcpy(out, s + n - (outsz - 1), outsz - 1);
        out[outsz - 1] = '\0';
        *len = outsz - 1;
        return;
    }
    if (*len + n > outsz - 1)
    {
        size_t drop = *len + n - (outsz - 1);
        memmove(out, out + drop, *len - drop);
        *len -= drop;
    }
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
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

/*constants*/
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define WINDOW_DELAY 3000
#define FONT_PATH "/usr/share/fonts/jetbrains-mono/JetBrainsMonoNL-Medium.ttf"
#define FONT_SIZE 14
#define INPUT_MAX 1024
#define OUTPUT_MAX 65536

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
    static char output[OUTPUT_MAX] = "";
    size_t output_len = 0;
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
                        output_append(output, sizeof output, &output_len, ps1);
                        output_append(output, sizeof output, &output_len, cmd);
                        output_append(output, sizeof output, &output_len, "\n");
                        static char result[OUTPUT_MAX];
                        run_command(cmd, result, sizeof result);
                        output_append(output, sizeof output, &output_len, result);
                        if (result[0] && result[strlen(result) - 1] != '\n')
                            output_append(output, sizeof output, &output_len, "\n");
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
        /*draw the command output, wrapped and anchored to the bottom*/
        if (output_len > 0)
        {
            SDL_Surface *text = TTF_RenderUTF8_Blended_Wrapped(font, output, white, win_w - 2 * margin);
            if (text)
            {
                SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, text);
                if (texture)
                {
                    int avail = win_h - line_skip - 2 * margin;
                    if (avail < 0)
                        avail = 0;
                    SDL_Rect src = {0, 0, text->w, text->h};
                    SDL_Rect dst = {margin, margin, text->w, text->h};
                    if (text->h > avail)
                    {
                        src.y = text->h - avail;
                        src.h = avail;
                        dst.h = avail;
                    }
                    SDL_RenderCopy(renderer, texture, &src, &dst);
                    SDL_DestroyTexture(texture);
                }
                SDL_FreeSurface(text);
            }
        }
        /*current input line pinned to the bottom*/
        int input_y = win_h - line_skip - margin;
        if (input_y < margin)
            input_y = margin;
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
