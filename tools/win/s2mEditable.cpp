#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <stdint.h>

#include <string>
#include <vector>

static const wchar_t *APP_TITLE = L"Stronghold 2 Map Unlocker";
static const char AUTHOR_KEY[] = "author";
static const wchar_t *DEFAULT_AUTHOR = L"Firefly";
// The smallest real body across 122 shipped, community and save files is
// 123421 bytes, so this only ever rejects a truncated or bogus file.
static const size_t MIN_BODY_BYTES = 16384;

struct Record {
    size_t off;
    size_t len;
};

struct Header {
    uint32_t strCount = 0;
    std::vector<Record> records;
    int authorIndex = -1;
    std::wstring authorValue;
    size_t intBlockStart = 0;
    size_t bodyOff = 0;
};

static void showError(const std::wstring &text) {
    MessageBoxW(NULL, text.c_str(), APP_TITLE, MB_ICONERROR | MB_OK);
}

static bool readU32(const std::vector<uint8_t> &d, size_t &off, uint32_t &out) {
    if (off + 4 > d.size()) {
        return false;
    }

    memcpy(&out, d.data() + off, 4);
    off += 4;
    return true;
}

// The header is a plain property block: u32 nStrings, then each as
// u32 keyLen / ascii key / u32 charCount / UTF-16LE value; then the same
// shape for ints with a u32 value. The compressed body follows immediately.
static bool parseHeader(const std::vector<uint8_t> &d, Header &h) {
    size_t off = 0;

    if (!readU32(d, off, h.strCount) || h.strCount > 64) {
        return false;
    }

    for (uint32_t i = 0; i < h.strCount; ++i) {
        size_t start = off;
        uint32_t keyLen = 0;

        if (!readU32(d, off, keyLen) || keyLen > 64 || off + keyLen > d.size()) {
            return false;
        }

        std::string key((const char *)d.data() + off, keyLen);
        off += keyLen;
        uint32_t chars = 0;

        if (!readU32(d, off, chars) || chars > 1024 || off + (size_t)chars * 2 > d.size()) {
            return false;
        }

        if (key == AUTHOR_KEY) {
            h.authorIndex = (int)i;
            h.authorValue.assign((const wchar_t *)(d.data() + off), chars);
        }

        off += (size_t)chars * 2;
        h.records.push_back({start, off - start});
    }

    h.intBlockStart = off;
    uint32_t intCount = 0;

    if (!readU32(d, off, intCount) || intCount > 64) {
        return false;
    }

    for (uint32_t i = 0; i < intCount; ++i) {
        uint32_t keyLen = 0;

        if (!readU32(d, off, keyLen) || keyLen > 64 || off + keyLen > d.size()) {
            return false;
        }

        off += keyLen;
        uint32_t value = 0;

        if (!readU32(d, off, value)) {
            return false;
        }
    }

    h.bodyOff = off;

    // The compressed body must begin exactly where the header ends.
    if (h.bodyOff + 2 > d.size() || d[h.bodyOff] != 0x78 || d[h.bodyOff + 1] != 0x9c) {
        return false;
    }

    if (d.size() - h.bodyOff < MIN_BODY_BYTES) {
        return false;
    }

    return true;
}

static bool readFile(const std::wstring &path, std::vector<uint8_t> &out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size;

    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > 0x8000000) {
        CloseHandle(f);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD got = 0;
    BOOL ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, NULL);
    CloseHandle(f);
    return ok && got == out.size();
}

static bool writeFile(const std::wstring &path, const std::vector<uint8_t> &data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &wrote, NULL);
    CloseHandle(f);

    if (!ok || wrote != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }

    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }

    return true;
}

static void appendU32(std::vector<uint8_t> &out, uint32_t v) {
    out.insert(out.end(), (uint8_t *)&v, (uint8_t *)&v + 4);
}

static std::vector<uint8_t> makeAuthorRecord(const std::wstring &name) {
    std::vector<uint8_t> r;
    appendU32(r, (uint32_t)strlen(AUTHOR_KEY));
    r.insert(r.end(), AUTHOR_KEY, AUTHOR_KEY + strlen(AUTHOR_KEY));
    appendU32(r, (uint32_t)name.size());
    r.insert(r.end(), (const uint8_t *)name.c_str(),
             (const uint8_t *)name.c_str() + name.size() * 2);
    return r;
}

// Rebuilds the file with the author record dropped (unlock) or inserted (lock).
// Untouched records are copied byte-for-byte, and the body is never rewritten.
static std::vector<uint8_t> rebuild(const std::vector<uint8_t> &d, const Header &h, bool lock) {
    std::vector<uint8_t> out;
    out.reserve(d.size() + 32);
    uint32_t count = h.strCount;

    if (lock) {
        count = (h.authorIndex >= 0) ? count : count + 1;
    } else {
        count -= 1;
    }

    appendU32(out, count);

    if (lock) {
        std::vector<uint8_t> rec = makeAuthorRecord(DEFAULT_AUTHOR);
        out.insert(out.end(), rec.begin(), rec.end());
    }

    for (size_t i = 0; i < h.records.size(); ++i) {
        if ((int)i == h.authorIndex) {
            continue;
        }

        const Record &r = h.records[i];
        out.insert(out.end(), d.begin() + r.off, d.begin() + r.off + r.len);
    }

    out.insert(out.end(), d.begin() + h.intBlockStart, d.end());
    return out;
}

// Never emit a file we cannot read back, and never let the body change.
static bool verify(const std::vector<uint8_t> &out, const std::vector<uint8_t> &src,
                   const Header &srcHdr, bool lock) {
    Header check;

    if (!parseHeader(out, check)) {
        return false;
    }

    if (lock != (check.authorIndex >= 0)) {
        return false;
    }

    if (out.size() - check.bodyOff != src.size() - srcHdr.bodyOff) {
        return false;
    }

    return memcmp(out.data() + check.bodyOff, src.data() + srcHdr.bodyOff,
                  out.size() - check.bodyOff) == 0;
}

static std::wstring userMapsFolder() {
    wchar_t docs[MAX_PATH] = {0};

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs))) {
        return L"";
    }

    std::wstring maps = std::wstring(docs) + L"\\Stronghold 2\\Maps";

    if (GetFileAttributesW(maps.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (SHCreateDirectoryExW(NULL, maps.c_str(), NULL) != ERROR_SUCCESS) {
            return L"";
        }
    }

    return maps;
}

static std::wstring baseName(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");

    if (slash == std::wstring::npos) {
        return path;
    }

    return path.substr(slash + 1);
}

static std::wstring pickFile() {
    static wchar_t buf[MAX_PATH * 8] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Stronghold 2 maps (*.s2m)\0*.s2m\0All files\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 8;
    ofn.lpstrTitle = L"Pick a map to make editable";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) {
        return L"";
    }

    return buf;
}

// The copy always lands in the user maps folder, whatever folder the source
// came from -- that is the only place the editor resolves a map name against.
static std::wstring editableCopyPath(const std::wstring &path, const std::wstring &mapsDir) {
    std::wstring stem = baseName(path);
    size_t dot = stem.find_last_of(L'.');
    std::wstring ext = L".s2m";

    if (dot != std::wstring::npos) {
        ext = stem.substr(dot);
        stem = stem.substr(0, dot);
    }

    return mapsDir + L"\\" + stem + L" editable" + ext;
}

static bool processOne(const std::wstring &path, const std::wstring &mapsDir,
                       std::wstring &report) {
    std::wstring name = baseName(path);
    std::vector<uint8_t> data;

    if (!readFile(path, data)) {
        report += name + L"\n    could not be read.\n\n";
        return false;
    }

    Header h;

    if (!parseHeader(data, h)) {
        report += name + L"\n    is not a Stronghold 2 map file.\n\n";
        return false;
    }

    bool lock = (h.authorIndex < 0);
    std::wstring dest = path;

    if (!lock) {
        dest = editableCopyPath(path, mapsDir);

        if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring q = baseName(dest) + L"\n\nalready exists. Replace it?";

            if (MessageBoxW(NULL, q.c_str(), APP_TITLE, MB_ICONQUESTION | MB_YESNO) != IDYES) {
                report += name + L"\n    skipped.\n\n";
                return false;
            }
        }
    } else {
        std::wstring q = name + L"\n\nis already editable. Make it read-only instead?";

        if (MessageBoxW(NULL, q.c_str(), APP_TITLE, MB_ICONQUESTION | MB_YESNO) != IDYES) {
            report += name + L"\n    skipped.\n\n";
            return false;
        }
    }

    std::vector<uint8_t> out = rebuild(data, h, lock);

    if (!verify(out, data, h, lock)) {
        report += name + L"\n    internal check failed, nothing written.\n\n";
        return false;
    }

    if (!writeFile(dest, out)) {
        report += name + L"\n    could not be written to:\n    " + dest + L"\n\n";
        return false;
    }

    if (lock) {
        report += name + L"\n    is now read-only.\n\n";
    } else {
        report += L"Created  " + baseName(dest) + L"\n    open it in the map editor.\n\n";
    }

    return true;
}

#ifndef S2M_TEST

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> files;

    for (int i = 1; i < argc; ++i) {
        files.push_back(argv[i]);
    }

    if (argv != NULL) {
        LocalFree(argv);
    }

    if (files.empty()) {
        std::wstring picked = pickFile();

        if (picked.empty()) {
            return 0;
        }

        files.push_back(picked);
    }

    std::wstring mapsDir = userMapsFolder();

    if (mapsDir.empty()) {
        showError(L"Could not find your Documents\\Stronghold 2\\Maps folder.\n\n"
                  L"Run Stronghold 2 once so it creates the folder, then try again.");
        return 1;
    }

    std::wstring report;
    int done = 0;

    for (size_t i = 0; i < files.size(); ++i) {
        if (processOne(files[i], mapsDir, report)) {
            ++done;
        }
    }

    if (report.empty()) {
        return 0;
    }

    report += L"Maps folder:\n" + mapsDir + L"\n\nOpen that folder now?";

    if (MessageBoxW(NULL, report.c_str(), APP_TITLE,
                    MB_ICONINFORMATION | MB_YESNO) == IDYES) {
        ShellExecuteW(NULL, L"open", mapsDir.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }

    return (done == (int)files.size()) ? 0 : 1;
}

#endif  // S2M_TEST
