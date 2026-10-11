// libSystem functions that call BACK into guest code: qsort, bsearch, atexit. The comparator of qsort is a real Mach-O function
// of the guest (or a libSystem thunk such as strcmp); ORVYN's host code never substitutes its own comparison. Every callback
// target is validated first: it must lie in an executable segment of a loaded image or in the libSystem thunk table.
#include <algorithm>
#include <cstring>
#include <numeric>
#include <vector>

#include "orvyn/libsystem.hpp"
#include "internal.hpp"

namespace orvyn::libsystem {

bool is_valid_callback(uint64_t fn) {
    RuntimeContext* c = context();
    if (!c || fn == 0) return false;
    for (const auto& r : c->code_ranges)
        if (fn >= r.first && fn < r.second) return true;
    return false;
}

namespace {

using abi::GuestCallArgs;

constexpr uint64_t kMaxSortBytes = 256ull << 20;
constexpr uint64_t kMaxSortElements = 1ull << 26;
constexpr size_t kMaxAtexit = 256;

// Calls cmp(a, b) in the guest. false = the sort must stop (trap raised, or the guest called exit()).
bool compare(uint64_t cmp, const uint8_t* a, const uint8_t* b, int& out) {
    GuestCallArgs args;
    args.push_int(reinterpret_cast<uint64_t>(a)).push_int(reinterpret_cast<uint64_t>(b));
    abi::CallOutcome r = abi::call_guest_nested(cmp, args);
    if (r.refused) { detail::trap("comparator callback refused: " + r.refusal); return false; }
    if (r.exited) return false;
    out = static_cast<int32_t>(static_cast<uint32_t>(r.value));
    return true;
}

// void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*))
// Merge sort over an index array: the comparator is only ever given pointers INTO the guest's array, never into host memory,
// and an inconsistent comparator cannot make the sort read or write out of bounds.
void w_qsort(uint8_t* base, uint64_t n, uint64_t size, uint64_t cmp) {
    if (n < 2 || size == 0) return;
    if (!base) { detail::fail_errno(detail::kEFAULT); return; }
    if (n > kMaxSortElements || n > kMaxSortBytes / size) { detail::trap("qsort: array too large"); return; }
    if (!is_valid_callback(cmp)) { detail::trap("qsort: comparator is not executable guest code"); return; }

    std::vector<uint32_t> a(n), b(n);
    std::iota(a.begin(), a.end(), 0u);
    for (uint64_t width = 1; width < n; width *= 2) {
        for (uint64_t lo = 0; lo < n; lo += 2 * width) {
            const uint64_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            uint64_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                int c = 0;
                if (!compare(cmp, base + static_cast<uint64_t>(a[i]) * size, base + static_cast<uint64_t>(a[j]) * size, c)) return;
                b[k++] = c <= 0 ? a[i++] : a[j++];            // <= keeps the sort stable
            }
            while (i < mid) b[k++] = a[i++];
            while (j < hi) b[k++] = a[j++];
        }
        a.swap(b);
    }
    std::vector<uint8_t> copy(base, base + n * size);
    for (uint64_t i = 0; i < n; ++i) std::memcpy(base + i * size, copy.data() + static_cast<uint64_t>(a[i]) * size, size);
}

// void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*))
void* w_bsearch(const uint8_t* key, const uint8_t* base, uint64_t n, uint64_t size, uint64_t cmp) {
    if (n == 0 || size == 0) return nullptr;
    if (!is_valid_callback(cmp)) { detail::trap("bsearch: comparator is not executable guest code"); return nullptr; }
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        int c = 0;
        if (!compare(cmp, key, base + mid * size, c)) return nullptr;
        if (c == 0) return const_cast<uint8_t*>(base + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}

// int atexit(void (*func)(void)): handlers run in reverse order when the guest exits or main returns.
int w_atexit(uint64_t fn) {
    RuntimeContext* c = context();
    if (!c || !is_valid_callback(fn)) return static_cast<int>(detail::fail_errno(detail::kEINVAL));
    if (c->atexit_handlers.size() >= kMaxAtexit) return static_cast<int>(detail::fail_errno(detail::kENOMEM));
    c->atexit_handlers.push_back(fn);
    c->atexit_args.resize(c->atexit_handlers.size() - 1, 0);
    c->atexit_args.push_back(0);
    return 0;
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

namespace detail {
void add_callback_symbols(std::vector<HostSymbol>& t) {
    t.push_back({"_qsort", addr(w_qsort)});
    t.push_back({"_bsearch", addr(w_bsearch)});
    t.push_back({"_atexit", addr(w_atexit)});
}
}  // namespace detail

}  // namespace orvyn::libsystem
