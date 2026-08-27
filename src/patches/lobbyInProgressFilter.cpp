#include "lobbyInProgressFilter.h"
#include "../core/config.h"
#include "../core/hook.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// See docs/bugs/in-progress-lobbies.md.
static const uintptr_t KEY_SITE_RVA = 0x3e065b; // push "Staging" in the data-update row builder
static const uintptr_t HOOK_SITE_RVA = 0x3e06bf; // mov ecx, <list model> before that row add
static const uintptr_t ROW_ADD_RVA = 0x3e06c4; // call that inserts/updates the row
static const uintptr_t EPILOGUE_RVA = 0x3e06c9; // pop edi/esi/ebx/ebp ; ret 0xc
static const uintptr_t LIST_SITE_RVA = 0x3e05bd; // push ebx (flag 0) in the list-arrival path
static const uintptr_t LIST_BACK_RVA = 0x3e05c3; // rest of that row add's argument setup
static const uintptr_t LIST_SKIP_RVA = 0x3e05e7; // next iteration of the lobby loop
static const uintptr_t STAGING_STR_RVA = 0x5f8884; // "Staging" - nothing ever publishes it
static const uintptr_t PLAYING_STR_RVA = 0x5e1898; // "Playing" - what the host actually publishes
static const uintptr_t LIST_MODEL_RVA = 0x26cdae8; // lobby browser list model
static const uintptr_t GET_LOBBY_DATA_IAT_RVA = 0x5009ec; // OnlineLobbyLister::GetLobbyData

// The list-arrival path hard-codes its in-progress flag to 0, so unlike the data-update path
// there is no flag to correct - the lookup has to be done here.
static const uint8_t LIST_SITE_BYTES[] = {0x53, 0x83, 0xec, 0x10, 0x8b, 0xcc};

// Re-emit state for the data-update trampoline. The overwritten 5 bytes are `mov ecx, imm32`
// whose immediate is an ASLR-relocated module address, captured live.
uint32_t g_lobbyListModel = 0;
uintptr_t g_lobbyRowAdd = 0;
uintptr_t g_lobbyRowSkip = 0;

// Re-emit state for the list-arrival trampoline, plus its verdict. The overwritten 6 bytes
// are position-independent, so only the resume addresses are needed.
uint8_t g_lobbySkipRow = 0;
uintptr_t g_lobbyListBack = 0;
uintptr_t g_lobbyListSkip = 0;

// Reads an absolute-address instruction operand by rebuilding what the loader
// would have relocated it to, rather than comparing against stale disk bytes.
static bool isOperand(const uint8_t *site, uint8_t opcode, uintptr_t expected) {
    uint32_t operand = 0;

    if (site[0] != opcode) {
        return false;
    }

    memcpy(&operand, site + 1, sizeof(operand));

    return operand == (uint32_t)expected;
}

static bool writeOperand(uint8_t *site, uintptr_t value) {
    DWORD oldProtect;
    uint32_t operand = (uint32_t)value;

    if (!VirtualProtect(site + 1, sizeof(operand), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    memcpy(site + 1, &operand, sizeof(operand));
    VirtualProtect(site + 1, sizeof(operand), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), site, 5);

    return true;
}

// GetLobbyData(OnlineID byval, const char *key) is thiscall and cleans its own 0x14 bytes.
// Reproduce the stock call shape rather than declaring a signature and trusting the compiler
// to lay out a 16-byte by-value struct the same way MSVC does: key pushed first, then the id
// copied into a stack slot the callee pops itself.
// Must have external C linkage: a static function whose body is hand-written asm is still a
// function GCC believes it owns, and with every caller visible it will switch it to a private
// register-based convention - which the asm, reading its arguments off the stack, then
// misreads (an earlier version took a stack slot as the callee address and jumped to 0).
extern "C" const char *
callGetLobbyData(void *lister, const uint32_t *id, const char *key, void *fn);

__declspec(naked) const char *
callGetLobbyData(void * /*lister*/, const uint32_t * /*id*/, const char * /*key*/, void * /*fn*/) {
    __asm__ volatile("push %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "push %esi\n\t"
                     "movl 0xc(%ebp), %esi\n\t"
                     "pushl 0x10(%ebp)\n\t"
                     "subl $0x10, %esp\n\t"
                     "movl (%esi), %eax\n\t"
                     "movl %eax, (%esp)\n\t"
                     "movl 0x4(%esi), %eax\n\t"
                     "movl %eax, 0x4(%esp)\n\t"
                     "movl 0x8(%esi), %eax\n\t"
                     "movl %eax, 0x8(%esp)\n\t"
                     "movl 0xc(%esi), %eax\n\t"
                     "movl %eax, 0xc(%esp)\n\t"
                     "movl 0x8(%ebp), %ecx\n\t"
                     "call *0x14(%ebp)\n\t"
                     "pop %esi\n\t"
                     "pop %ebp\n\t"
                     "ret\n\t");
}

// Asks Steam whether this lobby's game has already started. Reached from a mid-function hook,
// so it must stay free of floating-point work - it does string comparison only.
extern "C" void lobbyListRowCheck(void *lister, const uint32_t *id) {
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    void *fn = *(void **)(base + GET_LOBBY_DATA_IAT_RVA);

    g_lobbySkipRow = 0;

    if (!lister || !fn) {
        return;
    }

    const char *value = callGetLobbyData(lister, id, (const char *)(base + PLAYING_STR_RVA), fn);

    if (value && value[0] == '1' && value[1] == '\0') {
        g_lobbySkipRow = 1;
    }
}

// Hook site RVA 0x3e06bf, 5 bytes: `mov ecx, <list model>`, the last instruction before the
// data-update path hands a lobby to the browser's list. The in-progress flag that path has
// just computed is the byte at [ebp+0x10]; when it is set, the row is dropped instead of
// added. Skipping the call means cleaning up its arguments here - it is a `ret 0x18` callee,
// and the 0x18 bytes (flag, the 16-byte lobby id, host name) are already on the stack.
__declspec(naked) static void rowBuilderHook() {
    __asm__ volatile("cmpb $0, 0x10(%ebp)\n\t"
                     "jne .Lskip\n\t"
                     "movl _g_lobbyListModel, %ecx\n\t"
                     "jmp *_g_lobbyRowAdd\n\t"
                     ".Lskip:\n\t"
                     "addl $0x18, %esp\n\t"
                     "jmp *_g_lobbyRowSkip\n\t");
}

// Hook site RVA 0x3e05bd, 6 bytes: `push ebx ; sub esp,0x10 ; mov ecx,esp` - the start of the
// list-arrival path's argument setup for the same row add, where the flag is the hard-coded
// zero in ebx. The lobby id is the 16 bytes at [ebp-0x5c] and the lister is [ebp-0x60]; when
// the lookup says the game has started, jump straight to the loop's next iteration. Nothing
// has been pushed at that point, so the skip path needs no stack cleanup.
__declspec(naked) static void listRowHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "leal -0x5c(%ebp), %eax\n\t"
                     "pushl %eax\n\t"
                     "pushl -0x60(%ebp)\n\t"
                     "call _lobbyListRowCheck\n\t"
                     "addl $8, %esp\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "cmpb $0, _g_lobbySkipRow\n\t"
                     "jne .LskipRow\n\t"
                     "push %ebx\n\t"
                     "subl $0x10, %esp\n\t"
                     "movl %esp, %ecx\n\t"
                     "jmp *_g_lobbyListBack\n\t"
                     ".LskipRow:\n\t"
                     "jmp *_g_lobbyListSkip\n\t");
}

void installLobbyInProgressFilter() {
    if (configInt("multiplayer", "HideInProgressLobbies", 1) != 1) {
        return;
    }

    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uint8_t *keySite = (uint8_t *)(base + KEY_SITE_RVA);
    uint8_t *hookSite = (uint8_t *)(base + HOOK_SITE_RVA);
    uint8_t *listSite = (uint8_t *)(base + LIST_SITE_RVA);

    // All-or-nothing: verify every site before writing any of them.
    if (!isOperand(keySite, 0x68, base + STAGING_STR_RVA)) {
        return;
    }

    if (!isOperand(hookSite, 0xb9, base + LIST_MODEL_RVA)) {
        return;
    }

    if (memcmp(listSite, LIST_SITE_BYTES, sizeof(LIST_SITE_BYTES)) != 0) {
        return;
    }

    if (*(void **)(base + GET_LOBBY_DATA_IAT_RVA) == nullptr) {
        return;
    }

    if (!writeOperand(keySite, base + PLAYING_STR_RVA)) {
        return;
    }

    memcpy(&g_lobbyListModel, hookSite + 1, sizeof(g_lobbyListModel));
    g_lobbyRowAdd = base + ROW_ADD_RVA;
    g_lobbyRowSkip = base + EPILOGUE_RVA;
    installHook((void *)hookSite, reinterpret_cast<void *>(rowBuilderHook), 5);

    g_lobbyListBack = base + LIST_BACK_RVA;
    g_lobbyListSkip = base + LIST_SKIP_RVA;
    installHook((void *)listSite, reinterpret_cast<void *>(listRowHook), 6);
}
