// Jolt's MutexBase and SharedMutexBase on MinGW, as native/fbengine_physics/CMakeLists.txt's patch
// of Jolt/Core/Mutex.h declares them: a slim reader/writer lock each, which needs no destroy and
// allocates nothing, where winpthreads allocates a mutex as it's first locked and libstdc++'s
// std::mutex and std::shared_mutex never destroy theirs, so every one locked leaked. windows.h is
// here alone, as its near, far, min, and max would be everywhere Jolt's headers are.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on

#include <Jolt/Core/Mutex.h>
#include <windows.h>

static_assert(sizeof(void*) == sizeof(SRWLOCK));

namespace {

PSRWLOCK srw(void*& lock) {
    return reinterpret_cast<PSRWLOCK>(&lock);
}

} // namespace

JPH_NAMESPACE_BEGIN

bool MutexBase::try_lock() {
    return TryAcquireSRWLockExclusive(srw(mLock)) != 0;
}

void MutexBase::lock() {
    AcquireSRWLockExclusive(srw(mLock));
}

void MutexBase::unlock() {
    ReleaseSRWLockExclusive(srw(mLock));
}

bool SharedMutexBase::try_lock() {
    return TryAcquireSRWLockExclusive(srw(mLock)) != 0;
}

void SharedMutexBase::lock() {
    AcquireSRWLockExclusive(srw(mLock));
}

void SharedMutexBase::unlock() {
    ReleaseSRWLockExclusive(srw(mLock));
}

bool SharedMutexBase::try_lock_shared() {
    return TryAcquireSRWLockShared(srw(mLock)) != 0;
}

void SharedMutexBase::lock_shared() {
    AcquireSRWLockShared(srw(mLock));
}

void SharedMutexBase::unlock_shared() {
    ReleaseSRWLockShared(srw(mLock));
}

JPH_NAMESPACE_END
