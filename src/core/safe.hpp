// Trust boundary for pointers that cross the AML / JNI / game-image boundary.
//
// Why this exists: the installed Android Mod Loader is a third-party loader with
// a version-dependent virtual table. Calling a slot the installed version does
// not have does not throw, does not return null and does not log anything - the
// call lands in whatever function happens to sit at that index and returns
// garbage (a small integer, a stale pointer, half a handle). Feeding that value
// into std::string / strlen / a JNI call is a SIGSEGV with an address like 0x7,
// which is exactly the crash this header prevents.
//
// Rules:
//   * every const char*/void* that came from aml->*, a JNIEnv or a game array
//     must pass IsSane* before it is dereferenced;
//   * on failure the caller falls back to a value it owns and logs the raw
//     pointer, so the log names the broken loader slot instead of crashing.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace usa1to1 {
namespace core {

// The first pages of the address space are never valid user data; catching
// 0x7 / 0x8 / 0x1 here is what turns the old crash into a log line.
inline constexpr uintptr_t kMinSanePointer = 0x10000;

inline bool IsSanePointer(const void *p) {
    return p != nullptr && reinterpret_cast<uintptr_t>(p) >= kMinSanePointer;
}

// True only if the range [p, p+len) is inside mapped memory. Uses mincore(2),
// which is cheap and side-effect free; a wrong-but-large pointer such as a
// stale JNIEnv* is rejected here instead of faulting on first dereference.
inline bool IsMappedRange(const void *p, size_t len) {
    if (!IsSanePointer(p) || len == 0)
        return false;
    const long pageSizeL = ::sysconf(_SC_PAGESIZE);
    if (pageSizeL <= 0)
        return false;
    const uintptr_t pageSize = static_cast<uintptr_t>(pageSizeL);
    const uintptr_t pageMask = pageSize - 1;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = begin + len - 1;
    if (end < begin) // overflow
        return false;
    for (uintptr_t page = begin & ~pageMask;; page += pageSize) {
        unsigned char resident = 0;
        if (::mincore(reinterpret_cast<void *>(page), static_cast<size_t>(pageSize), &resident) != 0)
            return false;
        if (page >= (end & ~pageMask))
            break;
    }
    return true;
}

// Reads *p only when the byte is mapped.
inline bool TryReadByte(const void *p, unsigned char &out) {
    if (!IsMappedRange(p, 1))
        return false;
    out = *static_cast<const unsigned char *>(p);
    return true;
}

// A C string from the engine is only used when it is mapped, starts where the
// caller expects and is NUL-terminated inside `maxLen`. Page-by-page scan keeps
// the syscall count low while still never touching unmapped memory.
inline bool IsReadableCString(const char *s, size_t maxLen = 1024) {
    if (!IsSanePointer(s) || maxLen == 0)
        return false;
    const long pageSizeL = ::sysconf(_SC_PAGESIZE);
    if (pageSizeL <= 0)
        return false;
    const size_t pageSize = static_cast<size_t>(pageSizeL);
    size_t scanned = 0;
    const char *cur = s;
    while (scanned < maxLen) {
        if (!IsMappedRange(cur, 1))
            return false;
        const size_t offsetInPage = static_cast<size_t>(reinterpret_cast<uintptr_t>(cur) % pageSize);
        const size_t chunk = std::min(pageSize - offsetInPage, maxLen - scanned);
        if (std::memchr(cur, '\0', chunk) != nullptr)
            return true;
        cur += chunk;
        scanned += chunk;
    }
    return false;
}

// Absolute paths only: an AML getter that returns a small integer or a handle
// ("7", 0x7) never passes, and neither does an empty string.
inline bool IsSanePath(const char *p) {
    if (!IsReadableCString(p, 4096))
        return false;
    return p[0] == '/';
}

inline std::string SanePathOr(const char *p, const char *fallback) {
    if (IsSanePath(p))
        return std::string(p);
    return std::string(fallback ? fallback : "");
}

// Handles (XDL handles, libGTASA base addresses, function pointers) only need
// to be plausible and mapped; a function pointer is additionally checked for
// the Thumb bit being irrelevant, so anything below the user range is dropped.
inline bool IsSaneHandle(const void *p) { return IsMappedRange(p, 1); }

// A table entry is only usable when the slot itself is mapped and the pointer
// it holds points at a whole `structSize`-byte object. This is what stops the
// device crash where CModelInfo::ms_modelInfoPtrs[i] held the integer 10184 and
// `info->m_szModelName[0]` faulted at 10184 + 8.
template <class T> inline bool IsUsableTableEntry(T *const *table, int index, T *&out, size_t structSize) {
    out = nullptr;
    if (!table || index < 0)
        return false;
    if (!IsMappedRange(&table[index], sizeof(table[index])))
        return false;
    T *entry = table[index];
    if (!IsMappedRange(entry, structSize))
        return false;
    out = entry;
    return true;
}

inline std::string HexPointer(const void *p) {
    char buf[3 + 2 * sizeof(uintptr_t)] = {0};
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
    return std::string(buf);
}

} // namespace core
} // namespace usa1to1
