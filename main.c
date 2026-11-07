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

/*constants*/
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600
#define WINDOW_DELAY 3000
#define FONT_PATH "/usr/share/fonts/jetbrains-mono/JetBrainsMonoNL-Medium.ttf"
#define FONT_SIZE 14

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
    #define INPUT_MAX 1024
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
            if (e.type == SDL_TEXTINPUT)
            {
                size_t add = strlen(e.text.text);
                if (len + add < INPUT_MAX)
                {
                    memcpy(buffer + len, e.text.text, add + 1); /*copies the '\0' too*/
                    len += add;
                }
            }
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);/*surely this makes it black*/
        /*build the line to draw: prompt followed by what we typed*/
        char line[INPUT_MAX + 256];
        snprintf(line, sizeof(line), "%s%s", ps1, buffer);
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface *text = TTF_RenderText_Blended(font, line, white);
        if (text)
        {
            SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, text);
            if (texture)
            {
                SDL_Rect dst = {8, 8, text->w, text->h};
                SDL_RenderCopy(renderer, texture, NULL, &dst);
                SDL_DestroyTexture(texture);
            }
            SDL_FreeSurface(text);
        }
        /*blinking block cursor right after the text*/
        int line_w = 0, line_h = 0, cell_w = 0, cell_h = 0;
        if (TTF_SizeText(font, line, &line_w, &line_h) == 0 &&
            TTF_SizeText(font, "M", &cell_w, &cell_h) == 0 &&
            (SDL_GetTicks() / 500) % 2 == 0)
        {
            SDL_Rect cursor = {8 + line_w, 8, cell_w, TTF_FontHeight(font)};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer, &cursor);
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
