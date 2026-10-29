#include "screens.h"
#include <cstdint>
#include <windows.h>

static const uintptr_t LOBBY_SCREEN_PTR_RVA = 0xec5f28; // registerWindow("scenario lobby screen")
static const uintptr_t GAME_SCREEN_PTR_RVA = 0xec0de0; // registerWindow, GameScreen
static const uintptr_t PANE_PARENT_OFF = 0x80;

static bool screenUp(uintptr_t pointerRva) {
    uintptr_t screen = *(uintptr_t *)((uintptr_t)GetModuleHandleA(NULL) + pointerRva);

    if (!screen) {
        return false;
    }

    return *(uintptr_t *)(screen + PANE_PARENT_OFF) != 0;
}

bool lobbyScreenUp() { return screenUp(LOBBY_SCREEN_PTR_RVA); }

bool gameScreenUp() { return screenUp(GAME_SCREEN_PTR_RVA); }
