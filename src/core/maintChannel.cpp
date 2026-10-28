#include "maintChannel.h"
#include "hook.h"
#include <cstring>
#include <windows.h>

// See docs/features/ping-command.md.
static const uintptr_t MAINT_DISPATCH_RVA = 0x3d88c0; // GameSpy::Network maintenance handler
static const uintptr_t NETWORK_RVA = 0x26cd7d8; // GameSpy::Network singleton
static const uintptr_t SEND_P2P_IAT_RVA = 0x500a1c; // OnlineInterface::SendP2PPacket
static const uintptr_t ONLINE_IFACE_IAT_RVA =
    0x500a44; // OnlineInterface::ms_instance (data import)
static const uintptr_t GET_MS_IAT_RVA = 0x500894; // Dragonfly::Time::getMilliSeconds

static const uintptr_t PEER_BEGIN_OFF = 0x94; // vector<Peer> begin
static const uintptr_t PEER_END_OFF = 0x98; // vector<Peer> end
static const uintptr_t PEER_STRIDE = 0x40;
static const uintptr_t PEER_STATE_OFF = 0x08; // 1 = active in this session
static const uintptr_t PEER_NAME_OFF = 0x18; // std::wstring
static const uintptr_t PEER_ID_OFF = 0x34;
static const uintptr_t NET_LOCAL_PEER_OFF = 0xdc; // our own peer id; that entry is us

static const uint8_t MAINT_DISPATCH_BYTES[] = {0x55, 0x8b, 0xec, 0x53, 0x56};

// Largest message any feature sends; the stock keepalive is two bytes.
static const int MAX_MSG_LEN = 64;

static const int MAX_HANDLERS = 8;

struct GameWString {
    union {
        wchar_t *heap;
        wchar_t inl[8];
    };
    uint32_t size;
    uint32_t res;
};

typedef bool(__attribute__((thiscall)) * SendP2PFn)(
    void *self, const MaintOnlineId *id, const void *data, unsigned len, int sendType, int channel
);
typedef int (*GetMsFn)();

static MaintHandlerFn s_handlers[MAX_HANDLERS];
static int s_handlerCount = 0;
static bool s_hookInstalled = false;
static bool s_hookFailed = false;

// Set by the C handler, read by the trampoline after it restores registers.
uint8_t g_maintHandled = 0;
uintptr_t g_maintResume = 0;

static uintptr_t gameBase() { return (uintptr_t)GetModuleHandleA(NULL); }

uint32_t maintNowMs() {
    GetMsFn fn = *(GetMsFn *)(gameBase() + GET_MS_IAT_RVA);

    if (!fn) {
        return 0;
    }

    return (uint32_t)fn();
}

bool maintChannelAvailable() {
    uintptr_t base = gameBase();

    if (*(void **)(base + SEND_P2P_IAT_RVA) == nullptr) {
        return false;
    }

    if (*(void **)(base + ONLINE_IFACE_IAT_RVA) == nullptr) {
        return false;
    }

    return *(void **)(base + GET_MS_IAT_RVA) != nullptr;
}

bool maintSend(const MaintOnlineId *to, uint8_t id, const void *payload, int payloadLen) {
    uintptr_t base = gameBase();
    SendP2PFn send = *(SendP2PFn *)(base + SEND_P2P_IAT_RVA);
    void *iface = *(void **)(base + ONLINE_IFACE_IAT_RVA);
    uint8_t msg[MAX_MSG_LEN];

    if (!send || !iface || !to) {
        return false;
    }

    if (payloadLen < 0 || payloadLen > MAX_MSG_LEN - 2) {
        return false;
    }

    msg[0] = MAINT_MARKER;
    msg[1] = id;

    if (payloadLen > 0) {
        memcpy(msg + 2, payload, payloadLen);
    }

    return send(iface, to, msg, (unsigned)(payloadLen + 2), 1, 3);
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

// Walks the peer vector, calling `visit` for each active entry; `self` marks ours.
template <typename Visit> static void walkPeers(Visit visit) {
    uintptr_t net = gameBase() + NETWORK_RVA;
    uintptr_t begin = *(uintptr_t *)(net + PEER_BEGIN_OFF);
    uintptr_t end = *(uintptr_t *)(net + PEER_END_OFF);
    uint32_t localPeer = *(uint32_t *)(net + NET_LOCAL_PEER_OFF);

    if (!begin || end <= begin) {
        return;
    }

    for (uintptr_t p = begin; p + PEER_STRIDE <= end; p += PEER_STRIDE) {
        if (*(uint32_t *)(p + PEER_STATE_OFF) != 1) {
            continue;
        }

        // The peer list includes us; the stock keepalive loop skips that entry the
        // same way, by matching the peer id against the network object's own.
        bool self = *(uint32_t *)(p + PEER_ID_OFF) == localPeer;

        if (!visit(p, self)) {
            return;
        }
    }
}

int maintPeers(MaintPeer *out, int max) {
    int count = 0;

    walkPeers([&](uintptr_t p, bool self) {
        if (self) {
            return true;
        }

        if (count >= max) {
            return false;
        }

        MaintPeer &slot = out[count++];
        const wchar_t *name = nullptr;
        uint32_t nameLen = 0;

        slot.steamId = *(uint64_t *)p;
        slot.name[0] = 0;
        name = readWString((const GameWString *)(p + PEER_NAME_OFF), &nameLen);

        if (name) {
            uint32_t n = nameLen;

            if (n > MAINT_MAX_NAME - 1) {
                n = MAINT_MAX_NAME - 1;
            }

            memcpy(slot.name, name, n * sizeof(wchar_t));
            slot.name[n] = 0;
        }

        return true;
    });

    return count;
}

uint64_t maintLocalSteamId() {
    uint64_t id = 0;

    walkPeers([&](uintptr_t p, bool self) {
        if (!self) {
            return true;
        }

        id = *(uint64_t *)p;
        return false;
    });

    return id;
}

extern "C" int maintDispatch(const MaintOnlineId *sender, const uint8_t *msg, int length) {
    if (!sender || !msg || length < 2 || msg[0] != MAINT_MARKER) {
        return 0;
    }

    for (int i = 0; i < s_handlerCount; ++i) {
        if (s_handlers[i](sender, msg, length)) {
            return 1;
        }
    }

    return 0;
}

// Prologue hook, 5 bytes: `push ebp ; mov ebp,esp ; push ebx ; push esi`. Arguments
// are (senderOnlineID, message, length) and the callee cleans them (`ret 0xc`).
__declspec(naked) static void maintDispatchHook() {
    __asm__ volatile("pushal\n\t"
                     "pushfl\n\t"
                     "pushl 0x30(%esp)\n\t"
                     "pushl 0x30(%esp)\n\t"
                     "pushl 0x30(%esp)\n\t"
                     "call _maintDispatch\n\t"
                     "addl $12, %esp\n\t"
                     "movb %al, _g_maintHandled\n\t"
                     "popfl\n\t"
                     "popal\n\t"
                     "cmpb $0, _g_maintHandled\n\t"
                     "jne .LmaintHandled\n\t"
                     "push %ebp\n\t"
                     "movl %esp, %ebp\n\t"
                     "push %ebx\n\t"
                     "push %esi\n\t"
                     "jmp *_g_maintResume\n\t"
                     ".LmaintHandled:\n\t"
                     "ret $0xc\n\t");
}

// Register at install time, before the network layer is running: the hook is
// written on the first registration.
void registerMaintHandler(MaintHandlerFn fn) {
    if (!fn || s_handlerCount >= MAX_HANDLERS || s_hookFailed) {
        return;
    }

    if (!s_hookInstalled) {
        uintptr_t base = gameBase();
        uint8_t *site = (uint8_t *)(base + MAINT_DISPATCH_RVA);

        if (memcmp(site, MAINT_DISPATCH_BYTES, sizeof(MAINT_DISPATCH_BYTES)) != 0 ||
            !maintChannelAvailable()) {
            s_hookFailed = true;
            return;
        }

        g_maintResume = base + MAINT_DISPATCH_RVA + 5;
        installHook(site, reinterpret_cast<void *>(maintDispatchHook), 5);
        s_hookInstalled = true;
    }

    s_handlers[s_handlerCount++] = fn;
}
