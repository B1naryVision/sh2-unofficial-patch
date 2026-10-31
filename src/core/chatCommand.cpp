#include "chatCommand.h"
#include "gameWString.h"
#include "hook.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// See docs/features/ping-command.md.
static const uintptr_t CHAT_SUBMIT_RVA = 0x24d8e0; // IngameChatPanel::sendTypedMessage
static const uintptr_t WSTRING_ASSIGN_RVA =
    0x73e0; // std::wstring::assign(const wstring&, pos, len)

static const uintptr_t GAME_SCREEN_PTR_RVA = 0xec0de0; // registerWindow, GameScreen
static const uintptr_t GAME_SCREEN_CHAT_OFF = 0xf5e68; // IngameChatPanel member

static const uintptr_t CHAT_EXECUTE_SLOT_RVA = 0x5c7134; // ChatChore vtable 0x9c712c, slot 2
static const uintptr_t CHAT_EXECUTE_RVA = 0x1e89f0; // ChatChore::execute
static const uintptr_t CHORE_SENDER_OFF = 0x08; // sender's network peer id
static const uintptr_t CHORE_TEXT_OFF = 0x0c; // std::wstring

static const uintptr_t PANEL_TEXT_OFF = 0x2f8; // std::wstring holding the typed line
static const uintptr_t PANEL_CARET_OFF = 0x334; // cleared alongside the text on send

static const uint8_t CHAT_SUBMIT_BYTES[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};

static const int MAX_COMMANDS = 8;
static const int MAX_OBSERVERS = 4;

typedef void(__attribute__((thiscall)) * AssignFn)(
    void *dest, const GameWString *src, uint32_t pos, uint32_t len
);
typedef void(__attribute__((thiscall)) * ChatSubmitFn)(void *panel);
typedef void(__attribute__((thiscall)) * ChoreExecuteFn)(void *chore);

struct ChatCommand {
    const wchar_t *text;
    ChatCommandFn fn;
};

static ChatCommand s_commands[MAX_COMMANDS];
static int s_commandCount = 0;
static bool s_hookInstalled = false;
static bool s_hookFailed = false;
static bool s_posting = false;
static void *s_panel = nullptr;
static ChatObserverFn s_observers[MAX_OBSERVERS];
static int s_observerCount = 0;
static ChoreExecuteFn s_stockExecute = nullptr;
static bool s_executeSwapped = false;
static bool s_executeFailed = false;

// Set by the C handler, read by the trampoline after it restores registers.
uint8_t g_chatSwallowSubmit = 0;
uintptr_t g_chatSubmitResume = 0;

static uintptr_t gameBase() { return (uintptr_t)GetModuleHandleA(NULL); }

// MSVC wstring: capacity below 8 keeps the characters inline, otherwise the union
// holds a heap pointer. Never dereference without checking - a short string's first
// two characters form a plausible-looking address.
static const wchar_t *readWString(const GameWString *s, uint32_t *lengthOut) {
    if (!s || s->size == 0 || s->size > s->res) {
        return nullptr;
    }

    *lengthOut = s->size;

    if (s->res >= 8) {
        return s->heap;
    }

    return s->inl;
}

static bool textMatches(const wchar_t *text, uint32_t length, const wchar_t *command) {
    uint32_t n = 0;

    while (command[n]) {
        ++n;
    }

    while (length > 0 && text[length - 1] == L' ') {
        --length;
    }

    if (length != n) {
        return false;
    }

    for (uint32_t i = 0; i < n; ++i) {
        wchar_t c = text[i];

        if (c >= L'A' && c <= L'Z') {
            c = (wchar_t)(c + (L'a' - L'A'));
        }

        if (c != command[i]) {
            return false;
        }
    }

    return true;
}

// Mirrors what the stock send does to the input line once a message goes out.
static void clearPanelText(void *panel) {
    GameWString *text = (GameWString *)((uintptr_t)panel + PANEL_TEXT_OFF);
    wchar_t *buf = (text->res >= 8) ? text->heap : text->inl;

    if (buf) {
        buf[0] = 0;
    }

    text->size = 0;
    *(uint32_t *)((uintptr_t)panel + PANEL_CARET_OFF) = 0;
}

// Writes the line into the panel's own input buffer and lets the stock send build,
// refcount and post the chat message - the DLL never touches the game's allocator
// or its Handle discipline. The source only has to be readable, so a hand-built
// wstring with a capacity above the inline threshold is enough.
//
// `panel+0x308` is the input's _Mysize, which the stock send uses to refuse an empty
// line; the assign is what makes it non-zero, so it is no precondition here.
// The panel is a member of GameScreen, which is registered once at startup, so it
// can be found before the player has typed anything.
static void *findPanel() {
    uintptr_t screen = 0;

    if (s_panel) {
        return s_panel;
    }

    screen = *(uintptr_t *)(gameBase() + GAME_SCREEN_PTR_RVA);

    if (!screen) {
        return nullptr;
    }

    s_panel = (void *)(screen + GAME_SCREEN_CHAT_OFF);

    return s_panel;
}

bool chatPost(const wchar_t *text, int length) {
    uintptr_t base = gameBase();
    AssignFn assign = (AssignFn)(base + WSTRING_ASSIGN_RVA);
    ChatSubmitFn submit = (ChatSubmitFn)(base + CHAT_SUBMIT_RVA);
    GameWString src;
    // fxsave faults unless its destination is 16-byte aligned, and nothing guarantees
    // the alignment of a stack we entered from the game's main loop - so align by hand
    // rather than trusting the compiler's assumption about the incoming stack.
    uint8_t fpuBuffer[512 + 16];
    uint8_t *fpuState = (uint8_t *)(((uintptr_t)fpuBuffer + 15) & ~(uintptr_t)15);

    if (!findPanel() || !text || length <= 0) {
        return false;
    }

    src.heap = (wchar_t *)text;
    src.size = (uint32_t)length;
    src.res = 0x1000;

    // The frame tick is a mid-function hook, so the interrupted code may hold live
    // x87 state and the engine calls below need a clean stack of their own.
    __asm__ volatile("fxsave (%0)" : : "r"(fpuState) : "memory");
    __asm__ volatile("fninit");

    assign((void *)((uintptr_t)s_panel + PANEL_TEXT_OFF), &src, 0, 0xffffffff);

    s_posting = true;
    submit(s_panel);
    s_posting = false;

    __asm__ volatile("fxrstor (%0)" : : "r"(fpuState) : "memory");

    return true;
}

extern "C" int chatCommandSubmit(void *panel) {
    const wchar_t *chars = nullptr;
    uint32_t length = 0;

    if (s_posting || !panel) {
        return 0;
    }

    s_panel = panel;
    chars = readWString((const GameWString *)((uintptr_t)panel + PANEL_TEXT_OFF), &length);

    if (!chars) {
        return 0;
    }

    for (int i = 0; i < s_commandCount; ++i) {
        if (!textMatches(chars, length, s_commands[i].text)) {
            continue;
        }

        clearPanelText(panel);
        s_commands[i].fn();

        return 1;
    }

    return 0;
}

// Prologue hook, 5 bytes: `push ebp ; mov ebp,esp ; push -1`. The stock function
// takes no stack arguments (a bare `ret`), so swallowing a command is just a return.
__declspec(naked) static void chatSubmitHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "pushl %ecx\n\t"
                     "call _chatCommandSubmit\n\t"
                     "addl $4, %esp\n\t"
                     "movb %al, _g_chatSwallowSubmit\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "cmpb $0, _g_chatSwallowSubmit\n\t"
                     "jne .LchatSwallow\n\t"
                     "push %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $-1\n\t"
                     "jmp *_g_chatSubmitResume\n\t"
                     ".LchatSwallow:\n\t"
                     "ret\n\t");
}

// Runs in place of ChatChore::execute (thiscall, no stack arguments): the chat
// line as the simulation executes it. Observers run after the stock code has
// shown it, so a rules line is visible before anything acts on it.
static void __attribute__((thiscall)) chatExecuteHook(void *chore) {
    const wchar_t *chars = nullptr;
    uint32_t length = 0;
    uint32_t sender = 0;

    sender = *(uint32_t *)((uintptr_t)chore + CHORE_SENDER_OFF);
    chars = readWString((const GameWString *)((uintptr_t)chore + CHORE_TEXT_OFF), &length);

    // Copy before the stock call: nothing says the chore outlives it.
    wchar_t copy[256];
    int n = 0;

    if (chars) {
        for (; n < 255 && n < (int)length; ++n) {
            copy[n] = chars[n];
        }
    }

    copy[n] = 0;

    s_stockExecute(chore);

    if (!chars) {
        return;
    }

    for (int i = 0; i < s_observerCount; ++i) {
        s_observers[i](sender, copy, n);
    }
}

// The slot holds a relocated absolute address, so compare against the rebuilt
// one. It is the function's only reference, so the swap sees every execution.
void registerChatObserver(ChatObserverFn fn) {
    if (!fn || s_observerCount >= MAX_OBSERVERS || s_executeFailed) {
        return;
    }

    if (!s_executeSwapped) {
        uintptr_t base = gameBase();
        uintptr_t *slot = (uintptr_t *)(base + CHAT_EXECUTE_SLOT_RVA);
        DWORD oldProtect = 0;

        if (*slot != base + CHAT_EXECUTE_RVA ||
            !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) {
            s_executeFailed = true;
            return;
        }

        s_stockExecute = (ChoreExecuteFn)(base + CHAT_EXECUTE_RVA);
        *slot = (uintptr_t)&chatExecuteHook;
        VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);
        s_executeSwapped = true;
    }

    s_observers[s_observerCount++] = fn;
}

void registerChatCommand(const wchar_t *command, ChatCommandFn fn) {
    if (!command || !fn || s_commandCount >= MAX_COMMANDS || s_hookFailed) {
        return;
    }

    if (!s_hookInstalled) {
        uintptr_t base = gameBase();
        uint8_t *site = (uint8_t *)(base + CHAT_SUBMIT_RVA);

        if (memcmp(site, CHAT_SUBMIT_BYTES, sizeof(CHAT_SUBMIT_BYTES)) != 0) {
            s_hookFailed = true;
            return;
        }

        g_chatSubmitResume = base + CHAT_SUBMIT_RVA + 5;
        installHook(site, reinterpret_cast<void *>(chatSubmitHook), 5);
        s_hookInstalled = true;
    }

    s_commands[s_commandCount].text = command;
    s_commands[s_commandCount].fn = fn;
    ++s_commandCount;
}
