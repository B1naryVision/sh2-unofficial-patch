#include "pingCommand.h"
#include "../core/frameTick.h"
#include "../core/hook.h"
#include <cstdint>
#include <cstring>
#include <windows.h>

// See docs/features/ping-command.md.
static const uintptr_t CHAT_SUBMIT_RVA = 0x24d8e0; // IngameChatPanel::sendTypedMessage
static const uintptr_t MAINT_DISPATCH_RVA = 0x3d88c0; // GameSpy::Network maintenance handler
static const uintptr_t NETWORK_RVA = 0x26cd7d8; // GameSpy::Network singleton
static const uintptr_t WSTRING_ASSIGN_RVA =
    0x73e0; // std::wstring::assign(const wstring&, pos, len)
static const uintptr_t SEND_P2P_IAT_RVA = 0x500a1c; // OnlineInterface::SendP2PPacket
static const uintptr_t ONLINE_IFACE_IAT_RVA =
    0x500a44; // OnlineInterface::ms_instance (data import)
static const uintptr_t GET_MS_IAT_RVA = 0x500894; // Dragonfly::Time::getMilliSeconds

static const uintptr_t PANEL_TEXT_OFF = 0x2f8; // std::wstring holding the typed line
static const uintptr_t PANEL_SIZE_OFF = 0x308; // = text +0x10, its _Mysize: the stock
                                               // send refuses an empty line
static const uintptr_t PANEL_CARET_OFF = 0x334; // cleared alongside the text on send

static const uintptr_t PEER_BEGIN_OFF = 0x94; // vector<Peer> begin
static const uintptr_t PEER_END_OFF = 0x98; // vector<Peer> end
static const uintptr_t PEER_STRIDE = 0x40;
static const uintptr_t PEER_STATE_OFF = 0x08; // 1 = active in this session
static const uintptr_t PEER_NAME_OFF = 0x18; // std::wstring
static const uintptr_t PEER_ID_OFF = 0x34;
static const uintptr_t NET_LOCAL_PEER_OFF = 0xdc; // our own peer id; that entry is us

// Maintenance messages are `FF <id>` plus a payload; ids the stock build does not
// know are logged and dropped, so these two are inert against an unpatched client.
static const uint8_t MAINT_MARKER = 0xff;
static const uint8_t PING_REQUEST = 0x40;
static const uint8_t PING_REPLY = 0x41;
static const int PING_MSG_LEN = 6;

static const uint8_t CHAT_SUBMIT_BYTES[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
static const uint8_t MAINT_DISPATCH_BYTES[] = {0x55, 0x8b, 0xec, 0x53, 0x56};

static const int MAX_PEERS = 16;
static const int MAX_NAME = 24;
static const uint32_t COLLECT_TIMEOUT_MS = 1500;
static const uint32_t MAX_PLAUSIBLE_RTT_MS = 10000;
static const uint32_t REPLY_MIN_INTERVAL_MS = 200; // per-peer cap on how often we answer

struct GameWString {
    union {
        wchar_t *heap;
        wchar_t inl[8];
    };
    uint32_t size;
    uint32_t res;
};

struct OnlineId {
    uint64_t steamId;
    uint32_t valid;
};

typedef bool(__attribute__((thiscall)) * SendP2PFn)(
    void *self, const OnlineId *id, const void *data, unsigned len, int sendType, int channel
);
typedef void(__attribute__((thiscall)) * AssignFn)(
    void *dest, const GameWString *src, uint32_t pos, uint32_t len
);
typedef void(__attribute__((thiscall)) * ChatSubmitFn)(void *panel);
typedef int (*GetMsFn)();

struct PeerPing {
    uint64_t steamId;
    uint32_t sentMs;
    uint32_t replyMs;
    uint32_t lastRepliedMs;
    bool replied;
    wchar_t name[MAX_NAME];
};

static PeerPing s_peers[MAX_PEERS];
static int s_peerCount = 0;
static bool s_pending = false;
static bool s_broadcasting = false;
static uint32_t s_roundStartMs = 0;
static void *s_panel = nullptr;

// Set by the C handlers, read by the trampolines after they restore registers.
uint8_t g_pingSwallowSubmit = 0;
uint8_t g_pingHandledMessage = 0;
uintptr_t g_chatSubmitResume = 0;
uintptr_t g_maintResume = 0;

static uintptr_t gameBase() { return (uintptr_t)GetModuleHandleA(NULL); }

static uint32_t nowMs() {
    GetMsFn fn = *(GetMsFn *)(gameBase() + GET_MS_IAT_RVA);

    if (!fn) {
        return 0;
    }

    return (uint32_t)fn();
}

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

static void appendChars(wchar_t *buf, int cap, int *len, const wchar_t *text, int count) {
    for (int i = 0; i < count && *len < cap - 1; ++i) {
        buf[(*len)++] = text[i];
    }

    buf[*len] = 0;
}

static void appendText(wchar_t *buf, int cap, int *len, const wchar_t *text) {
    int n = 0;

    while (text[n]) {
        ++n;
    }

    appendChars(buf, cap, len, text, n);
}

static void appendNumber(wchar_t *buf, int cap, int *len, uint32_t value) {
    wchar_t digits[12];
    int n = 0;

    if (value == 0) {
        digits[n++] = L'0';
    }

    while (value > 0 && n < 11) {
        digits[n++] = (wchar_t)(L'0' + (value % 10));
        value /= 10;
    }

    for (int i = n - 1; i >= 0; --i) {
        appendChars(buf, cap, len, &digits[i], 1);
    }
}

static bool textIsPingCommand(const wchar_t *text, uint32_t length) {
    static const wchar_t *command = L"!ping";
    uint32_t i = 0;

    while (length > 0 && text[length - 1] == L' ') {
        --length;
    }

    if (length != 5) {
        return false;
    }

    for (i = 0; i < 5; ++i) {
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

static bool sendPingMessage(const OnlineId *id, uint8_t kind, uint32_t token) {
    uintptr_t base = gameBase();
    SendP2PFn send = *(SendP2PFn *)(base + SEND_P2P_IAT_RVA);
    void *iface = *(void **)(base + ONLINE_IFACE_IAT_RVA);
    uint8_t msg[PING_MSG_LEN];

    if (!send || !iface) {
        return false;
    }

    msg[0] = MAINT_MARKER;
    msg[1] = kind;
    memcpy(msg + 2, &token, sizeof(token));

    return send(iface, id, msg, PING_MSG_LEN, 1, 3);
}

// Snapshots the active peers and sends each a request stamped with the send time,
// so a reply carries everything needed to work out the round trip.
static void startRound() {
    uintptr_t base = gameBase();
    uintptr_t net = base + NETWORK_RVA;
    uintptr_t begin = *(uintptr_t *)(net + PEER_BEGIN_OFF);
    uintptr_t end = *(uintptr_t *)(net + PEER_END_OFF);
    uint32_t localPeer = *(uint32_t *)(net + NET_LOCAL_PEER_OFF);
    uint32_t now = nowMs();

    s_peerCount = 0;
    s_pending = true;
    s_roundStartMs = now;

    if (!begin || end <= begin) {
        return;
    }

    for (uintptr_t p = begin; p + PEER_STRIDE <= end && s_peerCount < MAX_PEERS; p += PEER_STRIDE) {
        PeerPing &slot = s_peers[s_peerCount];
        const wchar_t *name = nullptr;
        uint32_t nameLen = 0;

        if (*(uint32_t *)(p + PEER_STATE_OFF) != 1) {
            continue;
        }

        // The peer list includes us; the stock keepalive loop skips that entry the
        // same way, by matching the peer id against the network object's own.
        if (*(uint32_t *)(p + PEER_ID_OFF) == localPeer) {
            continue;
        }

        slot.steamId = *(uint64_t *)p;
        slot.sentMs = now;
        slot.replyMs = 0;
        slot.replied = false;
        slot.lastRepliedMs = 0;
        slot.name[0] = 0;

        name = readWString((const GameWString *)(p + PEER_NAME_OFF), &nameLen);

        if (name) {
            uint32_t n = nameLen;

            if (n > MAX_NAME - 1) {
                n = MAX_NAME - 1;
            }

            memcpy(slot.name, name, n * sizeof(wchar_t));
            slot.name[n] = 0;
        }

        ++s_peerCount;

        OnlineId id = {slot.steamId, 1};
        sendPingMessage(&id, PING_REQUEST, now);
    }
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

// Writes our line into the panel's own input buffer and lets the stock send build,
// refcount and post the chat message - the DLL never touches the game's allocator
// or its Handle discipline. The source only has to be readable, so a hand-built
// wstring with a capacity above the inline threshold is enough.
static void broadcast(const wchar_t *text, int length) {
    uintptr_t base = gameBase();
    AssignFn assign = (AssignFn)(base + WSTRING_ASSIGN_RVA);
    ChatSubmitFn submit = (ChatSubmitFn)(base + CHAT_SUBMIT_RVA);
    GameWString src;
    // fxsave faults unless its destination is 16-byte aligned, and nothing guarantees
    // the alignment of a stack we entered from the game's main loop - so align by hand
    // rather than trusting the compiler's assumption about the incoming stack.
    uint8_t fpuBuffer[512 + 16];
    uint8_t *fpuState = (uint8_t *)(((uintptr_t)fpuBuffer + 15) & ~(uintptr_t)15);

    if (!s_panel) {
        return;
    }

    src.heap = (wchar_t *)text;
    src.size = (uint32_t)length;
    src.res = 0x1000;

    // The frame tick is a mid-function hook, so the interrupted code may hold live
    // x87 state and the engine calls below need a clean stack of their own.
    __asm__ volatile("fxsave (%0)" : : "r"(fpuState) : "memory");
    __asm__ volatile("fninit");

    assign((void *)((uintptr_t)s_panel + PANEL_TEXT_OFF), &src, 0, 0xffffffff);

    s_broadcasting = true;
    submit(s_panel);
    s_broadcasting = false;

    __asm__ volatile("fxrstor (%0)" : : "r"(fpuState) : "memory");
}

static void reportRound() {
    wchar_t line[256];
    int len = 0;

    appendText(line, 256, &len, L"Ping:");

    // Always say something. A command that silently does nothing cannot be told
    // apart from one that is broken.
    if (s_peerCount == 0) {
        appendText(line, 256, &len, L" no other players");
        broadcast(line, len);

        return;
    }

    for (int i = 0; i < s_peerCount; ++i) {
        appendText(line, 256, &len, L" ");

        if (s_peers[i].name[0]) {
            appendText(line, 256, &len, s_peers[i].name);
        } else {
            appendText(line, 256, &len, L"?");
        }

        if (s_peers[i].replied) {
            appendText(line, 256, &len, L" ");
            appendNumber(line, 256, &len, s_peers[i].replyMs);
            appendText(line, 256, &len, L"ms");
        } else {
            appendText(line, 256, &len, L" n/a");
        }

        if (i + 1 < s_peerCount) {
            appendText(line, 256, &len, L",");
        }
    }

    if (len > 0) {
        broadcast(line, len);
    }
}

extern "C" int pingChatSubmit(void *panel) {
    const GameWString *text = nullptr;
    const wchar_t *chars = nullptr;
    uint32_t length = 0;

    if (s_broadcasting || !panel) {
        return 0;
    }

    text = (const GameWString *)((uintptr_t)panel + PANEL_TEXT_OFF);
    chars = readWString(text, &length);

    if (!chars || !textIsPingCommand(chars, length)) {
        return 0;
    }

    s_panel = panel;
    clearPanelText(panel);
    startRound();

    return 1;
}

// Runs at the maintenance handler's prologue, on the thread that reads P2P packets.
// Everything here treats the packet as hostile: fixed length, exact match, no loops
// over payload, no allocation, and a per-peer cap on how often we will answer.
extern "C" int pingMaintMessage(const void *sender, const uint8_t *msg, int length) {
    uint32_t token = 0;
    uint32_t now = 0;

    if (!sender || !msg || length != PING_MSG_LEN) {
        return 0;
    }

    if (msg[0] != MAINT_MARKER) {
        return 0;
    }

    if (msg[1] != PING_REQUEST && msg[1] != PING_REPLY) {
        return 0;
    }

    memcpy(&token, msg + 2, sizeof(token));
    now = nowMs();

    if (msg[1] == PING_REQUEST) {
        uint64_t from = *(const uint64_t *)sender;

        for (int i = 0; i < s_peerCount; ++i) {
            if (s_peers[i].steamId != from) {
                continue;
            }

            if (s_peers[i].lastRepliedMs &&
                now - s_peers[i].lastRepliedMs < REPLY_MIN_INTERVAL_MS) {
                return 1;
            }

            s_peers[i].lastRepliedMs = now;
            break;
        }

        sendPingMessage((const OnlineId *)sender, PING_REPLY, token);

        return 1;
    }

    if (s_pending) {
        uint64_t from = *(const uint64_t *)sender;
        uint32_t rtt = now - token;

        if (rtt <= MAX_PLAUSIBLE_RTT_MS) {
            for (int i = 0; i < s_peerCount; ++i) {
                if (s_peers[i].steamId == from && !s_peers[i].replied) {
                    s_peers[i].replied = true;
                    s_peers[i].replyMs = rtt;
                    break;
                }
            }
        }
    }

    return 1;
}

extern "C" void pingTick() {
    bool complete = true;

    if (!s_pending) {
        return;
    }

    for (int i = 0; i < s_peerCount; ++i) {
        if (!s_peers[i].replied) {
            complete = false;
            break;
        }
    }

    if (!complete && nowMs() - s_roundStartMs < COLLECT_TIMEOUT_MS) {
        return;
    }

    s_pending = false;
    reportRound();
}

// Prologue hook, 5 bytes: `push ebp ; mov ebp,esp ; push -1`. The stock function
// takes no stack arguments (a bare `ret`), so swallowing "!ping" is just a return.
__declspec(naked) static void chatSubmitHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "pushl %ecx\n\t"
                     "call _pingChatSubmit\n\t"
                     "addl $4, %esp\n\t"
                     "movb %al, _g_pingSwallowSubmit\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "cmpb $0, _g_pingSwallowSubmit\n\t"
                     "jne .LpingSwallow\n\t"
                     "push %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "pushl $-1\n\t"
                     "jmp *_g_chatSubmitResume\n\t"
                     ".LpingSwallow:\n\t"
                     "ret\n\t");
}

// Prologue hook, 5 bytes: `push ebp ; mov ebp,esp ; push ebx ; push esi`. Arguments
// are (senderOnlineID, message, length) and the callee cleans them (`ret 0xc`).
__declspec(naked) static void maintDispatchHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "pushl 0x30(%esp)\n\t"
                     "pushl 0x30(%esp)\n\t"
                     "pushl 0x30(%esp)\n\t"
                     "call _pingMaintMessage\n\t"
                     "addl $12, %esp\n\t"
                     "movb %al, _g_pingHandledMessage\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "cmpb $0, _g_pingHandledMessage\n\t"
                     "jne .LpingHandled\n\t"
                     "push %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "push %ebx\n\t"
                     "push %esi\n\t"
                     "jmp *_g_maintResume\n\t"
                     ".LpingHandled:\n\t"
                     "ret $0xc\n\t");
}

void installPingCommand() {
    uintptr_t base = gameBase();
    uint8_t *submitSite = (uint8_t *)(base + CHAT_SUBMIT_RVA);
    uint8_t *maintSite = (uint8_t *)(base + MAINT_DISPATCH_RVA);

    // All-or-nothing: both prologues and both imports must look right.
    if (memcmp(submitSite, CHAT_SUBMIT_BYTES, sizeof(CHAT_SUBMIT_BYTES)) != 0) {
        return;
    }

    if (memcmp(maintSite, MAINT_DISPATCH_BYTES, sizeof(MAINT_DISPATCH_BYTES)) != 0) {
        return;
    }

    if (*(void **)(base + SEND_P2P_IAT_RVA) == nullptr) {
        return;
    }

    if (*(void **)(base + ONLINE_IFACE_IAT_RVA) == nullptr) {
        return;
    }

    if (*(void **)(base + GET_MS_IAT_RVA) == nullptr) {
        return;
    }

    g_chatSubmitResume = base + CHAT_SUBMIT_RVA + 5;
    g_maintResume = base + MAINT_DISPATCH_RVA + 5;

    installHook(submitSite, reinterpret_cast<void *>(chatSubmitHook), 5);
    installHook(maintSite, reinterpret_cast<void *>(maintDispatchHook), 5);
    registerFrameTick(pingTick);
}
