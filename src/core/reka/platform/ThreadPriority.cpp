#include "reka/platform/ThreadPriority.h"

#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace reka {

#if defined(_WIN32)

ScopedAudioThreadPriority::ScopedAudioThreadPriority() noexcept {
    // Record the priority before MMCSS boosts the thread, so the destructor restores the original one.
    previousPriority_ = GetThreadPriority(GetCurrentThread());
    DWORD taskIndex = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (handle_ != nullptr) AvSetMmThreadPriority(static_cast<HANDLE>(handle_), AVRT_PRIORITY_CRITICAL);
    elevated_ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0 || handle_ != nullptr;
}

ScopedAudioThreadPriority::~ScopedAudioThreadPriority() noexcept {
    if (handle_ != nullptr) AvRevertMmThreadCharacteristics(static_cast<HANDLE>(handle_));
    SetThreadPriority(GetCurrentThread(), previousPriority_);
}

#else

ScopedAudioThreadPriority::ScopedAudioThreadPriority() noexcept {
    sched_param param{};
    int policy = 0;
    if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) previousPriority_ = param.sched_priority;
    sched_param rt{};
    rt.sched_priority = sched_get_priority_max(SCHED_FIFO) - 10;
    elevated_ = pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt) == 0;
}

ScopedAudioThreadPriority::~ScopedAudioThreadPriority() noexcept {
    if (!elevated_) return;
    sched_param param{};
    param.sched_priority = previousPriority_;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
}

#endif

}  // namespace reka
