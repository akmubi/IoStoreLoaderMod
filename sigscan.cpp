#include "sigscan.hpp"

#include <cstring>

namespace sigscan {

static inline int
hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int
exec_spans(HMODULE module, Span* out_spans, int max_spans)
{
    if (!module || !out_spans || max_spans <= 0) {
        return 0;
    }

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    auto* sec = IMAGE_FIRST_SECTION(nt);
    WORD n_sec = nt->FileHeader.NumberOfSections;

    int n_spans = 0;
    for (WORD i = 0; i < n_sec && n_spans < max_spans; ++i) {
        DWORD ch = sec[i].Characteristics;
        if ((ch & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        Span& s = out_spans[n_spans];
        std::memset(s.name, 0, sizeof(s.name));
        std::memcpy(s.name, sec[i].Name, IMAGE_SIZEOF_SHORT_NAME);

        uint64_t vsz = sec[i].Misc.VirtualSize;
        uint64_t rsz = sec[i].SizeOfRawData;
        s.base = base + sec[i].VirtualAddress;
        s.size = static_cast<size_t>((vsz > rsz) ? vsz : rsz);

        ++n_spans;
    }
    return n_spans;
}

size_t
parse_pattern(const char* pattern, uint8_t* out_bytes, char* out_mask, size_t max_len)
{
    if (!pattern || !out_bytes || !out_mask || max_len == 0) {
        return 0;
    }

    size_t n = 0;
    const char* p = pattern;

    auto skip_ws = [&]() {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') {
            ++p;
        }
    };

    skip_ws();
    while (*p && n < max_len) {
        skip_ws();
        if (!*p) {
            break;
        }

        if (*p == '?') {
            out_bytes[n] = 0;
            out_mask[n]  = '?';
            ++n;
            // support ? or ??
            ++p;
            if (*p == '?') {
                ++p;
            }
        } else {
            int hi = hex_val(p[0]);
            int lo = hex_val(p[1]);
            if (hi < 0 || lo < 0) {
                return 0;
            }

            out_bytes[n] = static_cast<uint8_t>((hi << 4) | lo);
            out_mask[n]  = 'x';
            ++n;
            p += 2;
        }

        skip_ws();
    }

    if (n < max_len) {
        out_mask[n] = '\0';
    }
    return n;
}

static inline bool
match_at(const uint8_t* a, const uint8_t* b, const char* mask, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (mask[i] != '?' && a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

const uint8_t*
find(const uint8_t* hay, size_t hay_len, const uint8_t* sig, const char* mask, size_t sig_len)
{
    if (!hay || !sig || !mask || sig_len == 0 || hay_len < sig_len) {
        return nullptr;
    }

    for (size_t i = 0; i + sig_len <= hay_len; ++i) {
        const uint8_t* p = hay + i;
        if (match_at(p, sig, mask, sig_len)) {
            return p;
        }
    }
    return nullptr;
}

const uint8_t*
scan_exec(HMODULE module, const char* pattern)
{
    uint8_t bytes[1024];
    char mask[1024];

    size_t len = parse_pattern(pattern, bytes, mask, sizeof(bytes));
    if (len == 0) {
        return nullptr;
    }

    Span spans[32];
    int n_spans = exec_spans(module, spans, 32);
    if (n_spans <= 0) {
        return nullptr;
    }

    for (int i = 0; i < n_spans; ++i) {
        const Span& s = spans[i];
        const uint8_t* m = find(s.base, s.size, bytes, mask, len);
        if (m) {
            return m;
        }
    }
    return nullptr;
}

} // namespace sigscan
