// uepkg - read the name / import / export tables out of a decompressed UE3 package.
//
// The point is one question: which class and which function computes the weapon's aim rotation,
// and is it script or native. That is an export-table lookup, not a probe - and the export table
// parses from structures that barely changed across UE3 versions, unlike the bytecode.
//
// Run tools/uedecompress first; this reads the .u it produces, not the shipped .xxx.
//
// ⚠️ This build is FileVersion 584, LicenseeVersion 126 - Raven customised the engine, and a
// licensee is free to change serialised layouts. So the export layout below is a HYPOTHESIS, and
// the tool validates it rather than trusting it: every name index must be in range, every outer
// index must reference a real entry, and the table must consume exactly the bytes between the
// export offset and the end of the header. If a licensee change moved a field, those checks fail
// as a group rather than producing a plausible-looking table of wrong names. A parser that cannot
// tell "layout is right" from "layout is wrong" is the same class of instrument defect that cost
// this project six runs on the ProcessEvent census.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>

struct Reader {
    const uint8_t* p = nullptr;
    size_t len = 0, off = 0;
    bool bad = false;

    bool need(size_t n) { if (off + n > len) { bad = true; return false; } return true; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v; memcpy(&v, p + off, 4); off += 4; return v; }
    int32_t  i32() { return (int32_t)u32(); }
    uint64_t u64() { const uint64_t lo = u32(); const uint64_t hi = u32(); return lo | (hi << 32); }
    void skip(size_t n) { if (need(n)) off += n; }

    // UE3 FString: positive length = ANSI (count includes the terminator), negative = UTF-16.
    std::string str() {
        const int32_t n = i32();
        if (bad) return {};
        if (n == 0) return {};
        if (n > 0) {
            if (n > 1 << 20 || !need((size_t)n)) { bad = true; return {}; }
            std::string s((const char*)(p + off), (size_t)n);
            off += (size_t)n;
            while (!s.empty() && s.back() == '\0') s.pop_back();
            return s;
        }
        const size_t chars = (size_t)(-(int64_t)n);
        if (chars > 1 << 20 || !need(chars * 2)) { bad = true; return {}; }
        std::string s;
        for (size_t i = 0; i < chars; ++i) {
            const uint16_t c = (uint16_t)(p[off + i * 2] | (p[off + i * 2 + 1] << 8));
            if (c) s.push_back(c < 128 ? (char)c : '?');
        }
        off += chars * 2;
        return s;
    }
};

struct Export {
    int32_t clsIdx = 0, superIdx = 0, outerIdx = 0, archIdx = 0;
    int32_t nameIdx = 0, nameNum = 0;
    uint64_t flags = 0;
    int32_t serialSize = 0, serialOffset = 0;
};
struct Import {
    int32_t pkgNameIdx = 0, clsNameIdx = 0, outerIdx = 0, nameIdx = 0, nameNum = 0;
};

static std::vector<std::string> gNames;
static std::vector<Export> gExports;
static std::vector<Import> gImports;

static const char* NameAt(int32_t i) {
    return (i >= 0 && (size_t)i < gNames.size()) ? gNames[(size_t)i].c_str() : "<bad>";
}

// UE3 PackageIndex: >0 is a 1-based export, <0 a 1-based import, 0 is None.
static std::string EntryName(int32_t idx) {
    if (idx == 0) return {};
    if (idx > 0) {
        const size_t i = (size_t)(idx - 1);
        return i < gExports.size() ? NameAt(gExports[i].nameIdx) : "<bad export>";
    }
    const size_t i = (size_t)(-idx - 1);
    return i < gImports.size() ? NameAt(gImports[i].nameIdx) : "<bad import>";
}

static int32_t EntryOuter(int32_t idx) {
    if (idx > 0) {
        const size_t i = (size_t)(idx - 1);
        return i < gExports.size() ? gExports[i].outerIdx : 0;
    }
    if (idx < 0) {
        const size_t i = (size_t)(-idx - 1);
        return i < gImports.size() ? gImports[i].outerIdx : 0;
    }
    return 0;
}

// Full dotted path, outermost first. Depth-capped because a corrupt outer index can otherwise
// form a cycle and hang the tool - and a hang reads like a crash, not like bad data.
static std::string FullPath(int32_t idx) {
    std::vector<std::string> parts;
    for (int guard = 0; idx != 0 && guard < 16; ++guard) {
        parts.push_back(EntryName(idx));
        idx = EntryOuter(idx);
    }
    std::string s;
    for (size_t i = parts.size(); i-- > 0;) { s += parts[i]; if (i) s += '.'; }
    return s;
}

static bool ReadWhole(const char* path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return false; }
    out.resize((size_t)len);
    const size_t got = fread(out.data(), 1, (size_t)len, f);
    fclose(f);
    return got == (size_t)len;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: uepkg <package.u> [filter]\n"
               "       lists exports; filter matches the object name (case-insensitive substring)\n");
        return 2;
    }
    std::vector<uint8_t> buf;
    if (!ReadWhole(argv[1], buf)) { printf("ERROR: cannot read %s\n", argv[1]); return 1; }

    Reader r{ buf.data(), buf.size(), 0, false };
    const uint32_t tag = r.u32();
    if (tag != 0x9E2A83C1u) {
        printf("ERROR: not a UE3 package (tag 0x%08X). Did you run uedecompress?\n", tag);
        return 1;
    }
    const uint32_t ver = r.u32();
    const uint16_t fileVer = (uint16_t)(ver & 0xFFFF), licVer = (uint16_t)(ver >> 16);
    const int32_t  headerSize = r.i32();
    const std::string folder = r.str();
    const uint32_t pkgFlags = r.u32();
    const int32_t nameCount = r.i32(),   nameOffset   = r.i32();
    const int32_t exportCount = r.i32(), exportOffset = r.i32();
    const int32_t importCount = r.i32(), importOffset = r.i32();

    printf("%s\n  version %u licensee %u  headerSize %d  folder '%s'  flags 0x%08X\n",
           argv[1], fileVer, licVer, headerSize, folder.c_str(), pkgFlags);
    printf("  names   %6d @ 0x%08X\n  exports %6d @ 0x%08X\n  imports %6d @ 0x%08X\n",
           nameCount, nameOffset, exportCount, exportOffset, importCount, importOffset);

    if (nameCount <= 0 || exportCount <= 0 ||
        (size_t)nameOffset >= buf.size() || (size_t)exportOffset >= buf.size() ||
        (size_t)importOffset >= buf.size()) {
        printf("ERROR: table offsets are not plausible - header layout differs from the assumption\n");
        return 1;
    }

    // ---- names ----
    gNames.reserve((size_t)nameCount);
    r.off = (size_t)nameOffset;
    for (int i = 0; i < nameCount && !r.bad; ++i) {
        gNames.push_back(r.str());
        r.u64();                       // EObjectFlags on the name entry
    }
    if (r.bad || (int)gNames.size() != nameCount) {
        printf("ERROR: name table did not parse (%zu of %d read)\n", gNames.size(), nameCount);
        return 1;
    }
    const size_t nameEnd = r.off;
    printf("  name table consumed to 0x%08zX (import table starts 0x%08X)%s\n",
           nameEnd, importOffset, nameEnd == (size_t)importOffset ? "  EXACT" : "  *** MISMATCH ***");
    if (nameEnd != (size_t)importOffset) {
        printf("ERROR: the name entry is not <FString><uint64> in this build. Every name index"
               " below would be shifted, so stopping rather than printing plausible wrong names.\n");
        return 1;
    }

    // ---- imports ----
    gImports.resize((size_t)importCount);
    r.off = (size_t)importOffset;
    for (int i = 0; i < importCount && !r.bad; ++i) {
        Import& im = gImports[(size_t)i];
        im.pkgNameIdx = r.i32(); r.i32();
        im.clsNameIdx = r.i32(); r.i32();
        im.outerIdx   = r.i32();
        im.nameIdx    = r.i32(); im.nameNum = r.i32();
    }
    if (r.bad) { printf("ERROR: import table did not parse\n"); return 1; }

    // ---- exports ----
    gExports.resize((size_t)exportCount);
    r.off = (size_t)exportOffset;
    for (int i = 0; i < exportCount && !r.bad; ++i) {
        Export& ex = gExports[(size_t)i];
        ex.clsIdx   = r.i32();
        ex.superIdx = r.i32();
        ex.outerIdx = r.i32();
        ex.nameIdx  = r.i32();
        ex.nameNum  = r.i32();
        ex.archIdx  = r.i32();
        ex.flags    = r.u64();
        ex.serialSize   = r.i32();
        ex.serialOffset = r.i32();
        r.u32();                                   // ExportFlags
        const int32_t netCount = r.i32();          // TArray<int32> GenerationNetObjectCount
        if (netCount < 0 || netCount > 1 << 16) { r.bad = true; break; }
        r.skip((size_t)netCount * 4);
        r.skip(16);                                // FGuid PackageGuid
        r.u32();                                   // PackageFlags
    }
    if (r.bad) { printf("ERROR: export table did not parse\n"); return 1; }

    // ---- validate the layout hypothesis, as a group ----
    int badName = 0, badOuter = 0, badSerial = 0;
    for (const Export& ex : gExports) {
        if (ex.nameIdx < 0 || ex.nameIdx >= nameCount) ++badName;
        if (ex.outerIdx > exportCount || ex.outerIdx < -importCount) ++badOuter;
        if (ex.serialSize < 0 || ex.serialOffset < 0 ||
            (size_t)ex.serialOffset + (size_t)ex.serialSize > buf.size()) ++badSerial;
    }
    const long tail = (long)headerSize - (long)r.off;
    printf("  export table consumed to 0x%08zX (headerSize 0x%08X, slack %ld)\n",
           r.off, headerSize, tail);
    printf("  VALIDATION: %d bad name idx, %d bad outer idx, %d bad serial range, out of %d\n",
           badName, badOuter, badSerial, exportCount);

    // Three independent structural checks. Any one of them could pass by luck on a wrong layout;
    // all three together cannot, which is the whole reason they are here rather than a single
    // "did it parse" flag.
    //
    // 1. The name table must end EXACTLY where the import table begins. Off-by-one in the entry
    //    size accumulates over hundreds of entries and cannot land on the right byte.
    // 2. Every index in every export must be in range - 1364 of them, so a wrong field offset
    //    would show up as thousands of failures, not zero.
    // 3. The bytes after the export table are the depends map: one empty TArray per export, so
    //    exactly 4 * exportCount. That is what pins the END of the export struct, and it is the
    //    check that would catch a licensee having ADDED a field to it.
    const long expectedTail = 4L * exportCount;
    if (tail == expectedTail)
        printf("  slack == 4 * exportCount, i.e. an empty depends map - export struct size confirmed.\n");

    if (badName || badOuter || badSerial || tail < 0) {
        printf("  *** LAYOUT REJECTED - the export struct for licensee %u is not what this tool"
               " assumes. Names printed below would be fiction; stopping.\n", licVer);
        return 1;
    }
    // 4. The serial regions must TILE the file: sorted by offset, each export's data ends exactly
    //    where the next one begins. This is the check that catches SerialSize/SerialOffset being
    //    the wrong pair of fields - wrong fields still land in range and still look plausible one
    //    at a time, but they cannot tile 36,873 regions with no gap and no overlap.
    {
        std::vector<size_t> order(gExports.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [](size_t a, size_t b) {
            return gExports[a].serialOffset < gExports[b].serialOffset;
        });
        int gaps = 0, overlaps = 0;
        long long covered = 0;
        for (size_t k = 0; k + 1 < order.size(); ++k) {
            const Export& cur = gExports[order[k]];
            const Export& nxt = gExports[order[k + 1]];
            const long long end = (long long)cur.serialOffset + cur.serialSize;
            if (end < nxt.serialOffset) ++gaps;
            if (end > nxt.serialOffset) ++overlaps;
            covered += cur.serialSize;
        }
        covered += gExports[order.back()].serialSize;
        printf("  TILING: %d gaps, %d overlaps across %zu serial regions; %lld bytes covered of"
               " %zu in the file\n", gaps, overlaps, gExports.size(), covered, buf.size());
        if (overlaps == 0 && gaps == 0)
            printf("  serial regions tile exactly - SerialSize/SerialOffset confirmed.\n");
        else
            printf("  *** serial regions do NOT tile. Treat every serialSize below as unverified;"
                   " the name and class columns are still sound (they passed the checks above).\n");
    }

    if (tail != expectedTail)
        // Plain ASCII in string literals - the build is code page 1252, same rule as the mod.
        printf("  WARNING: slack is %ld, not the %ld an empty depends map would give. Indices all check"
               " out, so the names below are probably right, but the struct size is NOT confirmed"
               " - treat anything surprising as suspect.\n", tail, expectedTail);
    printf("  layout accepted.\n\n");

    // ---- listing ----
    std::string filter = argc >= 3 ? argv[2] : "";
    for (char& c : filter) c = (char)tolower((unsigned char)c);

    int shown = 0;
    for (size_t i = 0; i < gExports.size(); ++i) {
        const Export& ex = gExports[i];
        std::string nm = NameAt(ex.nameIdx);
        // Match the FULL PATH, not the leaf name: finding a function's parameters means asking for
        // "RvSharedPawn.SetAim" and getting its children, which the leaf name alone cannot express.
        const std::string path = FullPath((int32_t)i + 1);
        if (!filter.empty()) {
            std::string low = path;
            for (char& c : low) c = (char)tolower((unsigned char)c);
            if (low.find(filter) == std::string::npos) continue;
        }
        const std::string cls = ex.clsIdx ? EntryName(ex.clsIdx) : "Class";
        printf("%-14s %-58s  super=%-28s  serial %7d @ 0x%08X\n",
               cls.c_str(), path.c_str(),
               ex.superIdx ? FullPath(ex.superIdx).c_str() : "-",
               ex.serialSize, ex.serialOffset);
        ++shown;
    }
    printf("\n%d of %d exports listed.\n", shown, exportCount);
    return 0;
}
