#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <sys/types.h> // ssize_t

// Test-only seam for injecting I/O failures.
//
// Engine code calls fault::write / fault::fsync instead of ::write / ::fsync.
// With no policy installed -- the production case -- both forward straight to
// the syscall, so the cost is one null check per call.
//
// Not thread-safe: the policy is a plain global. Fine while the engine is
// single-threaded; revisit if that changes.
namespace fault
{
enum class Op : std::uint8_t
{
    Write,
    Fsync
};

// What the policy decides to do with one syscall.
struct Decision
{
    // Non-zero: skip the syscall and report failure with this errno.
    int errnoValue = 0;

    static Decision proceed() { return {}; }
    static Decision fail(const int value) { return {value}; }
};

// Consulted once per syscall. `size` is the requested byte count for Write, 0 for Fsync.
using Policy = std::function<Decision(Op op, int fd, std::size_t size)>;

void install(Policy policy);
void clear();

// Installs on construction, clears on destruction. Prefer this in tests so a
// failing assertion cannot leave a policy armed for the next test.
class ScopedPolicy
{
  public:
    explicit ScopedPolicy(Policy policy);
    ~ScopedPolicy();

    ScopedPolicy(const ScopedPolicy&) = delete;
    ScopedPolicy& operator=(const ScopedPolicy&) = delete;
};

// Fails the nth (1-based) call of the given kind; every other call passes through.
[[nodiscard]] Policy failNth(Op op, unsigned n, int errnoValue);

// Fails every call of the given kind.
[[nodiscard]] Policy failEvery(Op op, int errnoValue);

// The injected syscalls.
ssize_t write(int fd, const void* data, std::size_t size);
int fsync(int fd);
} // namespace fault
