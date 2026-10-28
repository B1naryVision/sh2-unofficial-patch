#include "steamLobby.h"
#include <cstring>
#include <windows.h>

// See docs/features/patch-handshake.md.
static const uintptr_t ONLINE_LOBBY_RVA = 0x26cd918; // Online::OnlineLobby, GameSpy::Network+0x140

static const uintptr_t LOBBY_ID_OFF = 0x10; // OnlineID: SteamID (8) then a valid dword
static const uintptr_t LOBBY_ID_VALID_OFF = 0x18;
static const uintptr_t LOBBY_TYPE_OFF = 0x20; // OnlineType; 1 = Steam
static const uintptr_t LOBBY_OWNER_OFF = 0x28; // what OnlineLobby::IsLocalPlayerOwner returns

// ISteamMatchmaking (SDK interface v009) slots, each cross-checked against the
// wrapper in ffonlinelib.dll that calls it.
static const int SM_GET_NUM_MEMBERS = 0x44 / 4;
static const int SM_GET_MEMBER_BY_INDEX = 0x48 / 4;
static const int SM_GET_LOBBY_DATA = 0x4c / 4;
static const int SM_SET_LOBBY_DATA = 0x50 / 4;
static const int SM_GET_MEMBER_DATA = 0x60 / 4;
static const int SM_SET_MEMBER_DATA = 0x64 / 4;
static const int SM_GET_OWNER = 0x8c / 4;

static const uintptr_t ONLINE_IFACE_IAT_RVA =
    0x500a44; // OnlineInterface::ms_instance (data import)

static const uint32_t ONLINE_TYPE_STEAM = 1;

static const char *GET_LOCAL_ID_EXPORT = "?GetLocalOnlineID64@OnlineInterface@Online@@QBE_KXZ";
static const char *GET_PERSONA_EXPORT =
    "?GetFriendPersonaName@OnlineInterface@Online@@QAEPBDVOnlineID@2@@Z";

// Online::OnlineID as passed by value: the callee pops 0x10 bytes.
struct OnlineIdArg {
    uint64_t steamId;
    uint32_t valid;
    uint32_t pad;
};

typedef void *(*SteamMatchmakingFn)();
typedef uint64_t(__attribute__((thiscall)) * GetLocalIdFn)(void *self);
typedef const char *(__attribute__((thiscall)) * GetPersonaFn)(void *self, OnlineIdArg id);
typedef int(__attribute__((thiscall)) * GetNumMembersFn)(void *self, uint64_t lobby);
// CSteamID has constructors, so MSVC returns it through a hidden pointer pushed
// after the arguments - i.e. the first stack slot.
typedef uint64_t *(__attribute__((thiscall)) * GetMemberByIndexFn)(
    void *self, uint64_t *ret, uint64_t lobby, int index
);
typedef uint64_t *(__attribute__((thiscall)) * GetOwnerFn)(
    void *self, uint64_t *ret, uint64_t lobby
);
typedef const char *(__attribute__((thiscall)) * GetLobbyDataFn)(
    void *self, uint64_t lobby, const char *key
);
typedef bool(__attribute__((thiscall)) * SetLobbyDataFn)(
    void *self, uint64_t lobby, const char *key, const char *value
);
typedef const char *(__attribute__((thiscall)) * GetMemberDataFn)(
    void *self, uint64_t lobby, uint64_t member, const char *key
);
typedef void(__attribute__((thiscall)) * SetMemberDataFn)(
    void *self, uint64_t lobby, const char *key, const char *value
);

static uintptr_t lobbyObject() { return (uintptr_t)GetModuleHandleA(NULL) + ONLINE_LOBBY_RVA; }

static void **matchmaking() {
    static SteamMatchmakingFn s_fn = nullptr;
    static bool s_resolved = false;

    if (!s_resolved) {
        HMODULE steam = GetModuleHandleA("steam_api.dll");

        s_resolved = true;

        if (steam) {
            s_fn = (SteamMatchmakingFn)(void *)GetProcAddress(steam, "SteamMatchmaking");
        }
    }

    if (!s_fn) {
        return nullptr;
    }

    return (void **)s_fn();
}

static void *onlineInterface() {
    return *(void **)((uintptr_t)GetModuleHandleA(NULL) + ONLINE_IFACE_IAT_RVA);
}

static FARPROC onlineExport(const char *name) {
    HMODULE online = GetModuleHandleA("ffonlinelib.dll");

    if (!online) {
        return nullptr;
    }

    return GetProcAddress(online, name);
}

template <typename Fn> static Fn slot(void **iface, int index) {
    void **vtable = *(void ***)iface;

    return (Fn)vtable[index];
}

static void copyBounded(char *out, int cap, const char *value) {
    int n = 0;

    while (n < cap - 1 && value[n]) {
        out[n] = value[n];
        ++n;
    }

    out[n] = 0;
}

uint64_t localSteamId() {
    static GetLocalIdFn s_fn = (GetLocalIdFn)(void *)onlineExport(GET_LOCAL_ID_EXPORT);
    void *iface = onlineInterface();

    if (!s_fn || !iface) {
        return 0;
    }

    return s_fn(iface);
}

bool personaName(uint64_t member, wchar_t *out, int cap) {
    static GetPersonaFn s_fn = (GetPersonaFn)(void *)onlineExport(GET_PERSONA_EXPORT);
    void *iface = onlineInterface();
    OnlineIdArg id = {member, 1, 0};
    const char *name = nullptr;
    int n = 0;

    out[0] = 0;

    if (!s_fn || !iface || !member) {
        return false;
    }

    name = s_fn(iface, id);

    if (!name || !name[0]) {
        return false;
    }

    n = MultiByteToWideChar(CP_UTF8, 0, name, -1, out, cap);

    if (n <= 0) {
        out[0] = 0;
        return false;
    }

    out[cap - 1] = 0;

    return true;
}

bool lobbyActive() {
    uintptr_t lobby = lobbyObject();

    if (*(uint32_t *)(lobby + LOBBY_TYPE_OFF) != ONLINE_TYPE_STEAM) {
        return false;
    }

    if (*(uint32_t *)(lobby + LOBBY_ID_VALID_OFF) != 1) {
        return false;
    }

    return *(uint64_t *)(lobby + LOBBY_ID_OFF) != 0 && matchmaking() != nullptr;
}

uint64_t lobbyId() {
    if (!lobbyActive()) {
        return 0;
    }

    return *(uint64_t *)(lobbyObject() + LOBBY_ID_OFF);
}

bool lobbyIsLocalOwner() {
    if (!lobbyActive()) {
        return false;
    }

    return *(uint8_t *)(lobbyObject() + LOBBY_OWNER_OFF) != 0;
}

uint64_t lobbyOwner() {
    uint64_t lobby = lobbyId();
    uint64_t owner = 0;

    if (!lobby) {
        return 0;
    }

    void **sm = matchmaking();
    slot<GetOwnerFn>(sm, SM_GET_OWNER)(sm, &owner, lobby);

    return owner;
}

int lobbyMembers(uint64_t *out, int max) {
    uint64_t lobby = lobbyId();
    int count = 0;

    if (!lobby) {
        return 0;
    }

    void **sm = matchmaking();
    count = slot<GetNumMembersFn>(sm, SM_GET_NUM_MEMBERS)(sm, lobby);

    if (count < 0) {
        count = 0;
    }

    if (count > max) {
        count = max;
    }

    for (int i = 0; i < count; ++i) {
        out[i] = 0;
        slot<GetMemberByIndexFn>(sm, SM_GET_MEMBER_BY_INDEX)(sm, &out[i], lobby, i);
    }

    return count;
}

bool lobbyMemberData(uint64_t member, const char *key, char *out, int cap) {
    uint64_t lobby = lobbyId();
    const char *value = nullptr;

    out[0] = 0;

    if (!lobby || !member) {
        return false;
    }

    void **sm = matchmaking();
    value = slot<GetMemberDataFn>(sm, SM_GET_MEMBER_DATA)(sm, lobby, member, key);

    // Steam answers "" for a key that was never set.
    if (!value || !value[0]) {
        return false;
    }

    copyBounded(out, cap, value);

    return true;
}

void lobbySetMemberData(const char *key, const char *value) {
    uint64_t lobby = lobbyId();

    if (!lobby) {
        return;
    }

    void **sm = matchmaking();
    slot<SetMemberDataFn>(sm, SM_SET_MEMBER_DATA)(sm, lobby, key, value);
}

bool lobbyData(const char *key, char *out, int cap) {
    uint64_t lobby = lobbyId();
    const char *value = nullptr;

    out[0] = 0;

    if (!lobby) {
        return false;
    }

    void **sm = matchmaking();
    value = slot<GetLobbyDataFn>(sm, SM_GET_LOBBY_DATA)(sm, lobby, key);

    if (!value || !value[0]) {
        return false;
    }

    copyBounded(out, cap, value);

    return true;
}

void lobbySetData(const char *key, const char *value) {
    uint64_t lobby = lobbyId();

    if (!lobby || !lobbyIsLocalOwner()) {
        return;
    }

    void **sm = matchmaking();
    slot<SetLobbyDataFn>(sm, SM_SET_LOBBY_DATA)(sm, lobby, key, value);
}
