// Host-side tests for the pure replay code: the retimer (replayRetime.cpp) and
// every sidecar parser (replayFormat.cpp). Built natively with AddressSanitizer
// and UBSan by `make test`, so an out-of-bounds read in a parser fails the run
// even when it would not crash.

#include "replay/replayFormat.h"
#include "replay/replayRetime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++s_checks;                                                                                \
                                                                                                   \
        if (!(cond)) {                                                                             \
            ++s_failures;                                                                          \
            std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

// ── Retimer fixtures ────────────────────────────────────────────────────────

static const uint32_t VT_HUMAN = 0x5c6fb4; // any vtable RVA; only equality matters
static const uint32_t VT_AI = 0x5c69b8;
static const uint8_t ORIGIN_UI = 1, ORIGIN_AI = 2, ORIGIN_REMOTE = 4;

struct Fixture {
    std::vector<uint8_t> log;
    std::vector<ReplayBoundaryEntry> witness;

    void admit(uint32_t tick, uint16_t order, uint32_t vtable, uint32_t token, uint8_t origin) {
        ReplayEventRecord e = {};
        e.simulationTick = tick;
        e.order = order;
        e.classIndex = 1;
        e.payloadLength = 4;
        e.vtableRva = vtable;
        e.originHint = origin;
        e.flags = REPLAY_FLAG_PAYLOAD;
        const uint8_t *p = (const uint8_t *)&e;
        log.insert(log.end(), p, p + sizeof(e));
        const uint8_t payload[4] = {1, 2, 3, 4};
        log.insert(log.end(), payload, payload + 4);
        boundary(1, tick, order, vtable, token);
    }

    void execute(uint32_t tick, uint16_t order, uint32_t vtable, uint32_t token) {
        boundary(2, tick, order, vtable, token);
    }

    void boundary(uint8_t phase, uint32_t tick, uint16_t order, uint32_t vtable, uint32_t token) {
        ReplayBoundaryEntry b = {};
        b.trace.phase = phase;
        b.trace.simulationTick = tick;
        b.trace.order = order;
        b.trace.vtableRva = vtable;
        b.eventToken = token;
        witness.push_back(b);
    }

    ReplayRetimeResult
    run(uint32_t epoch, uint32_t dropped = 0, uint32_t hooks = 0xf, uint32_t limit = 0) {
        ReplayRetimeResult r;
        replayRetime(
            log.data(), (uint32_t)log.size(), epoch, witness.data(), (uint32_t)witness.size(),
            dropped, hooks, limit, &r
        );
        return r;
    }
};

static void testRetimeDelays() {
    Fixture f;
    f.admit(10, 0, VT_HUMAN, 0x100, ORIGIN_REMOTE); // remote, same job (delay 0)
    f.execute(10, 0, VT_HUMAN, 0x100);
    f.admit(20, 0, VT_HUMAN, 0x200, ORIGIN_UI); // the default delay of one job
    f.execute(21, 0, VT_HUMAN, 0x200);
    f.admit(30, 0, VT_HUMAN, 0x300, ORIGIN_REMOTE); // a late burst, two jobs
    f.execute(32, 0, VT_HUMAN, 0x300);
    const ReplayRetimeResult r = f.run(5);
    CHECK(r.failReason == 0);
    CHECK(r.events == 3);
    CHECK(r.injected == 3);
    CHECK(r.retimed == 2);
    CHECK(r.unretimed == 0);
    CHECK(replayRetimeRefs()[0].newTick == 9);
    CHECK(replayRetimeRefs()[1].newTick == 20);
    CHECK(replayRetimeRefs()[2].newTick == 31);
}

static void testRetimeAutonomousUntouched() {
    Fixture f;
    f.admit(10, 0, VT_AI, 0x100, ORIGIN_AI);
    f.execute(13, 0, VT_AI, 0x100);
    const ReplayRetimeResult r = f.run(5);
    CHECK(r.events == 1);
    CHECK(r.injected == 0);
    CHECK(replayRetimeRefs()[0].newTick == 10);
}

static void testRetimeNeverExecuted() {
    Fixture f;
    f.admit(10, 0, VT_HUMAN, 0x100, ORIGIN_REMOTE); // dropped live on every peer
    f.admit(200, 0, VT_HUMAN, 0x200, ORIGIN_UI);
    f.execute(201, 0, VT_HUMAN, 0x200);
    const ReplayRetimeResult r = f.run(5);
    CHECK(r.failReason == 0);
    CHECK(r.unretimed == 1);
    CHECK(r.events == 1);
    CHECK(replayRetimeRefs()[0].newTick == 200);
}

static void testRetimeExecutedBeforeEpoch() {
    Fixture f;
    f.admit(5, 0, VT_HUMAN, 0x100, ORIGIN_UI);
    f.execute(5, 0, VT_HUMAN, 0x100); // job 4 would precede the checkpoint
    const ReplayRetimeResult r = f.run(5);
    CHECK(r.unretimed == 1);
    CHECK(r.events == 0);
}

// The case that disabled retiming for the rest of a match: more than 4096
// commands that never execute. They expire after IN_FLIGHT_JOBS instead.
static void testRetimeManyNeverExecuted() {
    Fixture f;

    for (uint32_t i = 0; i < 5000; ++i) {
        f.admit(10 + i, 0, VT_HUMAN, 0x1000 + i * 16, ORIGIN_REMOTE);
    }

    f.admit(6000, 0, VT_HUMAN, 0x900000, ORIGIN_UI);
    f.execute(6002, 0, VT_HUMAN, 0x900000);
    const ReplayRetimeResult r = f.run(5);
    CHECK(r.failReason == 0);
    CHECK(r.unretimed == 5000);
    CHECK(r.events == 1);
    CHECK(r.retimed == 1);
    CHECK(replayRetimeRefs()[0].newTick == 6001);
}

// A dropped command's address is reused by a later command once it is freed.
static void testRetimeTokenReuse() {
    Fixture expired;
    expired.admit(10, 0, VT_HUMAN, 0x100, ORIGIN_REMOTE);
    expired.admit(100, 0, VT_HUMAN, 0x100, ORIGIN_UI);
    expired.execute(101, 0, VT_HUMAN, 0x100);
    const ReplayRetimeResult a = expired.run(5);
    CHECK(a.failReason == 0);
    CHECK(a.unretimed == 1);
    CHECK(a.events == 1);
    CHECK(replayRetimeRefs()[0].newTick == 100);

    // Within the in-flight window the reuse is genuinely ambiguous.
    Fixture ambiguous;
    ambiguous.admit(10, 0, VT_HUMAN, 0x100, ORIGIN_REMOTE);
    ambiguous.admit(20, 0, VT_HUMAN, 0x100, ORIGIN_UI);
    ambiguous.execute(21, 0, VT_HUMAN, 0x100);
    const ReplayRetimeResult b = ambiguous.run(5);
    CHECK(b.failReason == 3);
    CHECK(b.events == 2);
    CHECK(b.planWords == 0);
}

static void testRetimeHooksMissing() {
    Fixture f;
    f.admit(10, 0, VT_HUMAN, 0x100, ORIGIN_UI);
    f.execute(12, 0, VT_HUMAN, 0x100);
    const ReplayRetimeResult r = f.run(5, 0, 0x7);
    CHECK(r.failReason == 1);
    CHECK(r.events == 1);
    CHECK(r.retimed == 0);
    CHECK(replayRetimeRefs()[0].newTick == 10);
}

static void testRetimeWitnessOverflow() {
    Fixture f;

    for (uint32_t t = 10; t <= 20; ++t) {
        f.admit(t, 0, VT_HUMAN, 0x100 + t * 16, ORIGIN_UI);
        f.execute(t + 1, 0, VT_HUMAN, 0x100 + t * 16);
    }

    // The witness stopped at its last entry (job 21): jobs up to 20 are known.
    const ReplayRetimeResult r = f.run(5, 1);
    CHECK(r.failReason == 2);
    CHECK(r.endTick == 20);
    CHECK(r.events == 10);
    CHECK(r.cutEvents == 1);
}

static void testRetimeLimit() {
    Fixture f;

    for (uint32_t t = 10; t < 20; ++t) {
        f.admit(t, 0, VT_AI, 0x100 + t * 16, ORIGIN_AI);
    }

    const ReplayRetimeResult r = f.run(5, 0, 0xf, 15);
    CHECK(r.events == 5);
    CHECK(r.cutEvents == 5);
    CHECK(r.endTick == 15);
}

static void testRetimeExecutionPlan() {
    Fixture f;
    f.admit(29, 0, VT_AI, 0x100, ORIGIN_AI);
    f.admit(29, 1, VT_HUMAN, 0x200, ORIGIN_UI);
    f.execute(30, 0, VT_HUMAN, 0x200); // the injected command ran first
    f.execute(30, 1, VT_AI, 0x100);
    const ReplayRetimeResult r = f.run(5);
    CHECK(r.failReason == 0);
    CHECK(r.planJobs == 1);
    CHECK(r.planWords == 7);
    const uint32_t *w = replayRetimePlanWords();
    CHECK(w[0] == REPLAY_EXOR_MAGIC && w[1] == 1 && w[2] == 1);
    CHECK(w[3] == 25 && w[4] == 2);
    CHECK(w[5] == (0x80000000u | VT_HUMAN));
    CHECK(w[6] == VT_AI);

    // The plan must survive its own validator.
    std::vector<uint8_t> image((const uint8_t *)w, (const uint8_t *)(w + r.planWords));
    uint32_t jobs = 0;
    CHECK(replayValidateExecOrder(image.data(), (uint32_t)image.size(), &jobs) == 4);
    CHECK(jobs == 1);
}

static void testRetimeEventCap() {
    Fixture f;

    for (uint32_t i = 0; i <= REPLAY_MAX_EVENTS; ++i) {
        ReplayEventRecord e = {};
        e.simulationTick = 10 + i / 100;
        e.order = (uint16_t)(i % 100);
        e.originHint = ORIGIN_AI;
        const uint8_t *p = (const uint8_t *)&e;
        f.log.insert(f.log.end(), p, p + sizeof(e));
    }

    const ReplayRetimeResult r = f.run(5, 0, 0);
    CHECK(r.failReason == 5);
    CHECK(r.endTick == 10 + REPLAY_MAX_EVENTS / 100 - 1);
    CHECK(r.events + r.cutEvents == REPLAY_MAX_EVENTS);
    CHECK(replayRetimeRefs()[r.events - 1].newTick < r.endTick);
}

// ── Parser fixtures ─────────────────────────────────────────────────────────

template <typename T> static void put(std::vector<uint8_t> &v, const T &value) {
    const uint8_t *p = (const uint8_t *)&value;
    v.insert(v.end(), p, p + sizeof(T));
}

static void putWord(std::vector<uint8_t> &v, uint32_t w) { put(v, w); }

static std::vector<uint8_t> commandLog(uint32_t epoch, uint32_t classCount) {
    std::vector<uint8_t> body;

    for (uint32_t i = 0; i < 3; ++i) {
        ReplayEventRecord e = {};
        e.simulationTick = epoch + i;
        e.classIndex = (uint16_t)(i < 2 ? i : 0xffff);
        e.payloadLength = (uint16_t)(i == 1 ? 0 : 3);
        put(body, e);
        body.insert(body.end(), e.payloadLength, 0x5a);
    }

    ReplayLogFileHeader h = {};
    memcpy(h.magic, "SH2RCMD1", 8);
    h.version = 2;
    h.recordSize = sizeof(ReplayEventRecord);
    h.eventCount = 3;
    h.byteCount = (uint32_t)body.size();
    h.classCount = classCount;
    h.epochTick = epoch;
    std::vector<uint8_t> image;
    put(image, h);
    image.insert(image.end(), body.begin(), body.end());
    return image;
}

static bool cmdsOk(const std::vector<uint8_t> &image, uint32_t classCount = 40) {
    ReplayLogFileHeader out;
    return replayValidateCommandLog(
        image.data(), (uint32_t)image.size(), classCount, REPLAY_LOG_CAPACITY, &out
    );
}

static void patchRecord(
    std::vector<uint8_t> &image, uint32_t index, uint32_t field, uint32_t value, uint32_t size
) {
    // Records 0 and 2 carry 3 payload bytes, record 1 none.
    uint32_t at = sizeof(ReplayLogFileHeader);

    for (uint32_t i = 0; i < index; ++i) {
        uint16_t length = 0;
        memcpy(&length, &image[at + offsetof(ReplayEventRecord, payloadLength)], 2);
        at += sizeof(ReplayEventRecord) + length;
    }

    memcpy(&image[at + field], &value, size);
}

static void testCommandLog() {
    const std::vector<uint8_t> good = commandLog(100, 40);
    CHECK(cmdsOk(good));
    CHECK(!cmdsOk(good, 41)); // another class table

    std::vector<uint8_t> v = good;
    v[0] = 'X';
    CHECK(!cmdsOk(v));

    v = good;
    v.resize(v.size() - 1); // payload runs past the section
    CHECK(!cmdsOk(v));

    v = good;
    patchRecord(v, 2, offsetof(ReplayEventRecord, payloadLength), 0x7fff, 2);
    CHECK(!cmdsOk(v));

    v = good;
    patchRecord(v, 1, offsetof(ReplayEventRecord, simulationTick), 99, 4); // before the epoch
    CHECK(!cmdsOk(v));

    v = good;
    patchRecord(v, 2, offsetof(ReplayEventRecord, simulationTick), 100, 4); // goes backwards
    CHECK(!cmdsOk(v));

    v = good;
    patchRecord(v, 0, offsetof(ReplayEventRecord, classIndex), 40, 2);
    CHECK(!cmdsOk(v));

    v = good;
    ReplayLogFileHeader h;
    memcpy(&h, v.data(), sizeof(h));
    h.eventCount = 4;
    memcpy(v.data(), &h, sizeof(h));
    CHECK(!cmdsOk(v));

    v = good;
    memcpy(&h, v.data(), sizeof(h));
    h.droppedEvents = 1;
    memcpy(v.data(), &h, sizeof(h));
    CHECK(!cmdsOk(v));

    v = good;
    memcpy(&h, v.data(), sizeof(h));
    h.byteCount = REPLAY_LOG_CAPACITY + 1;
    memcpy(v.data(), &h, sizeof(h));
    CHECK(!cmdsOk(v));

    CHECK(!cmdsOk(std::vector<uint8_t>(10, 0)));
}

static std::vector<uint8_t>
bundle(const std::vector<std::vector<uint8_t>> &sections, const char *const *tags) {
    std::vector<uint8_t> body;

    for (size_t i = 0; i < sections.size(); ++i) {
        const ReplayBundleSection s = {replayTag(tags[i]), (uint32_t)sections[i].size()};
        put(body, s);
        body.insert(body.end(), sections[i].begin(), sections[i].end());
    }

    ReplayBundleHeader h = {};
    memcpy(h.magic, "S2REPLAY", 8);
    h.version = 1;
    h.headerSize = sizeof(h);
    h.sectionCount = (uint32_t)sections.size();
    h.bodyBytes = (uint32_t)body.size();
    h.bodyHash = replayFnv32(REPLAY_FNV32_SEED, body.data(), (uint32_t)body.size());
    std::vector<uint8_t> file;
    put(file, h);
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

static void testBundleAndSections() {
    static const char *const TAGS[] = {"CMDS", "TILE"};
    const std::vector<uint8_t> file = bundle({commandLog(5, 40), {1, 2, 3, 4}}, TAGS);
    ReplayBundleHeader h;
    CHECK(replayValidateBundle(file.data(), (uint32_t)file.size(), &h));
    const uint8_t *body = file.data() + sizeof(h);
    const uint8_t *data = nullptr;
    uint32_t size = 0;
    CHECK(replayFindSection(body, h.bodyBytes, h.sectionCount, replayTag("TILE"), &data, &size));
    CHECK(size == 4 && data[0] == 1);
    CHECK(!replayFindSection(body, h.bodyBytes, h.sectionCount, replayTag("PRNG"), &data, &size));
    // A section table that claims more sections than the body holds.
    CHECK(!replayFindSection(body, h.bodyBytes, 3, replayTag("PRNG"), &data, &size));

    std::vector<uint8_t> v = file;
    v.back() ^= 1; // body hash
    CHECK(!replayValidateBundle(v.data(), (uint32_t)v.size(), &h));
    v = file;
    v.push_back(0); // trailing bytes
    CHECK(!replayValidateBundle(v.data(), (uint32_t)v.size(), &h));
    CHECK(!replayValidateBundle(file.data(), 100, &h));

    // A section whose length runs past the body.
    v = file;
    uint32_t huge = 0x7fffffff;
    memcpy(&v[sizeof(ReplayBundleHeader) + 4], &huge, 4);
    memcpy(&h, v.data(), sizeof(h));
    CHECK(!replayFindSection(
        v.data() + sizeof(h), h.bodyBytes, h.sectionCount, replayTag("TILE"), &data, &size
    ));
}

static std::vector<uint8_t> tileOrder(std::vector<std::vector<uint32_t>> lists) {
    std::vector<uint8_t> v;
    putWord(v, REPLAY_TILE_MAGIC);
    putWord(v, 1);
    putWord(v, (uint32_t)lists.size());

    for (const auto &l : lists) {
        for (uint32_t w : l) {
            putWord(v, w);
        }
    }

    return v;
}

static void testTileOrder() {
    uint32_t lists = 0;
    std::vector<uint8_t> good = tileOrder({{0, 2, 7, 9}, {1, 3, 1, 2, 3}});
    CHECK(replayValidateTileOrder(good.data(), (uint32_t)good.size(), &lists) == 9);
    CHECK(lists == 2);
    std::vector<uint8_t> v = tileOrder({{2, 2, 7, 9}});
    CHECK(replayValidateTileOrder(v.data(), (uint32_t)v.size(), &lists) == REPLAY_FORMAT_REFUSED);
    v = tileOrder({{0, 1, 7}});
    CHECK(replayValidateTileOrder(v.data(), (uint32_t)v.size(), &lists) == REPLAY_FORMAT_REFUSED);
    v = tileOrder({{0, 65, 7}});
    CHECK(replayValidateTileOrder(v.data(), (uint32_t)v.size(), &lists) == REPLAY_FORMAT_REFUSED);
    v = tileOrder({{0, 4, 7, 9}}); // claims more members than follow
    CHECK(replayValidateTileOrder(v.data(), (uint32_t)v.size(), &lists) == REPLAY_FORMAT_REFUSED);
    v = good;
    v.resize(v.size() - 2);
    CHECK(replayValidateTileOrder(v.data(), (uint32_t)v.size(), &lists) == REPLAY_FORMAT_REFUSED);
    v = good;
    v[0] = 0;
    CHECK(replayValidateTileOrder(v.data(), (uint32_t)v.size(), &lists) == REPLAY_FORMAT_REFUSED);
}

static void testExecOrder() {
    uint32_t jobs = 0;
    std::vector<uint8_t> v;
    putWord(v, REPLAY_EXOR_MAGIC);
    putWord(v, 1);
    putWord(v, 1);
    putWord(v, 25);
    putWord(v, 0); // an empty plan
    CHECK(replayValidateExecOrder(v.data(), (uint32_t)v.size(), &jobs) == REPLAY_FORMAT_REFUSED);
    v.resize(16);
    putWord(v, 300); // more items than a drain holds
    CHECK(replayValidateExecOrder(v.data(), (uint32_t)v.size(), &jobs) == REPLAY_FORMAT_REFUSED);
    v.resize(16);
    putWord(v, 2);
    putWord(v, 0x80000001u); // one item of two
    CHECK(replayValidateExecOrder(v.data(), (uint32_t)v.size(), &jobs) == REPLAY_FORMAT_REFUSED);
    putWord(v, 2);
    CHECK(replayValidateExecOrder(v.data(), (uint32_t)v.size(), &jobs) == 4);
}

static std::vector<uint8_t> flyState(uint32_t count, uint32_t vtable, uint32_t offset) {
    std::vector<uint8_t> v;
    putWord(v, REPLAY_FLY_MAGIC);
    putWord(v, 1);
    putWord(v, count);

    for (uint32_t i = 0; i < count; ++i) {
        ReplayFlyField f = {(uint16_t)i, 7, vtable, offset, 0x453b8000u};
        put(v, f);
    }

    return v;
}

static void testFlyState() {
    std::vector<uint8_t> v = flyState(3, 0x5a882c, 0xd4); // Arrow damage
    CHECK(replayValidateFlyState(v.data(), (uint32_t)v.size()) == 3);
    v = flyState(1, 0x5a882c, 0xcc); // a field the recorder never captures
    CHECK(replayValidateFlyState(v.data(), (uint32_t)v.size()) == REPLAY_FORMAT_REFUSED);
    v = flyState(1, 0x123456, 0xd4);
    CHECK(replayValidateFlyState(v.data(), (uint32_t)v.size()) == REPLAY_FORMAT_REFUSED);
    v = flyState(2, 0x5a8ff4, 0xd0);
    v.pop_back();
    CHECK(replayValidateFlyState(v.data(), (uint32_t)v.size()) == REPLAY_FORMAT_REFUSED);
    v = flyState(0, 0, 0);
    CHECK(replayValidateFlyState(v.data(), (uint32_t)v.size()) == 0);
    memcpy(&v[8], &REPLAY_FLY_MAX, 4);
    v[8] += 1;
    CHECK(replayValidateFlyState(v.data(), (uint32_t)v.size()) == REPLAY_FORMAT_REFUSED);
}

static std::vector<uint8_t> prngImage(uint32_t version, uint32_t entries, uint32_t clocks) {
    ReplayPrngFileHeader h = {};
    memcpy(h.magic, "SH2RPRN", 8);
    h.version = version;
    h.entrySize = sizeof(ReplayPrngEntry);
    h.entryCount = entries;
    std::vector<uint8_t> v;
    put(v, h);

    for (uint32_t i = 0; i < entries; ++i) {
        ReplayPrngEntry e = {i * 2, (uint16_t)i, (uint16_t)(i + 1), 0, 0};
        put(v, e);
    }

    if (version >= 3) {
        v.insert(v.end(), entries, 1);
    }

    if (version >= 5) {
        for (uint32_t i = 0; i < entries; ++i) {
            ReplayPrngUpdates u = {5, {(uint16_t)(i + 10), (uint16_t)(i + 20), 0, 0, 0, 0, 0}};
            put(v, u);
        }
    }

    if (version >= 4) {
        putWord(v, REPLAY_CLOCK_MAGIC);
        putWord(v, clocks);

        for (uint32_t i = 0; i < clocks; ++i) {
            ReplayMonasteryClock c = {(uint16_t)(i * 3), 1, 77};
            put(v, c);
        }
    }

    return v;
}

static void testPrng() {
    ReplayPrngLayout layout;
    std::vector<uint8_t> v = prngImage(4, 10, 2);
    CHECK(replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));
    CHECK(layout.header.entryCount == 10 && layout.gates && layout.clockCount == 2);
    CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), 9, &layout)); // over capacity

    v = prngImage(2, 4, 0);
    CHECK(replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));
    CHECK(!layout.gates && layout.clockCount == 0);
    v.push_back(0); // v2 has nothing after the entries
    CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));

    v = prngImage(4, 10, 2);
    const uint32_t entryAt = sizeof(ReplayPrngFileHeader) + 3 * sizeof(ReplayPrngEntry);
    uint32_t tick = 2; // entry 3 back to entry 1's tick
    memcpy(&v[entryAt], &tick, 4);
    CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));

    v = prngImage(4, 10, 2);
    const uint32_t clockAt = (uint32_t)v.size() - 2 * sizeof(ReplayMonasteryClock);
    v[clockAt + sizeof(ReplayMonasteryClock)] = 0; // second clock's slot below the first
    v[clockAt + sizeof(ReplayMonasteryClock) + 1] = 0;
    CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));

    v = prngImage(4, 10, 2);
    v.pop_back();
    CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));

    v = prngImage(REPLAY_PRNG_VERSION + 1, 10, 0);
    CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));

    // v4 has no later-update cursors: playback restores once per job.
    v = prngImage(4, 10, 2);
    CHECK(replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));
    CHECK(!layout.updates);

    // v5: one ReplayPrngUpdates per entry between the gates and the clocks.
    v = prngImage(5, 10, 2);
    CHECK(replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));
    CHECK(layout.updates && layout.clockCount == 2 && layout.gates);
    CHECK(layout.updates[3].updates == 5 && layout.updates[3].startCursor[0] == 13);
    CHECK(layout.updates[3].startCursor[1] == 23);
    CHECK(layout.clocks[1].slot == 3);

    const uint32_t updatesAt = sizeof(ReplayPrngFileHeader) + 10 * (sizeof(ReplayPrngEntry) + 1);
    const uint16_t badCounts[] = {0, REPLAY_PRNG_UPDATES_MAX + 1};

    for (uint16_t bad : badCounts) {
        v = prngImage(5, 10, 2);
        memcpy(&v[updatesAt + 4 * sizeof(ReplayPrngUpdates)], &bad, 2);
        CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));
    }

    v = prngImage(5, 10, 0);
    v.resize(updatesAt + 9 * sizeof(ReplayPrngUpdates)); // the updates block cut short
    CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));

    v = prngImage(4, 10, 0);
    ReplayPrngFileHeader h;
    memcpy(&h, v.data(), sizeof(h));
    h.droppedTicks = 1;
    memcpy(v.data(), &h, sizeof(h));
    CHECK(!replayValidatePrng(v.data(), (uint32_t)v.size(), REPLAY_PRNG_CAPACITY, &layout));
}

// Every validator over mutations and truncations of a valid image. Under
// AddressSanitizer an out-of-bounds read aborts; otherwise this only proves
// nothing crashes or hangs. Each image is copied to an exact-size heap block so
// a read one byte past the end is caught.
static void fuzzValidators() {
    static const char *const TAGS[] = {"CMDS", "TILE", "EXOR", "FLYS", "PRNG", "PRNG"};
    std::vector<uint8_t> exor;
    putWord(exor, REPLAY_EXOR_MAGIC);
    putWord(exor, 1);
    putWord(exor, 1);
    putWord(exor, 3);
    putWord(exor, 1);
    putWord(exor, 9);
    const std::vector<std::vector<uint8_t>> seeds = {
        commandLog(5, 40),
        tileOrder({{0, 2, 7, 9}, {1, 2, 1, 2}}),
        exor,
        flyState(2, 0x5a8f34, 0xcc),
        prngImage(4, 6, 3),
        prngImage(5, 6, 3)
    };
    const std::vector<uint8_t> file = bundle(seeds, TAGS);
    uint32_t rng = 12345;

    for (int round = 0; round < 20000; ++round) {
        const size_t which = (size_t)round % (seeds.size() + 1);
        std::vector<uint8_t> v = which < seeds.size() ? seeds[which] : file;
        rng = rng * 1664525u + 1013904223u;
        const int edits = 1 + (int)(rng >> 28);

        for (int e = 0; e < edits && !v.empty(); ++e) {
            rng = rng * 1664525u + 1013904223u;
            v[(rng >> 8) % v.size()] = (uint8_t)(rng >> 3);
        }

        rng = rng * 1664525u + 1013904223u;

        if ((rng & 7) == 0 && !v.empty()) {
            v.resize((rng >> 8) % v.size());
        }

        const uint32_t n = (uint32_t)v.size();
        uint8_t *exact = (uint8_t *)malloc(n ? n : 1);
        memcpy(exact, v.data(), n);
        ReplayLogFileHeader lh;
        ReplayPrngLayout pl;
        ReplayBundleHeader bh;
        uint32_t count = 0;
        replayValidateCommandLog(exact, n, 40, REPLAY_LOG_CAPACITY, &lh);
        replayValidateTileOrder(exact, n, &count);
        replayValidateExecOrder(exact, n, &count);
        replayValidateFlyState(exact, n);
        replayValidatePrng(exact, n, REPLAY_PRNG_CAPACITY, &pl);

        if (n >= sizeof(ReplayBundleHeader)) {
            replayValidateBundle(exact, n, &bh);
            memcpy(&bh, exact, sizeof(bh));
            const uint8_t *data = nullptr;
            uint32_t size = 0;
            replayFindSection(
                exact + sizeof(bh), n - (uint32_t)sizeof(bh), bh.sectionCount & 0xff,
                replayTag("PRNG"), &data, &size
            );
        }

        free(exact);
    }
}

int main() {
    if (!replayRetimeAcquire()) {
        std::printf("retime storage unavailable\n");
        return 2;
    }

    testRetimeDelays();
    testRetimeAutonomousUntouched();
    testRetimeNeverExecuted();
    testRetimeExecutedBeforeEpoch();
    testRetimeManyNeverExecuted();
    testRetimeTokenReuse();
    testRetimeHooksMissing();
    testRetimeWitnessOverflow();
    testRetimeLimit();
    testRetimeExecutionPlan();
    testRetimeEventCap();
    testCommandLog();
    testBundleAndSections();
    testTileOrder();
    testExecOrder();
    testFlyState();
    testPrng();
    fuzzValidators();
    replayRetimeRelease();
    std::printf("%d checks, %d failed\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
