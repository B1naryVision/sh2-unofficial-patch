#include "choreCodec.h"

#include "../core/exeBase.h"
#include "../core/readableMemory.h"

#include <cstring>
#include <windows.h>

// Ghidra VAs use ImageBase 0x400000; everything below is an RVA.
static const uintptr_t NET_MGR_RVA = 0x17e6a30;
static const uintptr_t NET_CLASS_BEGIN_OFF = 0x5c;
static const uintptr_t NET_CLASS_END_OFF = 0x60;

// The World singleton and the dispatch slot the UI panels use to admit a
// command. Playback deliberately uses the same entry rather than the network
// receive path, so an injected command follows exactly the local route.
static const uintptr_t WORLD_RVA = 0xd781d0;
static const uintptr_t WORLD_DISPATCH_VTBL_OFF = 0x20;
// Ptr<Classified> destructor, already proven by the Lord control routes.
static const uintptr_t RELEASE_HANDLE_RVA = 0x19e9f0;

// dragonfly.dll imports the executable already carries. Resolving through the
// exe's own IAT keeps us on exactly the code the stock codec calls, and needs
// no module lookup of our own.
static const uintptr_t IAT_MEMBUF_WRAP = 0x500588; // MemoryBuffer(void*, uint)
static const uintptr_t IAT_MEMBUF_ALLOC = 0x5005ec; // MemoryBuffer(uint, bool)
static const uintptr_t IAT_MEMBUF_DTOR = 0x5005f0;
static const uintptr_t IAT_OUT_CTOR = 0x50057c; // BufferOutputStream(MemoryBuffer&)
static const uintptr_t IAT_OUT_DTOR = 0x500578;
static const uintptr_t IAT_IN_CTOR = 0x500584; // BufferInputStream(MemoryBuffer&)
static const uintptr_t IAT_IN_DTOR = 0x500580;

// Proven from the stock send path RVA 0x367050: the packet pointer is read
// from MemoryBuffer + 0x04 and its length from MemoryBuffer + 0x08.
static const size_t MEMBUF_DATA_OFF = 0x04;
static const size_t MEMBUF_LENGTH_OFF = 0x08;

// Stock stack frames use 0x18 bytes for MemoryBuffer and one small slot pair
// for either stream. Over-allocating costs nothing and removes the need to
// assume an exact size.
static const size_t MEMBUF_STORAGE = 0x40;
static const size_t STREAM_STORAGE = 0x40;

static const size_t SERIALIZE_CAPACITY = 0x1000; // same as the stock send path

// Class vtable slot 2 is createSharedInstance; it returns Ptr<Classified> by
// value, so it takes a hidden return-buffer pointer as its only stack argument.
static const size_t CLASS_CREATE_SHARED_SLOT = 2;
// Dragonfly::Class layout beyond the name: the instance size the factory
// allocates. Proven against ReturnLordChore's Class, which reports 0x10 and
// matches the object size recovered from its three stock allocators.
static const size_t CLASS_OBJECT_SIZE_OFF = 0x24;
// Classified vtable slot 1 is getClass(); Serializable vtable slot 1 is
// serial(Stream&).
static const size_t CLASSIFIED_GET_CLASS_SLOT = 1;
static const size_t SERIALIZABLE_SERIAL_SLOT = 1;
static const size_t SERIALIZABLE_SUBOBJECT_OFF = 4;

typedef void *(__attribute__((thiscall)) * MemBufWrapFn)(void *self, void *data, uint32_t length);
typedef void *(__attribute__((thiscall)) * MemBufAllocFn)(
    void *self, uint32_t capacity, uint32_t owns
);
typedef void(__attribute__((thiscall)) * MemBufDtorFn)(void *self);
typedef void *(__attribute__((thiscall)) * StreamCtorFn)(void *self, void *buffer);
typedef void(__attribute__((thiscall)) * StreamDtorFn)(void *self);
typedef void *(__attribute__((thiscall)) * GetClassFn)(void *self);
typedef void *(__attribute__((thiscall)) * CreateSharedFn)(void *self, void *ret);
typedef void(__attribute__((thiscall)) * SerialFn)(void *self, void *stream);
typedef void(__attribute__((thiscall)) * DispatchFn)(void *world, void *event);
typedef void(__attribute__((thiscall)) * ReleaseHandleFn)(void *handle);

struct CodecRuntime {
    uintptr_t base;
    void *netMgr;
    MemBufWrapFn memBufWrap;
    MemBufAllocFn memBufAlloc;
    MemBufDtorFn memBufDtor;
    StreamCtorFn outCtor;
    StreamDtorFn outDtor;
    StreamCtorFn inCtor;
    StreamDtorFn inDtor;
    uint32_t classCount;
    void **classes;
    uint16_t nameOffset;
    bool nameOffsetProven;
};

static bool executableAddress(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi;

    if (!address ||
        VirtualQuery(reinterpret_cast<const void *>(address), &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) {
        return false;
    }

    const DWORD protection = mbi.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

// A Dragonfly::Class stores its name as an inline MSVC 7.1 std::string, not as
// a char pointer: a 16-byte small-string union, then the length, then the
// capacity. Names of 15 characters or fewer live in the union; longer ones,
// which is most Chore names, live on the heap. Reading it wrongly would give
// plausible-looking garbage, so the offset is recovered rather than assumed and
// is only accepted when it decodes cleanly for every registered class.
static const size_t STD_STRING_SIZE_OFF = 0x10;
static const size_t STD_STRING_CAPACITY_OFF = 0x14;
static const size_t STD_STRING_SSO_CAPACITY = 15;

static bool identifierText(const char *text, size_t length) {
    if (!text || length < 3 || length >= 40) {
        return false;
    }

    if (!observerReadable(reinterpret_cast<uintptr_t>(text), length + 1)) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == ':';

        if (!ok) {
            return false;
        }
    }

    return text[length] == 0;
}

// Decodes the std::string at `field`. Returns the text and its length, or null.
static const char *stdStringAt(uintptr_t field, size_t *lengthOut) {
    if (!observerReadable(field, STD_STRING_CAPACITY_OFF + sizeof(uint32_t))) {
        return nullptr;
    }

    const uint32_t length = *reinterpret_cast<uint32_t *>(field + STD_STRING_SIZE_OFF);
    const uint32_t capacity = *reinterpret_cast<uint32_t *>(field + STD_STRING_CAPACITY_OFF);

    if (length == 0 || length > capacity || capacity > 0x1000) {
        return nullptr;
    }

    const char *text;

    if (capacity <= STD_STRING_SSO_CAPACITY) {
        text = reinterpret_cast<const char *>(field);
    } else {
        text = *reinterpret_cast<const char **>(field);
    }

    if (!identifierText(text, length)) {
        return nullptr;
    }

    if (lengthOut) {
        *lengthOut = length;
    }

    return text;
}

static bool recoverNameOffset(void **classes, uint32_t count, uint16_t *offsetOut) {
    for (uint16_t offset = 4; offset <= 0x20; offset += 4) {
        bool allGood = count != 0;

        for (uint32_t i = 0; i < count && allGood; ++i) {
            const uintptr_t cls = reinterpret_cast<uintptr_t>(classes[i]);

            if (!stdStringAt(cls + offset, nullptr)) {
                allGood = false;
            }
        }

        if (allGood) {
            *offsetOut = offset;
            return true;
        }
    }

    return false;
}

// Resolution walks the whole class table with VirtualQuery, which is far too
// expensive to repeat on every admission. It is cached after the first success,
// and only after the table is actually populated: resolving during startup
// would otherwise freeze an empty table into the cache.
static CodecRuntime s_cached;
static bool s_cachedValid = false;

static bool resolveRuntimeUncached(CodecRuntime *rt) {
    memset(rt, 0, sizeof(*rt));
    rt->base = exeBase();

    if (!rt->base) {
        return false;
    }

    const uintptr_t iats[] = {IAT_MEMBUF_WRAP, IAT_MEMBUF_ALLOC, IAT_MEMBUF_DTOR, IAT_OUT_CTOR,
                              IAT_OUT_DTOR,    IAT_IN_CTOR,      IAT_IN_DTOR};
    void *resolved[sizeof(iats) / sizeof(iats[0])] = {};

    for (size_t i = 0; i < sizeof(iats) / sizeof(iats[0]); ++i) {
        const uintptr_t slot = rt->base + iats[i];

        if (!observerReadable(slot, sizeof(void *))) {
            return false;
        }

        resolved[i] = *reinterpret_cast<void **>(slot);

        if (!executableAddress(reinterpret_cast<uintptr_t>(resolved[i]))) {
            return false;
        }
    }

    rt->memBufWrap = reinterpret_cast<MemBufWrapFn>(resolved[0]);
    rt->memBufAlloc = reinterpret_cast<MemBufAllocFn>(resolved[1]);
    rt->memBufDtor = reinterpret_cast<MemBufDtorFn>(resolved[2]);
    rt->outCtor = reinterpret_cast<StreamCtorFn>(resolved[3]);
    rt->outDtor = reinterpret_cast<StreamDtorFn>(resolved[4]);
    rt->inCtor = reinterpret_cast<StreamCtorFn>(resolved[5]);
    rt->inDtor = reinterpret_cast<StreamDtorFn>(resolved[6]);

    rt->netMgr = reinterpret_cast<void *>(rt->base + NET_MGR_RVA);
    const uintptr_t mgr = reinterpret_cast<uintptr_t>(rt->netMgr);

    if (!observerReadable(mgr + NET_CLASS_BEGIN_OFF, 8)) {
        return false;
    }

    const uintptr_t begin = *reinterpret_cast<uintptr_t *>(mgr + NET_CLASS_BEGIN_OFF);
    const uintptr_t end = *reinterpret_cast<uintptr_t *>(mgr + NET_CLASS_END_OFF);

    if (!begin || end < begin || (end - begin) % sizeof(void *) != 0 || end - begin > 0x4000 ||
        !observerReadable(begin, end - begin)) {
        return false;
    }

    rt->classes = reinterpret_cast<void **>(begin);
    rt->classCount = static_cast<uint32_t>((end - begin) / sizeof(void *));

    // The wire index is a single byte, so a table larger than 256 would make
    // the stock encoding ambiguous. Refuse rather than truncate.
    if (rt->classCount == 0 || rt->classCount > 256) {
        return false;
    }

    for (uint32_t i = 0; i < rt->classCount; ++i) {
        if (!observerReadable(
                reinterpret_cast<uintptr_t>(rt->classes[i]),
                CLASS_OBJECT_SIZE_OFF + sizeof(uint32_t)
            )) {
            return false;
        }
    }

    rt->nameOffsetProven = recoverNameOffset(rt->classes, rt->classCount, &rt->nameOffset);
    return true;
}

static bool resolveRuntime(CodecRuntime *rt) {
    if (s_cachedValid) {
        *rt = s_cached;
        return true;
    }

    if (!resolveRuntimeUncached(rt)) {
        return false;
    }

    if (rt->classCount != 0 && rt->nameOffsetProven) {
        s_cached = *rt;
        s_cachedValid = true;
    }

    return true;
}

static const char *classNameOf(const CodecRuntime *rt, uint32_t index, size_t *lengthOut) {
    if (!rt->nameOffsetProven || index >= rt->classCount) {
        return nullptr;
    }

    const uintptr_t cls = reinterpret_cast<uintptr_t>(rt->classes[index]);
    return stdStringAt(cls + rt->nameOffset, lengthOut);
}

static uint32_t indexOfClass(const CodecRuntime *rt, void *cls) {
    for (uint32_t i = 0; i < rt->classCount; ++i) {
        if (rt->classes[i] == cls) {
            return i;
        }
    }

    return 0xffffffffu;
}

static void *choreGetClass(void *chore) {
    const uintptr_t vtable = *reinterpret_cast<uintptr_t *>(chore);

    if (!observerReadable(vtable, (CLASSIFIED_GET_CLASS_SLOT + 1) * sizeof(void *))) {
        return nullptr;
    }

    GetClassFn fn =
        *reinterpret_cast<GetClassFn *>(vtable + CLASSIFIED_GET_CLASS_SLOT * sizeof(void *));

    if (!executableAddress(reinterpret_cast<uintptr_t>(fn))) {
        return nullptr;
    }

    return fn(chore);
}

static bool choreSerialInto(void *chore, void *stream) {
    const uintptr_t serializable = reinterpret_cast<uintptr_t>(chore) + SERIALIZABLE_SUBOBJECT_OFF;

    if (!observerReadable(serializable, sizeof(void *))) {
        return false;
    }

    const uintptr_t vtable = *reinterpret_cast<uintptr_t *>(serializable);

    if (!observerReadable(vtable, (SERIALIZABLE_SERIAL_SLOT + 1) * sizeof(void *))) {
        return false;
    }

    SerialFn fn = *reinterpret_cast<SerialFn *>(vtable + SERIALIZABLE_SERIAL_SLOT * sizeof(void *));

    if (!executableAddress(reinterpret_cast<uintptr_t>(fn))) {
        return false;
    }

    fn(reinterpret_cast<void *>(serializable), stream);
    return true;
}

// Runs the stock send-side encoding: allocate a growing MemoryBuffer, wrap it
// in a BufferOutputStream, let the class write itself, then copy out the exact
// {data, length} pair the stock path would have handed to the transport.
static size_t serializeWith(CodecRuntime *rt, void *chore, void *out, size_t cap) {
    alignas(8) uint8_t buffer[MEMBUF_STORAGE] = {};
    alignas(8) uint8_t stream[STREAM_STORAGE] = {};

    rt->memBufAlloc(buffer, SERIALIZE_CAPACITY, 1);
    rt->outCtor(stream, buffer);

    const bool ok = choreSerialInto(chore, stream);

    size_t written = 0;

    if (ok) {
        const uintptr_t data = *reinterpret_cast<uintptr_t *>(buffer + MEMBUF_DATA_OFF);
        const uint32_t length = *reinterpret_cast<uint32_t *>(buffer + MEMBUF_LENGTH_OFF);

        if (data && length && length <= cap && length <= SERIALIZE_CAPACITY &&
            observerReadable(data, length)) {
            memcpy(out, reinterpret_cast<const void *>(data), length);
            written = length;
        }
    }

    rt->outDtor(stream);
    rt->memBufDtor(buffer);
    return written;
}

// `heldOut` receives the Ptr<Classified> storage the factory filled in, which
// owns one reference to the object.
static void *createInstanceOfInto(CodecRuntime *rt, uint32_t index, void **heldOut) {
    if (index >= rt->classCount || !heldOut) {
        return nullptr;
    }

    void *cls = rt->classes[index];
    const uintptr_t vtable = *reinterpret_cast<uintptr_t *>(cls);

    if (!observerReadable(vtable, (CLASS_CREATE_SHARED_SLOT + 1) * sizeof(void *))) {
        return nullptr;
    }

    CreateSharedFn fn =
        *reinterpret_cast<CreateSharedFn *>(vtable + CLASS_CREATE_SHARED_SLOT * sizeof(void *));

    if (!executableAddress(reinterpret_cast<uintptr_t>(fn))) {
        return nullptr;
    }

    *heldOut = nullptr;
    void *returned = fn(cls, heldOut);
    // The stock receive path reads the object out of the returned Ptr rather
    // than trusting the local it passed in, so do the same.
    void *object = returned ? *reinterpret_cast<void **>(returned) : *heldOut;

    if (!object || !observerReadable(reinterpret_cast<uintptr_t>(object), 0x10)) {
        return nullptr;
    }

    *heldOut = object;
    return object;
}

static bool deserializeInto(CodecRuntime *rt, void *chore, const void *payload, size_t length) {
    alignas(8) uint8_t buffer[MEMBUF_STORAGE] = {};
    alignas(8) uint8_t stream[STREAM_STORAGE] = {};

    rt->memBufWrap(buffer, const_cast<void *>(payload), static_cast<uint32_t>(length));
    rt->inCtor(stream, buffer);
    const bool ok = choreSerialInto(chore, stream);
    rt->inDtor(stream);
    rt->memBufDtor(buffer);
    return ok;
}

bool choreCodecAdmit(uint32_t classIndex, const void *payload, size_t length, void **admittedOut) {
    if (admittedOut) {
        *admittedOut = nullptr;
    }

    CodecRuntime rt;

    if (!payload || !length || length > 0x1000 || !resolveRuntime(&rt) ||
        classIndex >= rt.classCount) {
        return false;
    }

    void *world = reinterpret_cast<void *>(rt.base + WORLD_RVA);

    if (!observerReadable(reinterpret_cast<uintptr_t>(world), sizeof(void *))) {
        return false;
    }

    const uintptr_t worldVtable = *reinterpret_cast<uintptr_t *>(world);

    if (!observerReadable(worldVtable + WORLD_DISPATCH_VTBL_OFF, sizeof(void *))) {
        return false;
    }

    DispatchFn dispatch = *reinterpret_cast<DispatchFn *>(worldVtable + WORLD_DISPATCH_VTBL_OFF);
    ReleaseHandleFn release = reinterpret_cast<ReleaseHandleFn>(rt.base + RELEASE_HANDLE_RVA);

    if (!executableAddress(reinterpret_cast<uintptr_t>(dispatch)) ||
        !executableAddress(reinterpret_cast<uintptr_t>(release))) {
        return false;
    }

    // Lifetime. createSharedInstance hands back a Ptr owning one reference.
    // World::dispatch (S2Game::post) takes its Ptr<Chore> argument BY VALUE and
    // drops that reference in its epilogue (VA 0x77f022..0x77f057, the contract
    // the admission hook's refusal path also relies on). Passing the raw object
    // pointer as that argument therefore hands our reference over: nothing is
    // released after dispatch, and nothing leaks. Releasing it again would be a
    // double release -- which is what wedged the simulation when an earlier
    // version tried it. Only a Ptr that is never handed over, because the class
    // refused the payload, is released here.
    void *held = nullptr;
    void *object = createInstanceOfInto(&rt, classIndex, &held);

    if (!object) {
        return false;
    }

    if (!deserializeInto(&rt, object, payload, length)) {
        release(&held);
        return false;
    }

    if (admittedOut) {
        *admittedOut = object;
    }

    dispatch(world, object);
    return true;
}

uint32_t choreCodecClassCount() {
    CodecRuntime rt;
    return resolveRuntime(&rt) ? rt.classCount : 0;
}

// Lobby and session machinery: never a player's command, so never injected.
static const char *const SESSION_CLASS_MARKS[] = {
    "StartGame", "UpdateGameDescription",   "GameReady",   "EndJob",
    "Peer",      "AcceptCommandLineInvite", "MapTransfer",
};

// 0 not decided yet, 1 injectable, 2 not. Decided once per class per process.
static uint8_t s_injectable[256];
static uint32_t s_injectableTable = 0;

bool choreCodecIsInjectable(uint32_t classIndex) {
    CodecRuntime rt;

    if (!resolveRuntime(&rt) || !rt.nameOffsetProven || classIndex >= rt.classCount) {
        return false;
    }

    if (s_injectableTable != rt.classCount) {
        memset(s_injectable, 0, sizeof(s_injectable));
        s_injectableTable = rt.classCount;
    }

    if (!s_injectable[classIndex]) {
        size_t length = 0;
        const char *name = classNameOf(&rt, classIndex, &length);
        bool ok = name && length > 5 && strcmp(name + length - 5, "Chore") == 0;

        for (const char *mark : SESSION_CLASS_MARKS) {
            ok = ok && strstr(name, mark) == nullptr;
        }

        s_injectable[classIndex] = ok ? 1 : 2;
    }

    return s_injectable[classIndex] == 1;
}

size_t choreCodecSerialize(void *chore, void *out, size_t cap, uint32_t *classIndexOut) {
    if (classIndexOut) {
        *classIndexOut = 0xffffffffu;
    }

    CodecRuntime rt;

    if (!chore || !out || !cap || !resolveRuntime(&rt) ||
        !observerReadable(reinterpret_cast<uintptr_t>(chore), 0x10)) {
        return 0;
    }

    void *cls = choreGetClass(chore);

    if (!cls) {
        return 0;
    }

    const uint32_t index = indexOfClass(&rt, cls);

    if (classIndexOut) {
        *classIndexOut = index;
    }

    // An unregistered class has no wire encoding at all. Report the failure
    // instead of emitting bytes the playback side could never route.
    if (index == 0xffffffffu) {
        return 0;
    }

    return serializeWith(&rt, chore, out, cap);
}
