// Console harness: exercises the real parse/rebuild/verify code headlessly, so
// the file handling can be regression-tested without clicking through dialogs.
// `unlock` then `lock` on the result must reproduce the input byte-for-byte.
// Not shipped.
#define S2M_TEST
#include "s2mEditable.cpp"

#include <stdio.h>

int wmain(int argc, wchar_t **argv) {
    if (argc >= 3 && wcscmp(argv[1], L"dest") == 0) {
        std::wstring maps = userMapsFolder();

        if (maps.empty()) {
            wprintf(L"FAIL no user maps folder\n");
            return 1;
        }

        wprintf(L"maps folder: %ls\n", maps.c_str());
        wprintf(L"copy goes to: %ls\n", editableCopyPath(argv[2], maps).c_str());
        return 0;
    }

    if (argc < 4) {
        wprintf(L"usage: test <unlock|lock> <in> <out>\n");
        wprintf(L"       test dest <in>\n");
        return 2;
    }

    bool lock = (wcscmp(argv[1], L"lock") == 0);
    std::vector<uint8_t> data;

    if (!readFile(argv[2], data)) {
        wprintf(L"FAIL read\n");
        return 1;
    }

    Header h;

    if (!parseHeader(data, h)) {
        wprintf(L"FAIL parse\n");
        return 1;
    }

    wprintf(L"strings=%u author=%d value=%ls bodyOff=%u\n", h.strCount, h.authorIndex,
            h.authorValue.empty() ? L"(none)" : h.authorValue.c_str(), (unsigned)h.bodyOff);

    if (lock == (h.authorIndex >= 0)) {
        wprintf(L"NOOP\n");
        return 0;
    }

    std::vector<uint8_t> out = rebuild(data, h, lock);

    if (!verify(out, data, h, lock)) {
        wprintf(L"FAIL verify\n");
        return 1;
    }

    if (!writeFile(argv[3], out)) {
        wprintf(L"FAIL write\n");
        return 1;
    }

    wprintf(L"OK %u -> %u bytes\n", (unsigned)data.size(), (unsigned)out.size());
    return 0;
}
