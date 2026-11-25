/* Host keys to ADB key codes, by position: the codes the Apple Extended
 * Keyboard sends (Guide to the Macintosh Family Hardware, 2nd ed., "Apple
 * Desktop Bus", keyboard key-code figures; right-hand modifiers as sent in
 * device handler 3). */
#include "desktop.h"

#include <SDL.h>

int mc_desktop_adb_key(int scancode, uint32_t command_key) {
    /* Command is Left Alt unless input.keyboard.command names another of
     * the Alt and Control keys; the other Alt keys are Option, the other
     * Control keys Control. The Windows keys open the configuration window
     * (desktop.c) and reach the Macintosh as nothing. */
    switch (scancode) {
    case SDL_SCANCODE_LCTRL: if (command_key == MC_COMMAND_LEFT_CONTROL) return 0x37; break;
    case SDL_SCANCODE_LALT: if (command_key == MC_COMMAND_LEFT_ALT) return 0x37; break;
    case SDL_SCANCODE_RCTRL: if (command_key == MC_COMMAND_RIGHT_CONTROL) return 0x37; break;
    case SDL_SCANCODE_RALT: if (command_key == MC_COMMAND_RIGHT_ALT) return 0x37; break;
    case SDL_SCANCODE_LGUI: case SDL_SCANCODE_RGUI: return -1;
    default: break;
    }
    switch (scancode) {
#define K(s, v) case SDL_SCANCODE_##s: return v
    K(A, 0x00); K(S, 0x01); K(D, 0x02); K(F, 0x03); K(H, 0x04); K(G, 0x05); K(Z, 0x06); K(X, 0x07);
    K(C, 0x08); K(V, 0x09); K(NONUSBACKSLASH, 0x0a); K(B, 0x0b); K(Q, 0x0c); K(W, 0x0d); K(E, 0x0e); K(R, 0x0f);
    K(Y, 0x10); K(T, 0x11); K(1, 0x12); K(2, 0x13); K(3, 0x14); K(4, 0x15); K(6, 0x16); K(5, 0x17);
    K(EQUALS, 0x18); K(9, 0x19); K(7, 0x1a); K(MINUS, 0x1b); K(8, 0x1c); K(0, 0x1d); K(RIGHTBRACKET, 0x1e); K(O, 0x1f);
    K(U, 0x20); K(LEFTBRACKET, 0x21); K(I, 0x22); K(P, 0x23); K(RETURN, 0x24); K(L, 0x25); K(J, 0x26); K(APOSTROPHE, 0x27);
    K(K, 0x28); K(SEMICOLON, 0x29); K(BACKSLASH, 0x2a); K(COMMA, 0x2b); K(SLASH, 0x2c); K(N, 0x2d); K(M, 0x2e); K(PERIOD, 0x2f);
    K(TAB, 0x30); K(SPACE, 0x31); K(GRAVE, 0x32); K(BACKSPACE, 0x33); K(ESCAPE, 0x35);
    K(LCTRL, 0x36); K(LSHIFT, 0x38); K(CAPSLOCK, 0x39); K(LALT, 0x3a);
    K(LEFT, 0x3b); K(RIGHT, 0x3c); K(DOWN, 0x3d); K(UP, 0x3e);
    K(KP_PERIOD, 0x41); K(KP_MULTIPLY, 0x43); K(KP_PLUS, 0x45); K(NUMLOCKCLEAR, 0x47); K(KP_DIVIDE, 0x4b);
    K(KP_ENTER, 0x4c); K(KP_MINUS, 0x4e); K(KP_EQUALS, 0x51); K(KP_0, 0x52); K(KP_1, 0x53); K(KP_2, 0x54);
    K(KP_3, 0x55); K(KP_4, 0x56); K(KP_5, 0x57); K(KP_6, 0x58); K(KP_7, 0x59); K(KP_8, 0x5b); K(KP_9, 0x5c);
    K(F5, 0x60); K(F6, 0x61); K(F7, 0x62); K(F3, 0x63); K(F8, 0x64); K(F9, 0x65); K(F11, 0x67);
    K(PRINTSCREEN, 0x69); K(SCROLLLOCK, 0x6b); K(F10, 0x6d); K(F12, 0x6f); K(PAUSE, 0x71);
    K(INSERT, 0x72); K(HOME, 0x73); K(PAGEUP, 0x74); K(DELETE, 0x75); K(F4, 0x76); K(END, 0x77);
    K(F2, 0x78); K(PAGEDOWN, 0x79); K(F1, 0x7a); K(RSHIFT, 0x7b); K(RALT, 0x7c); K(RCTRL, 0x7d);
#undef K
    default: return -1;
    }
}
