/*A Mini Terminal using SDL2*/
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_error.h>
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_render.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_video.h>
#include <stdio.h>
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <string.h>

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
    #define MAX_INPUT 1024
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
    char buffer[MAX_INPUT] = "";
    size_t len = 0;
    /*first of all let me get my PS1*/
    const char *ps1 = getenv("PS1");
    if (!ps1)
        ps1 = "$ "; /*PS1 is a shell variable, so it usually isn't exported*/
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
                if (len + add < MAX_INPUT)
                {
                    memcpy(buffer + len, e.text.text, add + 1); /*copies the '\0' too*/
                    len += add;
                }
            }
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);/*surely this makes it black*/
        /*build the line to draw: prompt followed by what we typed*/
        char line[MAX_INPUT + 256];
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
