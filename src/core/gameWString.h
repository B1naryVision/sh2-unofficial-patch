#pragma once
#include <cstdint>

// MSVC (VC7/VC8) std::wstring as the game lays it out: a 16-byte union holding
// either up to seven inline characters or a heap pointer, then the size and the
// capacity. The capacity decides which: below 8 the characters are inline.
// Never dereference `heap` without checking, because a short string's first two
// characters form a plausible-looking address.
struct GameWString {
    union {
        wchar_t *heap;
        wchar_t inl[8];
    };
    uint32_t size;
    uint32_t res;
};

static_assert(sizeof(GameWString) == 0x18, "MSVC std::wstring layout");

// The characters of `s`, or nullptr when its size and capacity disagree.
inline const wchar_t *gameWStringText(const GameWString *s) {
    if (!s || s->size > s->res) {
        return nullptr;
    }

    if (s->res >= 8) {
        return s->heap;
    }

    return s->inl;
}
