#include "FaultInjection.h"

#include <cerrno>
#include <memory>
#include <unistd.h>
#include <utility>

namespace
{
fault::Policy installedPolicy;

fault::Decision consult(const fault::Op operation, const int fileDescriptor, const std::size_t size)
{
    if (!installedPolicy)
        return fault::Decision::proceed();
    return installedPolicy(operation, fileDescriptor, size);
}
} // namespace

namespace fault
{
void install(Policy policy) { installedPolicy = std::move(policy); }

void clear() { installedPolicy = nullptr; }

ScopedPolicy::ScopedPolicy(Policy policy) { install(std::move(policy)); }

ScopedPolicy::~ScopedPolicy() { clear(); }

Policy failNth(const Op operation, const unsigned occurrence, const int errnoValue)
{
    auto seen = std::make_shared<unsigned>(0);
    return [operation, occurrence, errnoValue, seen](const Op currentOperation, int, std::size_t)
    {
        if (currentOperation != operation)
            return Decision::proceed();
        return ++*seen == occurrence ? Decision::fail(errnoValue) : Decision::proceed();
    };
}

Policy failEvery(const Op operation, const int errnoValue)
{
    return [operation, errnoValue](const Op currentOperation, int, std::size_t)
    { return currentOperation == operation ? Decision::fail(errnoValue) : Decision::proceed(); };
}

ssize_t write(const int fileDescriptor, const void* data, const std::size_t size)
{
    const Decision decision = consult(Op::Write, fileDescriptor, size);
    if (decision.errnoValue != 0)
    {
        errno = decision.errnoValue;
        return -1;
    }
    return ::write(fileDescriptor, data, size);
}

int fsync(const int fileDescriptor)
{
    const Decision decision = consult(Op::Fsync, fileDescriptor, 0);
    if (decision.errnoValue != 0)
    {
        errno = decision.errnoValue;
        return -1;
    }
    return ::fsync(fileDescriptor);
}
} // namespace fault
