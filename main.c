#include <SDL2/SDL_error.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_video.h>
#include <stdio.h>
#include <SDL2/SDL.h>

/*constants*/
#define WINDOW_WIDTH 400
#define WINDOW_HEIGHT 600
#define WINDOW_DELAY 3000

int main(int argc, char *argv[])
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
                              0);
    if (!window)
    {
        fprintf(stderr, "SDL Failed to initialize: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Delay(WINDOW_DELAY);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
