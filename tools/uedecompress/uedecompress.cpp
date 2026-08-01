// uedecompress - unpack a UE3 "fully compressed" package (Singularity ships these as .xxx).
//
// Why this exists: RvGame\CookedPC\*.xxx are the game's .u SCRIPT packages, LZO-compressed
// whole-file. Core.xxx decompresses to 198,749 bytes, which is Core.u's size - that is what
// identifies them. Reading Raven's own UnrealScript is the difference between knowing which
// function computes the weapon's aim and probing for it one headset run at a time.
//
// No download and no new tooling: the container is 16 bytes plus a chunk table (verified against
// RvGame.xxx before this was written), and LZO1X decompression is short enough to carry here.
//
//     tag 0x9E2A83C1 | blockSize | compressedSize | uncompressedSize
//     FCompressedChunkInfo[ceil(uncompressedSize / blockSize)]  = { compressed, uncompressed }
//     ... raw compressed blocks, back to back ...
//
// The last chunk header ends exactly where the first block begins, which is what confirms the
// blocks are contiguous rather than each carrying a nested header.
//
// ⚠️ The self-check that matters is the SIZE. LZO is unforgiving - a decompressor with one wrong
// shift does not produce slightly-wrong output, it desynchronises within a few hundred bytes and
// either overruns or emits garbage. A chunk that lands on its declared uncompressed size to the
// byte, 214 times running, is not a coincidence. That is why every chunk is checked individually
// and a mismatch is fatal rather than a warning.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------- LZO1X decompression
//
// Structurally identical to minilzo's lzo1x_decompress, flattened so every label sits at function
// scope - jumping into the body of a nested loop is a C idiom that C++ compilers are entitled to
// refuse, and there is no reason to find out which way MSVC leans.
//
// Bounds are checked on every read and write. This parses a 13 MB file from disk; an out-of-range
// match distance should be a diagnosable error, not an access violation.
static bool Lzo1xDecompress(const uint8_t* in, size_t inLen,
                            uint8_t* out, size_t outCap, size_t* outUsed, const char** err) {
    const uint8_t* ip = in;
    const uint8_t* const ipEnd = in + inLen;
    uint8_t* op = out;
    uint8_t* const opEnd = out + outCap;
    const uint8_t* mPos = nullptr;
    size_t t = 0, n = 0;

#define FAIL(msg) do { *err = (msg); return false; } while (0)
#define NEED_IN(k)  do { if ((size_t)(ipEnd - ip) < (size_t)(k)) FAIL("input underrun");  } while (0)
#define NEED_OUT(k) do { if ((size_t)(opEnd - op) < (size_t)(k)) FAIL("output overrun");  } while (0)
#define NEED_MATCH  do { if (mPos < out || mPos >= op) FAIL("match distance out of range"); } while (0)

    NEED_IN(1);
    if (*ip > 17) {
        t = (size_t)(*ip++) - 17;
        if (t < 4) goto match_next;
        NEED_IN(t); NEED_OUT(t);
        do { *op++ = *ip++; } while (--t > 0);
        goto first_literal_run;
    }

top:
    NEED_IN(1);
    t = *ip++;
    if (t >= 16) goto do_match;
    if (t == 0) {
        NEED_IN(1);
        while (*ip == 0) { t += 255; ++ip; NEED_IN(1); }
        t += 15 + *ip++;
    }
    n = t + 3;
    NEED_IN(n); NEED_OUT(n);
    do { *op++ = *ip++; } while (--n > 0);

first_literal_run:
    NEED_IN(1);
    t = *ip++;
    if (t >= 16) goto do_match;
    NEED_IN(1);
    mPos = op - (1 + 0x0800);
    mPos -= t >> 2;
    mPos -= (size_t)(*ip++) << 2;
    NEED_MATCH; NEED_OUT(3);
    *op++ = *mPos++; *op++ = *mPos++; *op++ = *mPos++;
    goto match_done;

do_match:
    if (t >= 64) {
        NEED_IN(1);
        mPos = op - 1;
        mPos -= (t >> 2) & 7;
        mPos -= (size_t)(*ip++) << 3;
        t = (t >> 5) - 1;
    } else if (t >= 32) {
        t &= 31;
        if (t == 0) {
            NEED_IN(1);
            while (*ip == 0) { t += 255; ++ip; NEED_IN(1); }
            t += 31 + *ip++;
        }
        NEED_IN(2);
        mPos = op - 1;
        mPos -= (size_t)(ip[0] | (ip[1] << 8)) >> 2;
        ip += 2;
    } else if (t >= 16) {
        mPos = op;
        mPos -= (t & 8) << 11;
        t &= 7;
        if (t == 0) {
            NEED_IN(1);
            while (*ip == 0) { t += 255; ++ip; NEED_IN(1); }
            t += 7 + *ip++;
        }
        NEED_IN(2);
        mPos -= (size_t)(ip[0] | (ip[1] << 8)) >> 2;
        ip += 2;
        if (mPos == op) goto eof_found;   // the only legitimate way out of the stream
        mPos -= 0x4000;
    } else {
        NEED_IN(1);
        mPos = op - 1;
        mPos -= t >> 2;
        mPos -= (size_t)(*ip++) << 2;
        NEED_MATCH; NEED_OUT(2);
        *op++ = *mPos++; *op++ = *mPos++;
        goto match_done;
    }

    // copy_match: t + 2 bytes, byte at a time because source and destination overlap by design -
    // that overlap is how LZO encodes runs, so memcpy would be wrong here, not merely slower.
    NEED_MATCH; NEED_OUT(t + 2);
    *op++ = *mPos++; *op++ = *mPos++;
    do { *op++ = *mPos++; } while (--t > 0);

match_done:
    t = ip[-2] & 3;
    if (t == 0) goto top;

match_next:
    NEED_IN(t); NEED_OUT(t);
    do { *op++ = *ip++; } while (--t > 0);
    NEED_IN(1);
    t = *ip++;
    goto do_match;

eof_found:
    *outUsed = (size_t)(op - out);
    return true;

#undef NEED_MATCH
#undef NEED_OUT
#undef NEED_IN
#undef FAIL
}

// ---------------------------------------------------------------- container

static const uint32_t kPackageTag = 0x9E2A83C1u;

static uint32_t RdU32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool ReadFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return false; }
    out.resize((size_t)len);
    const size_t got = len ? fread(out.data(), 1, (size_t)len, f) : 0;
    fclose(f);
    return got == (size_t)len;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: uedecompress <in.xxx> [out.u]\n"
               "       decompresses a UE3 fully-compressed package.\n");
        return 2;
    }
    const char* inPath = argv[1];

    std::string outPath;
    if (argc >= 3) {
        outPath = argv[2];
    } else {
        outPath = inPath;
        const size_t dot = outPath.find_last_of('.');
        const size_t sep = outPath.find_last_of("\\/");
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
            outPath.resize(dot);
        outPath += ".u";
    }

    std::vector<uint8_t> src;
    if (!ReadFile(inPath, src)) { printf("ERROR: cannot read %s\n", inPath); return 1; }
    if (src.size() < 16)        { printf("ERROR: %s is too small to be a package\n", inPath); return 1; }

    const uint32_t tag   = RdU32(&src[0]);
    const uint32_t block = RdU32(&src[4]);
    const uint32_t csz   = RdU32(&src[8]);
    const uint32_t usz   = RdU32(&src[12]);

    printf("%s\n", inPath);
    printf("  tag 0x%08X  block %u  compressed %u  uncompressed %u\n", tag, block, csz, usz);

    if (tag != kPackageTag) {
        // A package that is NOT fully compressed starts with the same tag but is followed by the
        // version, not a block size. Saying which case this is beats "unsupported".
        printf("ERROR: tag mismatch - expected 0x%08X. This is not a UE3 package.\n", kPackageTag);
        return 1;
    }
    if (block == 0 || block > (1u << 24)) { printf("ERROR: implausible block size\n"); return 1; }
    if (usz == 0)                         { printf("ERROR: uncompressed size is zero\n"); return 1; }

    const uint32_t nChunks = (usz + block - 1) / block;
    const size_t   hdrLen  = 16 + (size_t)nChunks * 8;
    if (src.size() < hdrLen) { printf("ERROR: chunk table runs past end of file\n"); return 1; }

    struct Chunk { uint32_t comp, unc; };
    std::vector<Chunk> chunks(nChunks);
    uint64_t sumComp = 0, sumUnc = 0;
    for (uint32_t i = 0; i < nChunks; ++i) {
        chunks[i].comp = RdU32(&src[16 + i * 8]);
        chunks[i].unc  = RdU32(&src[16 + i * 8 + 4]);
        sumComp += chunks[i].comp;
        sumUnc  += chunks[i].unc;
    }
    printf("  %u chunks, table ends at 0x%zX, sum(compressed) %llu, sum(uncompressed) %llu\n",
           nChunks, hdrLen, (unsigned long long)sumComp, (unsigned long long)sumUnc);

    // Both of these are structural claims about the format, so check them rather than assume.
    // If either is wrong the blocks are not laid out the way this tool reads them.
    if (sumUnc != usz)
        printf("  WARNING: chunk uncompressed sizes sum to %llu, header says %u\n",
               (unsigned long long)sumUnc, usz);
    if (hdrLen + sumComp != src.size())
        printf("  WARNING: header + sum(compressed) = %llu but the file is %zu bytes -"
               " the blocks may not be contiguous\n",
               (unsigned long long)(hdrLen + sumComp), src.size());

    // zlib would start 0x78; LZO does not. Report it rather than producing garbage, because
    // UE3 supports both and which one a build uses is a property of the build, not of the file.
    if (src[hdrLen] == 0x78)
        printf("  NOTE: first block starts 0x78 - this looks like ZLIB, not LZO. Expect failure.\n");

    std::vector<uint8_t> dst;
    dst.reserve(usz);
    std::vector<uint8_t> tmp(block + 64);

    size_t off = hdrLen;
    for (uint32_t i = 0; i < nChunks; ++i) {
        const Chunk& c = chunks[i];
        if (off + c.comp > src.size()) {
            printf("ERROR: chunk %u runs past end of file\n", i);
            return 1;
        }
        if (c.unc > tmp.size()) tmp.resize(c.unc + 64);

        if (c.comp == c.unc) {
            // UE3 stores a block verbatim when compression did not pay. Not an error.
            memcpy(tmp.data(), &src[off], c.unc);
        } else {
            size_t used = 0;
            const char* err = "unknown";
            if (!Lzo1xDecompress(&src[off], c.comp, tmp.data(), c.unc, &used, &err)) {
                printf("ERROR: chunk %u/%u failed to decompress (%s) at input offset 0x%zX\n",
                       i, nChunks, err, off);
                return 1;
            }
            if (used != c.unc) {
                printf("ERROR: chunk %u produced %zu bytes, header declares %u\n", i, used, c.unc);
                return 1;
            }
        }
        dst.insert(dst.end(), tmp.begin(), tmp.begin() + c.unc);
        off += c.comp;
    }

    if (dst.size() != usz) {
        printf("ERROR: total output %zu bytes, header declares %u\n", dst.size(), usz);
        return 1;
    }

    // The decompressed stream must itself be a UE3 package, and its first four bytes are the same
    // tag. A tool that reported success on 28 MB of plausible-looking garbage would be worse than
    // one that failed, so check the one thing that garbage cannot fake.
    const bool inner = dst.size() >= 8 && RdU32(&dst[0]) == kPackageTag;
    printf("  decompressed %zu bytes; inner tag %s", dst.size(), inner ? "OK" : "*** WRONG ***");
    if (dst.size() >= 12)
        printf(" (version %u, licensee %u)", RdU32(&dst[4]) & 0xFFFF, RdU32(&dst[4]) >> 16);
    printf("\n");
    if (!inner) {
        printf("ERROR: output is not a package - the decompressor is wrong, not the input.\n");
        return 1;
    }

    FILE* f = nullptr;
    if (fopen_s(&f, outPath.c_str(), "wb") != 0 || !f) {
        printf("ERROR: cannot write %s\n", outPath.c_str());
        return 1;
    }
    fwrite(dst.data(), 1, dst.size(), f);
    fclose(f);
    printf("  -> %s\n", outPath.c_str());
    return 0;
}
