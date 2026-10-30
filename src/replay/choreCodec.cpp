#include "choreCodec.h"

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
static const uintptr_t IAT_MEMBUF_WRAP = 0x500588;  // MemoryBuffer(void*, uint)
static const uintptr_t IAT_MEMBUF_ALLOC = 0x5005ec; // MemoryBuffer(uint, bool)
static const uintptr_t IAT_MEMBUF_DTOR = 0x5005f0;
static const uintptr_t IAT_OUT_CTOR = 0x50057c;     // BufferOutputStream(MemoryBuffer&)
static const uintptr_t IAT_OUT_DTOR = 0x500578;
static const uintptr_t IAT_IN_CTOR = 0x500584;      // BufferInputStream(MemoryBuffer&)
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

typedef void *(__attribute__((thiscall)) *MemBufWrapFn)(void *self, void *data,
                                                        uint32_t length);
typedef void *(__attribute__((thiscall)) *MemBufAllocFn)(void *self,
                                                         uint32_t capacity,
                                                         uint32_t owns);
typedef void(__attribute__((thiscall)) *MemBufDtorFn)(void *self);
typedef void *(__attribute__((thiscall)) *StreamCtorFn)(void *self, void *buffer);
typedef void(__attribute__((thiscall)) *StreamDtorFn)(void *self);
typedef void *(__attribute__((thiscall)) *GetClassFn)(void *self);
typedef void *(__attribute__((thiscall)) *CreateSharedFn)(void *self, void *ret);
typedef void(__attribute__((thiscall)) *SerialFn)(void *self, void *stream);
typedef void(__attribute__((thiscall)) *DispatchFn)(void *world, void *event);
typedef void(__attribute__((thiscall)) *ReleaseHandleFn)(void *handle);

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

static bool readableRange(uintptr_t address, size_t length) {
    if (!address || !length || address + length < address) {
        return false;
    }
    uintptr_t cur = address;
    const uintptr_t end = address + length;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void *>(cur), &mbi,
                         sizeof(mbi)) != sizeof(mbi)) {
            return false;
        }
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
            (mbi.Protect & PAGE_NOACCESS)) {
            return false;
        }
        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cur) {
            return false;
        }
        cur = regionEnd;
    }
    return true;
}

static bool executableAddress(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!address || VirtualQuery(reinterpret_cast<const void *>(address), &mbi,
                                 sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
        (mbi.Protect & PAGE_NOACCESS)) {
        return false;
    }
    const DWORD protection = mbi.Protect & 0xff;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

static uint32_t fnv1a32(const void *data, size_t length) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 0x01000193u;
    }
    return hash;
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
    if (!readableRange(reinterpret_cast<uintptr_t>(text), length + 1)) {
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
    if (!readableRange(field, STD_STRING_CAPACITY_OFF + sizeof(uint32_t))) {
        return nullptr;
    }
    const uint32_t length = *reinterpret_cast<uint32_t *>(field + STD_STRING_SIZE_OFF);
    const uint32_t capacity =
        *reinterpret_cast<uint32_t *>(field + STD_STRING_CAPACITY_OFF);
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

static bool recoverNameOffset(void **classes, uint32_t count,
                              uint16_t *offsetOut) {
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
    rt->base = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
    if (!rt->base) {
        return false;
    }

    const uintptr_t iats[] = {IAT_MEMBUF_WRAP, IAT_MEMBUF_ALLOC, IAT_MEMBUF_DTOR,
                              IAT_OUT_CTOR,    IAT_OUT_DTOR,     IAT_IN_CTOR,
                              IAT_IN_DTOR};
    void *resolved[sizeof(iats) / sizeof(iats[0])] = {};
    for (size_t i = 0; i < sizeof(iats) / sizeof(iats[0]); ++i) {
        const uintptr_t slot = rt->base + iats[i];
        if (!readableRange(slot, sizeof(void *))) {
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
    if (!readableRange(mgr + NET_CLASS_BEGIN_OFF, 8)) {
        return false;
    }
    const uintptr_t begin =
        *reinterpret_cast<uintptr_t *>(mgr + NET_CLASS_BEGIN_OFF);
    const uintptr_t end =
        *reinterpret_cast<uintptr_t *>(mgr + NET_CLASS_END_OFF);
    if (!begin || end < begin || (end - begin) % sizeof(void *) != 0 ||
        end - begin > 0x4000 || !readableRange(begin, end - begin)) {
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
        if (!readableRange(reinterpret_cast<uintptr_t>(rt->classes[i]),
                           CLASS_OBJECT_SIZE_OFF + sizeof(uint32_t))) {
            return false;
        }
    }
    rt->nameOffsetProven = recoverNameOffset(rt->classes, rt->classCount,
                                             &rt->nameOffset);
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

static const char *classNameOf(const CodecRuntime *rt, uint32_t index,
                               size_t *lengthOut) {
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
    if (!readableRange(vtable, (CLASSIFIED_GET_CLASS_SLOT + 1) * sizeof(void *))) {
        return nullptr;
    }
    GetClassFn fn = *reinterpret_cast<GetClassFn *>(
        vtable + CLASSIFIED_GET_CLASS_SLOT * sizeof(void *));
    if (!executableAddress(reinterpret_cast<uintptr_t>(fn))) {
        return nullptr;
    }
    return fn(chore);
}

static bool choreSerialInto(void *chore, void *stream) {
    const uintptr_t serializable =
        reinterpret_cast<uintptr_t>(chore) + SERIALIZABLE_SUBOBJECT_OFF;
    if (!readableRange(serializable, sizeof(void *))) {
        return false;
    }
    const uintptr_t vtable = *reinterpret_cast<uintptr_t *>(serializable);
    if (!readableRange(vtable, (SERIALIZABLE_SERIAL_SLOT + 1) * sizeof(void *))) {
        return false;
    }
    SerialFn fn = *reinterpret_cast<SerialFn *>(
        vtable + SERIALIZABLE_SERIAL_SLOT * sizeof(void *));
    if (!executableAddress(reinterpret_cast<uintptr_t>(fn))) {
        return false;
    }
    fn(reinterpret_cast<void *>(serializable), stream);
    return true;
}

// Runs the stock send-side encoding: allocate a growing MemoryBuffer, wrap it
// in a BufferOutputStream, let the class write itself, then copy out the exact
// {data, length} pair the stock path would have handed to the transport.
static size_t serializeWith(CodecRuntime *rt, void *chore, void *out,
                            size_t cap) {
    alignas(8) uint8_t buffer[MEMBUF_STORAGE] = {};
    alignas(8) uint8_t stream[STREAM_STORAGE] = {};

    rt->memBufAlloc(buffer, SERIALIZE_CAPACITY, 1);
    rt->outCtor(stream, buffer);

    const bool ok = choreSerialInto(chore, stream);

    size_t written = 0;
    if (ok) {
        const uintptr_t data =
            *reinterpret_cast<uintptr_t *>(buffer + MEMBUF_DATA_OFF);
        const uint32_t length =
            *reinterpret_cast<uint32_t *>(buffer + MEMBUF_LENGTH_OFF);
        if (data && length && length <= cap && length <= SERIALIZE_CAPACITY &&
            readableRange(data, length)) {
            memcpy(out, reinterpret_cast<const void *>(data), length);
            written = length;
        }
    }

    rt->outDtor(stream);
    rt->memBufDtor(buffer);
    return written;
}

// `heldOut` receives the Ptr<Classified> storage the factory filled in. A caller
// that admits the object must release that Ptr once World has taken its own
// reference; a caller that only inspects the object may leave it held.
static void *createInstanceOfInto(CodecRuntime *rt, uint32_t index,
                                  void **heldOut) {
    if (index >= rt->classCount || !heldOut) {
        return nullptr;
    }
    void *cls = rt->classes[index];
    const uintptr_t vtable = *reinterpret_cast<uintptr_t *>(cls);
    if (!readableRange(vtable, (CLASS_CREATE_SHARED_SLOT + 1) * sizeof(void *))) {
        return nullptr;
    }
    CreateSharedFn fn = *reinterpret_cast<CreateSharedFn *>(
        vtable + CLASS_CREATE_SHARED_SLOT * sizeof(void *));
    if (!executableAddress(reinterpret_cast<uintptr_t>(fn))) {
        return nullptr;
    }
    *heldOut = nullptr;
    void *returned = fn(cls, heldOut);
    // The stock receive path reads the object out of the returned Ptr rather
    // than trusting the local it passed in, so do the same.
    void *object = returned ? *reinterpret_cast<void **>(returned) : *heldOut;
    if (!object || !readableRange(reinterpret_cast<uintptr_t>(object), 0x10)) {
        return nullptr;
    }
    *heldOut = object;
    return object;
}

static void *createInstanceOf(CodecRuntime *rt, uint32_t index) {
    void *held = nullptr;
    return createInstanceOfInto(rt, index, &held);
}

static bool deserializeInto(CodecRuntime *rt, void *chore, const void *payload,
                            size_t length) {
    alignas(8) uint8_t buffer[MEMBUF_STORAGE] = {};
    alignas(8) uint8_t stream[STREAM_STORAGE] = {};

    rt->memBufWrap(buffer, const_cast<void *>(payload),
                   static_cast<uint32_t>(length));
    rt->inCtor(stream, buffer);
    const bool ok = choreSerialInto(chore, stream);
    rt->inDtor(stream);
    rt->memBufDtor(buffer);
    return ok;
}

bool choreCodecAdmit(uint32_t classIndex, const void *payload, size_t length,
                    void **admittedOut) {
    if (admittedOut) *admittedOut = nullptr;
    CodecRuntime rt;
    if (!payload || !length || length > 0x1000 || !resolveRuntime(&rt) ||
        classIndex >= rt.classCount) {
        return false;
    }

    void *world = reinterpret_cast<void *>(rt.base + WORLD_RVA);
    if (!readableRange(reinterpret_cast<uintptr_t>(world), sizeof(void *))) {
        return false;
    }
    const uintptr_t worldVtable = *reinterpret_cast<uintptr_t *>(world);
    if (!readableRange(worldVtable + WORLD_DISPATCH_VTBL_OFF, sizeof(void *))) {
        return false;
    }
    DispatchFn dispatch = *reinterpret_cast<DispatchFn *>(
        worldVtable + WORLD_DISPATCH_VTBL_OFF);
    ReleaseHandleFn release =
        reinterpret_cast<ReleaseHandleFn>(rt.base + RELEASE_HANDLE_RVA);
    if (!executableAddress(reinterpret_cast<uintptr_t>(dispatch)) ||
        !executableAddress(reinterpret_cast<uintptr_t>(release))) {
        return false;
    }

    // Lifetime, and the reason this deliberately leaks one reference.
    //
    // createSharedInstance hands back a Ptr owning one reference.
    // World::dispatch adds and drops its own reference around the call, so it
    // is balanced. Releasing our reference after dispatch therefore drops the
    // object to zero and destroys it. That was tried once and wedged
    // the simulation thread: the admit returned OK, then the simulation stopped
    // advancing while the process still pumped messages. The queued command does not
    // keep the object alive on its own.
    //
    // So this matches the discipline the Lord routes already proved: hand the
    // command to World and keep one outstanding reference forever. It leaks a
    // Chore-sized allocation per admitted command, which is acceptable for a
    // recorder-driven replay and is tracked as a known cost rather than hidden.
    void *held = nullptr;
    void *object = createInstanceOfInto(&rt, classIndex, &held);
    if (!object) {
        return false;
    }
    (void)release;
    if (!deserializeInto(&rt, object, payload, length)) {
        return false;
    }
    if (admittedOut) *admittedOut = object;
    dispatch(world, object);
    return true;
}

bool choreCodecReady() {
    CodecRuntime rt;
    return resolveRuntime(&rt) && rt.nameOffsetProven;
}

uint32_t choreCodecClassCount() {
    CodecRuntime rt;
    return resolveRuntime(&rt) ? rt.classCount : 0;
}

bool choreCodecIsLobbyClass(uint32_t classIndex) {
    CodecRuntime rt;
    if (!resolveRuntime(&rt) || !rt.nameOffsetProven ||
        classIndex >= rt.classCount) {
        return false;
    }
    const char *name = classNameOf(&rt, classIndex, nullptr);
    if (!name) {
        return false;
    }
    return strcmp(name, "StartGameChore") == 0 ||
           strcmp(name, "UpdateGameDescriptionChore") == 0;
}

uint32_t choreCodecClassIndexByName(const char *name) {
    CodecRuntime rt;
    if (!name || !resolveRuntime(&rt) || !rt.nameOffsetProven) {
        return 0xffffffffu;
    }
    for (uint32_t i = 0; i < rt.classCount; ++i) {
        const char *candidate = classNameOf(&rt, i, nullptr);
        if (candidate && strcmp(candidate, name) == 0) {
            return i;
        }
    }
    return 0xffffffffu;
}

size_t choreCodecSerialize(void *chore, void *out, size_t cap,
                           uint32_t *classIndexOut) {
    if (classIndexOut) {
        *classIndexOut = 0xffffffffu;
    }
    CodecRuntime rt;
    if (!chore || !out || !cap || !resolveRuntime(&rt) ||
        !readableRange(reinterpret_cast<uintptr_t>(chore), 0x10)) {
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

size_t choreCodecClassSnapshot(void *out, size_t cap, uint32_t first) {
    if (!out || cap < sizeof(ChoreClassSnapshotHeader)) {
        return 0;
    }
    CodecRuntime rt;
    const bool resolved = resolveRuntime(&rt);

    ChoreClassSnapshotHeader header = {};
    header.magic = 0x31534c43u; // "CLS1"
    header.version = 1;
    header.status = (resolved ? 1u : 0u) |
                    (resolved && rt.nameOffsetProven ? 2u : 0u);
    if (!resolved) {
        memcpy(out, &header, sizeof(header));
        return sizeof(header);
    }
    header.nameOffset = rt.nameOffset;
    header.total = rt.classCount;
    header.first = static_cast<uint16_t>(first);

    uint8_t *cursor = static_cast<uint8_t *>(out) + sizeof(header);
    size_t remaining = cap - sizeof(header);
    uint16_t count = 0;
    for (uint32_t i = first;
         i < rt.classCount && remaining >= sizeof(ChoreClassRecord); ++i) {
        ChoreClassRecord record = {};
        record.index = i;
        const uintptr_t cls = reinterpret_cast<uintptr_t>(rt.classes[i]);
        record.classRva = cls >= rt.base ? static_cast<uint32_t>(cls - rt.base)
                                         : 0xffffffffu;
        record.objectSize =
            *reinterpret_cast<uint32_t *>(cls + CLASS_OBJECT_SIZE_OFF);
        size_t length = 0;
        const char *name = classNameOf(&rt, i, &length);
        if (name) {
            if (length >= sizeof(record.name)) {
                length = sizeof(record.name) - 1;
            }
            memcpy(record.name, name, length);
            record.nameHash = fnv1a32(name, length);
        }
        memcpy(cursor, &record, sizeof(record));
        cursor += sizeof(record);
        remaining -= sizeof(record);
        ++count;
    }
    header.count = count;
    memcpy(out, &header, sizeof(header));
    return sizeof(header) + count * sizeof(ChoreClassRecord);
}

size_t choreCodecRoundTrip(const char *className, uint32_t probe, void *out,
                           size_t cap) {
    if (!out || cap < sizeof(ChoreRoundTripHeader)) {
        return 0;
    }
    ChoreRoundTripHeader header = {};
    header.magic = 0x31505452u; // "RTP1"
    header.version = 1;
    header.classIndex = 0xffffu;

    CodecRuntime rt;
    if (!className || !resolveRuntime(&rt) || !rt.nameOffsetProven) {
        memcpy(out, &header, sizeof(header));
        return sizeof(header);
    }

    uint32_t index = 0xffffffffu;
    for (uint32_t i = 0; i < rt.classCount; ++i) {
        const char *candidate = classNameOf(&rt, i, nullptr);
        if (candidate && strcmp(candidate, className) == 0) {
            index = i;
            break;
        }
    }
    if (index == 0xffffffffu) {
        memcpy(out, &header, sizeof(header));
        return sizeof(header);
    }
    header.classIndex = static_cast<uint16_t>(index);

    // Both objects are built through the stock factory and are never admitted,
    // so this cannot enter the simulation even if the codec misbehaves.
    void *source = createInstanceOf(&rt, index);
    void *rebuilt = createInstanceOf(&rt, index);
    if (!source || !rebuilt) {
        memcpy(out, &header, sizeof(header));
        return sizeof(header);
    }
    header.status |= 1;

    // Stamp a recognisable pattern over the data words only. The two vtable
    // words at +0x00 and +0x04 are left alone; overwriting them would destroy
    // the object rather than test the codec. The window is the instance size
    // the class itself reports, so this can never run past the allocation.
    const uint32_t declaredSize = *reinterpret_cast<uint32_t *>(
        reinterpret_cast<uintptr_t>(rt.classes[index]) + CLASS_OBJECT_SIZE_OFF);
    // The 0x100 ceiling bounds the STAMPING window below, not the codec. With
    // probe == 0 nothing is stamped, so applying it there silently excluded the
    // two largest classes -- StartGameChore (0x670) and
    // UpdateGameDescriptionChore (0x6ac) -- from every sweep ever run. They
    // returned status=0x1 "built, never serialized", which was then misread
    // as "these two do not round trip byte-identically". They were never
    // tested.
    const uint32_t sizeCap = probe ? 0x100u : 0x1000u;
    if (declaredSize < 0x0c || declaredSize > sizeCap || (declaredSize & 3) != 0) {
        memcpy(out, &header, sizeof(header));
        return sizeof(header);
    }
    const size_t probeBytes = declaredSize;
    header.objectSize = static_cast<uint32_t>(probeBytes);
    uint8_t *sourceBytes = static_cast<uint8_t *>(source);
    if (!readableRange(reinterpret_cast<uintptr_t>(source), probeBytes) ||
        !readableRange(reinterpret_cast<uintptr_t>(rebuilt), probeBytes)) {
        memcpy(out, &header, sizeof(header));
        return sizeof(header);
    }
    // probe == 0 deliberately leaves the freshly constructed object alone.
    // Classes that own vectors or strings must be swept in that state: stamping
    // a pattern over an owning field would hand `serial` a fabricated pointer.
    if (probe) {
        for (size_t offset = 8; offset + 4 <= probeBytes; offset += 4) {
            *reinterpret_cast<uint32_t *>(sourceBytes + offset) =
                probe + static_cast<uint32_t>(offset);
        }
    }

    // 4096 matches the recorder's payload buffer; a live StartGameChore payload
    // measured ~700 bytes with 10 slots, so 2048 was close enough to matter.
    uint8_t payload[4096] = {};
    const size_t length = serializeWith(&rt, source, payload, sizeof(payload));
    if (!length) {
        memcpy(out, &header, sizeof(header));
        return sizeof(header);
    }
    header.status |= 2;
    header.payloadLength = static_cast<uint32_t>(length);
    header.payloadHash = fnv1a32(payload, length);

    if (deserializeInto(&rt, rebuilt, payload, length)) {
        header.status |= 4;
    }

    uint8_t second[4096] = {};
    const size_t secondLength = serializeWith(&rt, rebuilt, second,
                                              sizeof(second));
    if (secondLength == length && memcmp(second, payload, length) == 0) {
        header.status |= 8;
    }

    header.sourceObjectHash = fnv1a32(sourceBytes + 8, probeBytes - 8);
    header.rebuiltObjectHash =
        fnv1a32(static_cast<uint8_t *>(rebuilt) + 8, probeBytes - 8);
    if (header.sourceObjectHash == header.rebuiltObjectHash) {
        header.status |= 16;
    }

    memcpy(out, &header, sizeof(header));
    const size_t used = sizeof(header);
    const size_t copy = length < cap - used ? length : cap - used;
    memcpy(static_cast<uint8_t *>(out) + used, payload, copy);
    return used + copy;
}
