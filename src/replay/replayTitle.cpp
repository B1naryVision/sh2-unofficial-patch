#include "replayTitle.h"

#include <cwchar>

static bool fileNameChar(wchar_t c) {
    if (c < 0x20 || c == 0x7f) {
        return false;
    }

    return !wcschr(L"<>:\"/\\|?*", c);
}

static bool highSurrogate(wchar_t c) { return c >= 0xd800 && c <= 0xdbff; }

// `src` made safe for a file name and cut to `limit` characters, with leading
// and trailing spaces dropped and no surrogate pair split. Returns its length.
static size_t cleanLabel(const wchar_t *src, size_t limit, wchar_t *out) {
    while (*src == L' ') {
        ++src;
    }

    size_t n = 0;

    for (; src[n] && n < limit; ++n) {
        out[n] = fileNameChar(src[n]) ? src[n] : L'_';
    }

    if (n && src[n] && highSurrogate(out[n - 1])) {
        --n;
    }

    while (n && out[n - 1] == L' ') {
        --n;
    }

    out[n] = 0;
    return n;
}

static bool sharedName(const ReplayTitlePlayer *players, int count, int index) {
    for (int i = 0; i < count; ++i) {
        if (i != index && players[i].name && !wcscmp(players[i].name, players[index].name)) {
            return true;
        }
    }

    return false;
}

static bool append(wchar_t *out, size_t &len, size_t budget, const wchar_t *text, size_t n) {
    if (len + n > budget) {
        return false;
    }

    wmemcpy(out + len, text, n);
    len += n;
    return true;
}

static size_t
formatAt(const ReplayTitlePlayer *players, int count, size_t limit, wchar_t *out, size_t budget) {
    static const int MAX_LABEL = 64;
    size_t len = 0;
    bool ok = true;

    for (int i = 0; ok && i < count; ++i) {
        bool seen = false;

        for (int k = 0; k < i; ++k) {
            if (players[k].team == players[i].team) {
                seen = true;
            }
        }

        if (seen) {
            continue;
        }

        if (len) {
            ok = append(out, len, budget, L" vs ", 4);
        }

        bool first = true;

        for (int j = i; ok && j < count; ++j) {
            if (players[j].team != players[i].team) {
                continue;
            }

            wchar_t label[MAX_LABEL + 1];
            const wchar_t *name = players[j].name;
            size_t n = 0;

            if (name && *name && !sharedName(players, count, j)) {
                n = cleanLabel(name, limit < MAX_LABEL ? limit : MAX_LABEL, label);
            }

            if (!n) {
                const wchar_t *fallback = players[j].fallback;

                if (!fallback || !*fallback) {
                    fallback = L"Player";
                }

                n = cleanLabel(fallback, MAX_LABEL, label);
            }

            if (!first) {
                ok = append(out, len, budget, L"+", 1);
            }

            ok = ok && append(out, len, budget, label, n);
            first = false;
        }
    }

    if (!ok) {
        len = 0;
    }

    out[len] = 0;
    return len;
}

size_t replayFormatTitle(const ReplayTitlePlayer *players, int count, wchar_t *out, size_t budget) {
    static const size_t LIMITS[] = {10, 6, 3};

    out[0] = 0;

    if (!players || count <= 0) {
        return 0;
    }

    for (size_t limit : LIMITS) {
        const size_t len = formatAt(players, count, limit, out, budget);

        if (len) {
            return len;
        }
    }

    return 0;
}
