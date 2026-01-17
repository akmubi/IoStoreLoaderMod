#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>

namespace sigscan {

struct Span {
    const uint8_t* base;
    size_t         size;
    char           name[9];
};

/// Collect executable sections of a PE module.
int
exec_spans(HMODULE module, Span* out_spans, int max_spans);

/// Parse IDA-style pattern: "48 8B ?? ?? 89 ? 90"
size_t
parse_pattern(const char* pattern, uint8_t* out_bytes, char* out_mask, size_t max_len);

/// Find first match of pattern in [hay, hay+hay_len).
const uint8_t*
find(const uint8_t* hay, size_t hay_len, const uint8_t* sig, const char* mask, size_t sig_len);

/// Scan all executable spans for pattern. Returns match address or nullptr.
const uint8_t*
scan_exec(HMODULE module, const char* pattern);

/// Helpers: resolve common x64 encodings
inline const uint8_t*
rip_rel32(const uint8_t* disp32_at)
{
    int32_t d = *reinterpret_cast<const int32_t*>(disp32_at);
    return disp32_at + 4 + d;
}

// For E8 xx xx xx xx (call rel32) or jmp rel32 etc.
// imm32_at points at the 4-byte immediate.
inline const uint8_t*
rel32(const uint8_t* imm32_at)
{
    int32_t d = *reinterpret_cast<const int32_t*>(imm32_at);
    return imm32_at + 4 + d;
}

} // namespace sigscan
