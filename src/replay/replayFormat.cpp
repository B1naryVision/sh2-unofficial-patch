#include "replayFormat.h"

#include <cstring>

uint32_t replayFnv32(uint32_t hash, const void *data, uint32_t bytes) {
    const uint8_t *p = (const uint8_t *)data;

    for (uint32_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }

    return hash;
}

uint32_t replayTag(const char *s) {
    return (uint32_t)(uint8_t)s[0] | (uint32_t)(uint8_t)s[1] << 8 | (uint32_t)(uint8_t)s[2] << 16 |
           (uint32_t)(uint8_t)s[3] << 24;
}

bool replayValidateBundle(const uint8_t *file, uint32_t bytes, ReplayBundleHeader *out) {
    memset(out, 0, sizeof(*out));

    if (!file || bytes < sizeof(ReplayBundleHeader)) {
        return false;
    }

    memcpy(out, file, sizeof(*out));
    const uint8_t *body = file + sizeof(ReplayBundleHeader);
    return memcmp(out->magic, "S2REPLAY", 8) == 0 && out->version == 1 &&
           out->headerSize == sizeof(ReplayBundleHeader) &&
           out->bodyBytes == bytes - sizeof(ReplayBundleHeader) &&
           replayFnv32(REPLAY_FNV32_SEED, body, out->bodyBytes) == out->bodyHash;
}

bool replayFindSection(
    const uint8_t *body, uint32_t bytes, uint32_t sections, uint32_t tag, const uint8_t **data,
    uint32_t *size
) {
    uint32_t at = 0;

    for (uint32_t i = 0; i < sections; ++i) {
        if (bytes - at < sizeof(ReplayBundleSection)) {
            return false;
        }

        ReplayBundleSection s;
        memcpy(&s, body + at, sizeof(s));
        at += sizeof(s);

        if (s.bytes > bytes - at) {
            return false;
        }

        if (s.tag == tag) {
            *data = body + at;
            *size = s.bytes;
            return true;
        }

        at += s.bytes;
    }

    return false;
}

bool replayValidateCommandLog(
    const void *data, uint32_t bytes, uint32_t classCount, uint32_t capacity,
    ReplayLogFileHeader *out
) {
    if (!data || bytes < sizeof(ReplayLogFileHeader)) {
        return false;
    }

    ReplayLogFileHeader h;
    memcpy(&h, data, sizeof(h));

    if (memcmp(h.magic, "SH2RCMD1", 8) != 0 || h.version != 2 ||
        h.recordSize != sizeof(ReplayEventRecord) || h.droppedEvents != 0 ||
        h.byteCount > capacity || bytes - sizeof(h) < h.byteCount || h.classCount != classCount) {
        return false;
    }

    const uint8_t *log = (const uint8_t *)data + sizeof(h);
    uint32_t at = 0;
    uint32_t events = 0;
    uint32_t lastTick = h.epochTick;

    while (at < h.byteCount) {
        if (h.byteCount - at < sizeof(ReplayEventRecord)) {
            return false;
        }

        ReplayEventRecord e;
        memcpy(&e, log + at, sizeof(e));
        at += sizeof(e);

        if (e.payloadLength > h.byteCount - at || e.simulationTick < lastTick ||
            (e.classIndex != 0xffffu && e.classIndex >= classCount)) {
            return false;
        }

        lastTick = e.simulationTick;
        at += e.payloadLength;
        ++events;
    }

    if (events != h.eventCount) {
        return false;
    }

    *out = h;
    return true;
}

static uint32_t loadWord(const uint8_t *words, uint32_t index) {
    uint32_t w = 0;
    memcpy(&w, words + index * 4, 4);
    return w;
}

uint32_t replayValidateTileOrder(const void *data, uint32_t bytes, uint32_t *lists) {
    uint32_t header[3] = {};

    if (!data || bytes < sizeof(header)) {
        return REPLAY_FORMAT_REFUSED;
    }

    memcpy(header, data, sizeof(header));
    bytes -= sizeof(header);

    if (header[0] != REPLAY_TILE_MAGIC || header[1] != 1 || bytes % 4 != 0 ||
        bytes / 4 > REPLAY_TILE_MAX_WORDS) {
        return REPLAY_FORMAT_REFUSED;
    }

    const uint8_t *body = (const uint8_t *)data + sizeof(header);
    const uint32_t words = bytes / 4;
    uint32_t at = 0;

    for (uint32_t list = 0; list < header[2]; ++list) {
        if (words - at < 2) {
            return REPLAY_FORMAT_REFUSED;
        }

        const uint32_t kind = loadWord(body, at);
        const uint32_t length = loadWord(body, at + 1);

        if (kind > 1 || length < 2 || length > REPLAY_TILE_MAX_MEMBERS || words - at - 2 < length) {
            return REPLAY_FORMAT_REFUSED;
        }

        at += 2 + length;
    }

    *lists = header[2];
    return at;
}

uint32_t replayValidateExecOrder(const void *data, uint32_t bytes, uint32_t *jobs) {
    uint32_t header[3] = {};

    if (!data || bytes < sizeof(header)) {
        return REPLAY_FORMAT_REFUSED;
    }

    memcpy(header, data, sizeof(header));
    bytes -= sizeof(header);

    if (header[0] != REPLAY_EXOR_MAGIC || header[1] != 1 || bytes % 4 != 0 ||
        bytes / 4 > REPLAY_EXOR_MAX_WORDS) {
        return REPLAY_FORMAT_REFUSED;
    }

    const uint8_t *body = (const uint8_t *)data + sizeof(header);
    const uint32_t words = bytes / 4;
    uint32_t at = 0;

    for (uint32_t job = 0; job < header[2]; ++job) {
        if (words - at < 2) {
            return REPLAY_FORMAT_REFUSED;
        }

        const uint32_t n = loadWord(body, at + 1);

        if (n == 0 || n > REPLAY_EXOR_MAX_ITEMS || words - at - 2 < n) {
            return REPLAY_FORMAT_REFUSED;
        }

        at += 2 + n;
    }

    *jobs = header[2];
    return at;
}

// {class vtable RVA, damage offset}: Arrow ctor 0xcea70 sets +0xd4 = 750.0,
// Axe 0xd4b20 +0xcc, Spear 0xd5bd0 +0xd0; none of the three serializes it.
static const uint32_t FLY_FIELDS[][2] = {{0x5a882c, 0xd4}, {0x5a8f34, 0xcc}, {0x5a8ff4, 0xd0}};

uint32_t replayFlyFieldOffset(uint32_t vtableRva) {
    for (const auto &f : FLY_FIELDS) {
        if (f[0] == vtableRva) {
            return f[1];
        }
    }

    return 0;
}

bool replayFlyFieldKnown(uint32_t vtableRva, uint32_t offset) {
    return offset && replayFlyFieldOffset(vtableRva) == offset;
}

uint32_t replayValidateFlyState(const void *data, uint32_t bytes) {
    uint32_t header[3] = {};

    if (!data || bytes < sizeof(header)) {
        return REPLAY_FORMAT_REFUSED;
    }

    memcpy(header, data, sizeof(header));

    if (header[0] != REPLAY_FLY_MAGIC || header[1] != 1 || header[2] > REPLAY_FLY_MAX ||
        bytes - sizeof(header) != header[2] * sizeof(ReplayFlyField)) {
        return REPLAY_FORMAT_REFUSED;
    }

    const uint8_t *entries = (const uint8_t *)data + sizeof(header);

    for (uint32_t i = 0; i < header[2]; ++i) {
        ReplayFlyField f;
        memcpy(&f, entries + i * sizeof(f), sizeof(f));

        if (!replayFlyFieldKnown(f.vtableRva, f.offset)) {
            return REPLAY_FORMAT_REFUSED;
        }
    }

    return header[2];
}

bool replayValidatePrng(
    const void *data, uint32_t bytes, uint32_t maxEntries, ReplayPrngLayout *out
) {
    memset(out, 0, sizeof(*out));
    ReplayPrngFileHeader h;

    if (!data || bytes < sizeof(h)) {
        return false;
    }

    memcpy(&h, data, sizeof(h));

    if (memcmp(h.magic, "SH2RPRN", 8) != 0 || h.version < 2 || h.version > REPLAY_PRNG_VERSION ||
        h.entrySize != sizeof(ReplayPrngEntry) || h.entryCount == 0 || h.entryCount > maxEntries ||
        h.droppedTicks != 0) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)data;
    const uint32_t gateBytes = h.version >= 3 ? h.entryCount : 0;
    const uint32_t gatesAt = sizeof(h) + h.entryCount * sizeof(ReplayPrngEntry);
    const uint32_t updatesAt = gatesAt + gateBytes;
    const uint32_t updateBytes = h.version >= 5 ? h.entryCount * sizeof(ReplayPrngUpdates) : 0;
    const uint32_t end = updatesAt + updateBytes;

    if (bytes < end) {
        return false;
    }

    const ReplayPrngEntry *entries = (const ReplayPrngEntry *)(p + sizeof(h));

    for (uint32_t i = 1; i < h.entryCount; ++i) {
        if (entries[i].relativeTick <= entries[i - 1].relativeTick) {
            return false;
        }
    }

    const ReplayPrngUpdates *updates =
        updateBytes ? (const ReplayPrngUpdates *)(p + updatesAt) : nullptr;

    for (uint32_t i = 0; updates && i < h.entryCount; ++i) {
        if (updates[i].updates == 0 || updates[i].updates > REPLAY_PRNG_UPDATES_MAX) {
            return false;
        }
    }

    const ReplayMonasteryClock *clocks = nullptr;
    uint32_t clockCount = 0;

    if (h.version >= 4) {
        uint32_t footer[2] = {};

        if (bytes - end < sizeof(footer)) {
            return false;
        }

        memcpy(footer, p + end, sizeof(footer));

        if (footer[0] != REPLAY_CLOCK_MAGIC || footer[1] > REPLAY_CLOCK_MAX ||
            bytes - end - sizeof(footer) != footer[1] * sizeof(ReplayMonasteryClock)) {
            return false;
        }

        clocks = (const ReplayMonasteryClock *)(p + end + sizeof(footer));
        clockCount = footer[1];

        // Duplicate or unsorted identities could write one clock twice.
        for (uint32_t i = 1; i < clockCount; ++i) {
            if (clocks[i - 1].slot >= clocks[i].slot) {
                return false;
            }
        }
    } else if (bytes != end) {
        return false;
    }

    out->header = h;
    out->entries = entries;
    out->gates = gateBytes ? p + gatesAt : nullptr;
    out->updates = updates;
    out->clocks = clocks;
    out->clockCount = clockCount;
    return true;
}
