/* The shell: the ADB key map, the window's shape (a hidden window), the sound path from the
 * machine to an output device (SDL's dummy driver under ctest) and the
 * serial ports' host ends. */
#include "test.h"

#include "../src/app/desktop.h"
#include "../src/platform/audio.h"
#include "../src/platform/serial.h"
#include "../src/platform/video.h"

#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int mc_test_failures;

static void test_keys(void) {
    /* Left Alt is Command by default, Right Alt Option, Control Control;
     * the Windows keys open the configuration window and send nothing. */
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_A, MC_COMMAND_LEFT_ALT), 0x00);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_RETURN, MC_COMMAND_LEFT_ALT), 0x24);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_ESCAPE, MC_COMMAND_LEFT_ALT), 0x35);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_LGUI, MC_COMMAND_LEFT_ALT), -1);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_RGUI, MC_COMMAND_LEFT_ALT), -1);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_LALT, MC_COMMAND_LEFT_ALT), 0x37);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_RALT, MC_COMMAND_LEFT_ALT), 0x7c);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_LCTRL, MC_COMMAND_LEFT_ALT), 0x36);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_RCTRL, MC_COMMAND_LEFT_ALT), 0x7d);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_LCTRL, MC_COMMAND_LEFT_CONTROL), 0x37);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_LALT, MC_COMMAND_LEFT_CONTROL), 0x3a);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_LALT, MC_COMMAND_RIGHT_ALT), 0x3a);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_RALT, MC_COMMAND_RIGHT_ALT), 0x37);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_RCTRL, MC_COMMAND_RIGHT_CONTROL), 0x37);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_RSHIFT, MC_COMMAND_LEFT_ALT), 0x7b);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_KP_ENTER, MC_COMMAND_LEFT_ALT), 0x4c);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_F1, MC_COMMAND_LEFT_ALT), 0x7a);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_NONUSBACKSLASH, MC_COMMAND_LEFT_ALT), 0x0a);
    CHECK_EQ(mc_desktop_adb_key(SDL_SCANCODE_APPLICATION, MC_COMMAND_LEFT_ALT), -1);
    /* Each ADB code comes from one host key. */
    static const uint32_t choices[] = {MC_COMMAND_LEFT_ALT, MC_COMMAND_RIGHT_ALT, MC_COMMAND_LEFT_CONTROL, MC_COMMAND_RIGHT_CONTROL};
    for (size_t k = 0; k < sizeof choices / sizeof *choices; ++k) {
        int seen[128] = {0};
        for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) {
            int code = mc_desktop_adb_key(sc, choices[k]);
            CHECK(code >= -1 && code < 128);
            if (code >= 0) seen[code]++;
        }
        for (int code = 0; code < 128; ++code) CHECK(seen[code] <= 1);
        CHECK(seen[0x37] == 1);
    }
}

static void test_sound(void) {
    mc_config c;
    mc_config_defaults(&c);
    c.model = (uint32_t)mc_model_index(mc_model_find(MC_MODEL_STUB));   /* the stub machine's tone */
    char error[256];
    uint8_t rom[16] = {0};
    mc_machine *m = mc_machine_create(&c, rom, sizeof rom, error, sizeof error);
    CHECK(m != NULL);
    if (!m) return;
    CHECK(!mc_machine_set_audio_source(m, NULL));
    CHECK(!mc_machine_set_audio_source(NULL, NULL));
    SDL_SetMainReady();
    CHECK(SDL_Init(SDL_INIT_AUDIO) == 0);
    mc_host_audio audio;
    CHECK(mc_host_audio_open(&audio));
    SDL_PauseAudioDevice(audio.device, 1);   /* keep what is queued */
    CHECK(mc_machine_key(m, 0x31, 1));
    mc_machine_run(m, mc_machine_rate(m) / 4, 0);
    CHECK(mc_host_audio_pump(&audio, m));
    /* 250 ms at 22,255 Hz arrives as about 250 ms at the device's rate (the
     * resampler keeps back the end of its last block; the shell keeps at
     * most 500 ms queued). */
    uint32_t bytes = mc_host_audio_queued(&audio), expect = (uint32_t)audio.output.freq;
    if (!(bytes > expect * 85 / 100 && bytes <= expect)) printf("queued %u, expected about %u\n", (unsigned)bytes, (unsigned)expect);
    CHECK(bytes > expect * 85 / 100 && bytes <= expect);
    mc_host_audio_close(&audio);
    SDL_Quit();
    mc_machine_destroy(m);
}

static void test_capture(void) {
    SDL_SetMainReady();
    CHECK(SDL_Init(SDL_INIT_AUDIO) == 0);
    mc_host_capture capture = {0};
    mc_audio_source source = mc_host_capture_callbacks(&capture);
    int16_t frame[2] = {123, -456};
    source.sample(source.ctx, 22050, frame);
    CHECK(frame[0] == 0 && frame[1] == 0 && !capture.device);
    source.enable(source.ctx, 22050);
    CHECK(capture.device != 0 && capture.rate == 22050);
    if (capture.device) {
        SDL_Delay(300);
        SDL_PauseAudioDevice(capture.device, 1);
        SDL_LockAudioDevice(capture.device);
        CHECK(capture.count == MC_CAPTURE_FRAMES);
        capture.read = MC_CAPTURE_FRAMES - 1;
        capture.count = 2;
        capture.stereo[capture.read * 2] = 1234;
        capture.stereo[capture.read * 2 + 1] = -5678;
        capture.stereo[0] = -12;
        capture.stereo[1] = 34;
        SDL_UnlockAudioDevice(capture.device);
        source.sample(source.ctx, 22050, frame);
        CHECK(frame[0] == 1234 && frame[1] == -5678);
        source.sample(source.ctx, 22050, frame);
        CHECK(frame[0] == -12 && frame[1] == 34);
        source.sample(source.ctx, 22050, frame);
        CHECK(frame[0] == 0 && frame[1] == 0);
        const SDL_AudioDeviceID device = capture.device;
        source.enable(source.ctx, 22050);
        CHECK(capture.device == device);
    }
    source.enable(source.ctx, 44100);
    CHECK(capture.device != 0 && capture.rate == 44100);
    if (capture.device) {
        source.sample(source.ctx, 44100, frame);
        CHECK(frame[0] == 0 && frame[1] == 0);
    }
    source.enable(source.ctx, 0);
    CHECK(!capture.device && !capture.rate && !capture.initialized);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
    mc_host_capture_close(&capture);
    SDL_Quit();
}

#ifdef _WIN32   /* the pipes are the Windows ones; the Linux ports have no test here yet */
/* ports.b to an output file, ports.a to a pipe Livadix serves, then to one
 * another program serves: bytes out through the transmit callback, bytes
 * in to the port's input buffer, a client's departure and return. */
static void test_serial(void) {
    char pipe[64], file[64], error[256];
    snprintf(pipe, sizeof pipe, "\\\\.\\pipe\\livadix-test-%lu", (unsigned long)GetCurrentProcessId());
    snprintf(file, sizeof file, "mc_shell_serial_%lu.txt", (unsigned long)GetCurrentProcessId());
    remove(file);
    mc_port_config ports[2];
    memset(ports, 0, sizeof ports);
    ports[0].type = MC_PORT_PIPE;
    strcpy(ports[0].path, pipe);
    ports[1].type = MC_PORT_OUTPUT;
    strcpy(ports[1].path, file);
    mc_host_serial s;
    CHECK(mc_host_serial_open(&s, ports, error, sizeof error));
    CHECK(s.port[0].server && !s.port[0].connected);
    mc_serial_host h = mc_host_serial_callbacks(&s);
    h.transmit(h.ctx, 1, 'P');
    h.transmit(h.ctx, 1, 'R');
    mc_host_serial_flush(&s.port[1]);
    /* Nobody on the pipe yet: what the Macintosh sends is dropped. */
    h.transmit(h.ctx, 0, 'x');
    mc_host_serial_flush(&s.port[0]);
    CHECK_EQ(s.port[0].out_count, 0);
    HANDLE client = CreateFileA(pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(client != INVALID_HANDLE_VALUE);
    mc_host_serial_read(&s.port[0]);
    CHECK(s.port[0].connected);
    h.transmit(h.ctx, 0, 'H');
    h.transmit(h.ctx, 0, 'i');
    mc_host_serial_flush(&s.port[0]);
    char got[8] = {0};
    DWORD n = 0;
    CHECK(ReadFile(client, got, 2, &n, NULL) && n == 2 && !memcmp(got, "Hi", 2));
    CHECK(WriteFile(client, "ok", 2, &n, NULL) && n == 2);
    mc_host_serial_read(&s.port[0]);
    CHECK_EQ(s.port[0].in_count, 2);
    CHECK(!memcmp(s.port[0].in, "ok", 2));
    /* The client leaves; the pipe waits for the next. */
    CloseHandle(client);
    mc_host_serial_read(&s.port[0]);
    CHECK(!s.port[0].connected);
    client = CreateFileA(pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(client != INVALID_HANDLE_VALUE);
    mc_host_serial_read(&s.port[0]);
    CHECK(s.port[0].connected);
    CloseHandle(client);
    mc_host_serial_close(&s);
    FILE *f = fopen(file, "rb");
    CHECK(f != NULL);
    if (f) {
        CHECK_EQ(fread(got, 1, sizeof got, f), 2);
        CHECK(!memcmp(got, "PR", 2));
        fclose(f);
    }
    remove(file);
    /* A pipe another program serves: Livadix joins it as a client. */
    HANDLE server = CreateNamedPipeA(pipe, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1,
                                     256, 256, 0, NULL);
    CHECK(server != INVALID_HANDLE_VALUE);
    ConnectNamedPipe(server, NULL);
    ports[1].type = MC_PORT_DEFAULT;
    CHECK(mc_host_serial_open(&s, ports, error, sizeof error));
    CHECK(!s.port[0].server && s.port[0].connected);
    h = mc_host_serial_callbacks(&s);
    h.transmit(h.ctx, 0, 'A');
    mc_host_serial_flush(&s.port[0]);
    CHECK(ReadFile(server, got, 1, &n, NULL) && n == 1 && got[0] == 'A');
    mc_host_serial_close(&s);
    CloseHandle(server);
    /* A COM port that is not there is refused. */
    ports[0].type = MC_PORT_SERIAL;
    strcpy(ports[0].path, "COM199");
    CHECK(!mc_host_serial_open(&s, ports, error, sizeof error));
    CHECK(strstr(error, "COM199") != NULL);
}
#endif

/* The window as livadix.exe opens it, windowed (src/app/desktop.c): the
 * screen's size times the window scale, whole multiples (integer scale),
 * linear filter, the LCD look on a gray LCD. The picture as the window
 * shows it holds the whole screen with square pixels: a white border and
 * white 4x4 blocks in the corners of a black screen land where a whole,
 * equal scale on both axes puts them. */
static int luma(const SDL_Surface *s, int x, int y) {
    const uint32_t p = ((const uint32_t *)((const uint8_t *)s->pixels + (size_t)y * (size_t)s->pitch))[x];
    return (int)(((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF)) / 3;
}
static void test_window_shape(void) {
    static const int sizes[][2] = {{640, 400}, {512, 342}, {640, 480}, {1152, 870}};
    CHECK(SDL_InitSubSystem(SDL_INIT_VIDEO) == 0);
    uint32_t *frame = malloc((size_t)1152 * 870 * sizeof *frame);
    CHECK(frame != NULL);
    if (!frame) return;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        const int w = sizes[k][0], h = sizes[k][1];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const int cx = x < 4 || x >= w - 4, cy = y < 4 || y >= h - 4;
                frame[y * w + x] = x == 0 || y == 0 || x == w - 1 || y == h - 1 || (cx && cy) ? 0xFFFFFFFFu : 0xFF000000u;
            }
        for (int scale = 1; scale <= 2; scale++) {
            mc_settings settings;
            memset(&settings, 0, sizeof settings);
            settings.window_scale = scale;
            settings.aspect = MC_ASPECT_SQUARE;
            settings.filter = MC_FILTER_LINEAR;
            settings.integer_scale = 1 << 16;
            char error[256] = "", path[512], saved[512];
            mc_video *v = mc_video_open("shape", &settings, w, h, w, h, true, error, sizeof error);
            CHECK(v != NULL);
            if (!v) continue;
            const bool lcd = mc_video_set_look(v, MC_VIDEO_LOOK_LCD, MC_VIDEO_PHOSPHOR_GRAY);
            int cw = 0, ch = 0;
            SDL_GetWindowSize(mc_video_window(v), &cw, &ch);
            CHECK(cw == w * scale && ch == h * scale);
            snprintf(path, sizeof path, "%s/shell_shape.bmp", MC_WORK);
            mc_video_set_geometry(v, w, h);
            mc_video_request_shot(v, path, !lcd);
            mc_video_present(v, frame, w, h);
            CHECK(mc_video_take_shot_result(v, saved, sizeof saved) && saved[0]);
            SDL_Surface *loaded = SDL_LoadBMP(path);
            SDL_Surface *shot = loaded ? SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_ARGB8888, 0) : NULL;
            CHECK(shot != NULL);
            if (shot) {
                CHECK(shot->w == w * scale && shot->h == h * scale);
                if (shot->w == w * scale && shot->h == h * scale) {
                    /* Guest pixel (x, y) at the centre of its scale x scale cell. */
                    static const int bright[][2] = {{0, 0}, {3, 3}, {-1, -1}, {-4, -4}, {-1, 0}, {0, -1}, {-4, 3}, {3, -4}, {10, 0}, {0, 10}};
                    static const int dark[][2] = {{4, 2}, {2, 4}, {-5, -3}, {-3, -5}, {10, 10}, {-10, -10}};
                    int ok = 1;
                    for (size_t i = 0; i < sizeof bright / sizeof bright[0]; i++) {
                        const int x = bright[i][0] < 0 ? w + bright[i][0] : bright[i][0], y = bright[i][1] < 0 ? h + bright[i][1] : bright[i][1];
                        if (luma(shot, x * scale + scale / 2, y * scale + scale / 2) < 96) ok = 0;
                    }
                    for (size_t i = 0; i < sizeof dark / sizeof dark[0]; i++) {
                        const int x = dark[i][0] < 0 ? w + dark[i][0] : dark[i][0], y = dark[i][1] < 0 ? h + dark[i][1] : dark[i][1];
                        if (luma(shot, x * scale + scale / 2, y * scale + scale / 2) > 48) ok = 0;
                    }
                    if (!ok) printf("window shape: %dx%d at %dx: the picture is not the whole screen with square pixels\n", w, h, scale);
                    CHECK(ok);
                }
                SDL_FreeSurface(shot);
            }
            SDL_FreeSurface(loaded);
            remove(path);
            mc_video_close(v);
        }
    }
    free(frame);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int main(void) {
    test_keys();
    test_window_shape();
    test_sound();
    test_capture();
#ifdef _WIN32
    test_serial();
#endif
    printf("shell: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
