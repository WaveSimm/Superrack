#include "AudioWorkerPool.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <avrt.h>
 #pragma comment (lib, "avrt.lib")
#endif

#include <immintrin.h>   // _mm_pause

namespace
{
    inline void cpuPause() noexcept
    {
       #if JUCE_INTEL
        _mm_pause();
       #else
        std::this_thread::yield();
       #endif
    }

    // 게이트 오픈 감지용 스핀 횟수 — 대략 수십 µs. 그 안에 다음 블록이 안 오면
    // 이벤트 대기로 내려가 코어를 놓아준다 (spin → event, 리서치 §3).
    constexpr int spinIterations = 4000;
}

//==============================================================================
AudioWorkerPool::AudioWorkerPool()
{
    // 1코어 = GUI/Writer/OS 몫, 1코어 = 오디오 스레드 → 나머지가 워커.
    const int cores = juce::SystemStats::getNumPhysicalCpus();
    const int numWorkers = juce::jlimit (0, 6, cores - 2);

    workers.reserve ((size_t) numWorkers);
    for (int i = 0; i < numWorkers; ++i)
    {
        auto w = std::make_unique<Worker>();
        auto* self = w.get();   // unique_ptr 이라 벡터 재배치와 무관하게 주소 안정
        w->thread = std::thread ([this, self] { workerLoop (*self); });
        workers.push_back (std::move (w));
    }
}

AudioWorkerPool::~AudioWorkerPool()
{
    shouldExit.store (true, std::memory_order_release);
    for (auto& w : workers)
        w->wake.signal();
    for (auto& w : workers)
        if (w->thread.joinable())
            w->thread.join();
}

//==============================================================================
void AudioWorkerPool::drainJobs() noexcept
{
    const juce::ScopedNoDenormals noDenormals;   // 스레드별 FPU 플래그

    for (;;)
    {
        // acquire: 게이트 release-store 이전에 기록된 잡 배열/개수를 가시화.
        const auto idx = ticket.fetch_add (1, std::memory_order_acquire);
        if (idx >= (juce::uint64) jobCount.load (std::memory_order_relaxed))
            return;   // 게이트 닫힘(GATE_CLOSED+k) 또는 잡 소진

        const Job& j = jobs[idx];
        if (j.strip != nullptr && j.in != nullptr && j.out != nullptr)
            j.strip->process (j.in, j.out, j.numSamples);

        jobsDone.fetch_add (1, std::memory_order_release);
    }
}

void AudioWorkerPool::processJobs (const Job* jobList, int numJobs) noexcept
{
    if (numJobs <= 0)
        return;

    // 병렬 비활성·워커 없음·잡 1개면 인라인 직렬 실행.
    if (! enabled.load (std::memory_order_relaxed) || workers.empty() || numJobs <= 1)
    {
        for (int i = 0; i < numJobs; ++i)
        {
            const Job& j = jobList[i];
            if (j.strip != nullptr && j.in != nullptr && j.out != nullptr)
                j.strip->process (j.in, j.out, j.numSamples);
        }
        return;
    }

    // 게시: 잡 → 카운터 리셋 → 게이트 오픈(release) 순서가 워커 가시성을 보장.
    jobs = jobList;
    jobCount.store (numJobs, std::memory_order_relaxed);
    jobsDone.store (0, std::memory_order_relaxed);
    ticket.store (0, std::memory_order_release);   // ── 게이트 오픈

    for (auto& w : workers)
        w->wake.signal();   // 자고 있으면 깨움, 스핀 중이면 no-op 수준

    drainJobs();            // 오디오 스레드도 처리에 참여

    // 조인: 모든 잡 완료까지 스핀 (남은 잡 = 다른 스레드가 실행 중인 것뿐).
    while (jobsDone.load (std::memory_order_acquire) < numJobs)
        cpuPause();

    ticket.store (GATE_CLOSED, std::memory_order_relaxed);   // ── 게이트 클로즈
}

//==============================================================================
void AudioWorkerPool::workerLoop (Worker& self)
{
   #if JUCE_WINDOWS
    // 오디오 스레드급 우선순위 — 프로세스 클래스는 건드리지 않는다(리서치 §3).
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    DWORD mmcssTaskIndex = 0;
    AvSetMmThreadCharacteristicsW (L"Pro Audio", &mmcssTaskIndex);
   #endif

    while (! shouldExit.load (std::memory_order_acquire))
    {
        drainJobs();

        // 다음 블록 대기: 짧은 스핀(게이트 오픈 감지) → 이벤트 대기.
        bool opened = false;
        for (int k = 0; k < spinIterations; ++k)
        {
            if (ticket.load (std::memory_order_relaxed) < GATE_CLOSED)
            {
                opened = true;
                break;
            }
            cpuPause();
        }

        // 2ms 타임아웃 — 시그널 유실·정지 상태에서도 폴링으로 복구.
        if (! opened)
            self.wake.wait (2);
    }
}
