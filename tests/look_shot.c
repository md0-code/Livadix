/* Headless check of the window's looks: shows a BMP through the presenter in
 * a hidden window and saves the picture as the window shows it.
 *
 *   mc_look_shot IN.bmp OUT.bmp LOOK PHOSPHOR SCALE [FILTER INTEGER WIDTH HEIGHT]
 *
 * LOOK and PHOSPHOR are the MC_VIDEO_LOOK_* and MC_VIDEO_PHOSPHOR_* numbers,
 * FILTER an mc_filter (default nearest), INTEGER 1 for whole multiples
 * (default fit) and WIDTH x HEIGHT the window's size (default the picture
 * times SCALE).
 * Exit status 0: saved; 2: the look needs OpenGL 3.3, which is unavailable;
 * 1: any other failure. */
#include "../src/platform/video.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 6 && argc != 10) {
        fputs("usage: mc_look_shot IN.bmp OUT.bmp LOOK PHOSPHOR SCALE [FILTER INTEGER WIDTH HEIGHT]\n", stderr);
        return 1;
    }
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Surface *loaded = SDL_LoadBMP(argv[1]);
    SDL_Surface *frame = loaded ? SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
    if (!frame) { fprintf(stderr, "%s: %s\n", argv[1], SDL_GetError()); SDL_Quit(); return 1; }
    mc_settings settings = {0};
    settings.window_scale = atoi(argv[5]);
    settings.aspect = MC_ASPECT_SQUARE;
    if (argc == 10) {
        settings.filter = atoi(argv[6]);
        settings.integer_scale = atoi(argv[7]) ? 1 << 16 : 0;
    }
    char error[256] = "";
    mc_video *video = mc_video_open("look", &settings, frame->w, frame->h, frame->w, frame->h, true, error, sizeof error);
    if (!video) { fprintf(stderr, "%s\n", error); SDL_Quit(); return 1; }
    if (argc == 10) SDL_SetWindowSize(mc_video_window(video), atoi(argv[8]), atoi(argv[9]));
    int status = 0;
    if (!mc_video_set_look(video, atoi(argv[3]), atoi(argv[4]))) status = 2;
    else {
        char saved[1024];
        mc_video_request_shot(video, argv[2], false);
        mc_video_present(video, (const uint32_t *)frame->pixels, frame->w, frame->h);
        if (!mc_video_take_shot_result(video, saved, sizeof saved) || !saved[0]) status = 1;
    }
    mc_video_close(video);
    SDL_FreeSurface(frame);
    SDL_FreeSurface(loaded);
    SDL_Quit();
    return status;
}
