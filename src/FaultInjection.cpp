#include "FaultInjection.h"

#include <cerrno>
#include <memory>
#include <unistd.h>
#include <utility>

namespace
{
fault::Policy activePolicy;

fault::Decision consult(const fault::Op op, const int fd, const std::size_t size)
{
    if (!activePolicy)
        return fault::Decision::proceed();
    return activePolicy(op, fd, size);
}
} // namespace

namespace fault
{
void install(Policy policy) { activePolicy = std::move(policy); }

void clear() { activePolicy = nullptr; }

ScopedPolicy::ScopedPolicy(Policy policy) { install(std::move(policy)); }

ScopedPolicy::~ScopedPolicy() { clear(); }

Policy failNth(const Op op, const unsigned n, const int errnoValue)
{
    auto seen = std::make_shared<unsigned>(0);
    return [op, n, errnoValue, seen](const Op current, int, std::size_t)
    {
        if (current != op)
            return Decision::proceed();
        return ++*seen == n ? Decision::fail(errnoValue) : Decision::proceed();
    };
}

Policy failEvery(const Op op, const int errnoValue)
{
    return [op, errnoValue](const Op current, int, std::size_t)
    { return current == op ? Decision::fail(errnoValue) : Decision::proceed(); };
}

ssize_t write(const int fd, const void* data, const std::size_t size)
{
    const Decision decision = consult(Op::Write, fd, size);
    if (decision.errnoValue != 0)
    {
        errno = decision.errnoValue;
        return -1;
    }
    return ::write(fd, data, size);
}

int fsync(const int fd)
{
    const Decision decision = consult(Op::Fsync, fd, 0);
    if (decision.errnoValue != 0)
    {
        errno = decision.errnoValue;
        return -1;
    }
    return ::fsync(fd);
}
} // namespace fault
