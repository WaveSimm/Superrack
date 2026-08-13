#include "AudioWorkerPool.h"
#include "AppSettings.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <avrt.h>
 #pragma comment (lib, "avrt.lib")
#endif

#if JUCE_INTEL
 #include <immintrin.h>   // _mm_pause (x86 전용)
#endif

#if JUCE_MAC
 #include <pthread.h>
 #include <sys/sysctl.h>
 #include <mach/mach.h>
 #include <mach/mach_time.h>
 #include <mach/thread_policy.h>
#endif

namespace
{
    inline void cpuPause() noexcept
    {
       #if JUCE_INTEL
        _mm_pause();
       #elif defined (__aarch64__) || defined (__arm64__)
        // arm64: YIELD 는 Apple 코어에서 사실상 no-op 이고 std::this_thread::yield 는
        // 커널 진입(수 µs)이라 RT 스핀에 부적합. ISB 가 파이프라인을 비워
        // 스핀 전력/코어 경쟁을 낮춘다 (x86 PAUSE 대응 관용구).
        __asm__ __volatile__ ("isb sy" ::: "memory");
       #else
        std::this_thread::yield();
       #endif
    }

   #if JUCE_MAC
    /** Apple Silicon 의 성능(P) 코어 수. Intel 맥/조회 실패 시 물리 코어 수. */
    int getPerformanceCoreCount()
    {
        int n = 0;
        size_t sz = sizeof (n);

        // perflevel0 = 최고 성능 레벨(M1: P코어 4). E코어는 perflevel1.
        if (sysctlbyname ("hw.perflevel0.physicalcpu", &n, &sz, nullptr, 0) == 0 && n > 0)
            return n;

        return juce::SystemStats::getNumPhysicalCpus();
    }

    /** ns → mach absolute time 틱. */
    juce::uint32 nsToMachTicks (double ns)
    {
        static mach_timebase_info_data_t tb = ([]
        {
            mach_timebase_info_data_t t {};
            mach_timebase_info (&t);
            if (t.numer == 0 || t.denom == 0) { t.numer = 1; t.denom = 1; }
            return t;
        })();

        return (juce::uint32) juce::jmax (1.0, ns * (double) tb.denom / (double) tb.numer);
    }

    /** 호출 스레드를 오디오 데드라인 스레드로 승격 (THREAD_TIME_CONSTRAINT_POLICY).

        QoS(USER_INTERACTIVE) 만으로는 스케줄러가 워커를 효율(E) 코어에 올릴 수 있고
        데드라인 보장도 없다. time-constraint 정책은 P코어 배치 + 블록 예산 기반
        선점 규칙을 준다 — CoreAudio 자체 IO 스레드와 같은 계열. */
    bool applyTimeConstraintPolicy (double periodNs)
    {
        thread_time_constraint_policy_data_t p {};
        p.period      = nsToMachTicks (periodNs);
        p.computation = nsToMachTicks (periodNs * 0.5);    // 워커가 쓰는 예상 시간
        p.constraint  = nsToMachTicks (periodNs * 0.85);   // 데드라인
        p.preemptible = 0;                                 // 데드라인 스레드 = 비선점

        return thread_policy_set (pthread_mach_thread_np (pthread_self()),
                                  THREAD_TIME_CONSTRAINT_POLICY,
                                  (thread_policy_t) &p,
                                  THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
    }
   #endif

    // 게이트 오픈 감지용 스핀 횟수 — 대략 수십 µs. 그 안에 다음 블록이 안 오면
    // 이벤트 대기로 내려가 코어를 놓아준다 (spin → event, 리서치 §3).
    constexpr int spinIterations = 4000;
}

//==============================================================================
AudioWorkerPool::AudioWorkerPool()
{
    usPerTick = 1.0e6 / (double) juce::Time::getHighResolutionTicksPerSecond();

    // A3 실측 결론(2026-07-03, docs/measurements/cpu-profile-parallel-2026-07-03.md):
    //  - 워커+오디오가 물리 코어를 거의 채우면(잔여 1코어) 저순위 스레드(플러그인
    //    백그라운드·GUI·OS) 굶음으로 워커가 실행 중 10~28ms 스톨 → 코어 2개 여유 필수.
    //  - MMCSS "Pro Audio" 등록 워커는 예약 윈도 스로틀로 ~2.3ms 주기 스톨
    //    → 워커는 TIME_CRITICAL 만 사용(미등록). 오디오 스레드는 드라이버 소관.
    // 실험용 오버라이드: SUPERRACK_WORKERS=n (0..6) / SUPERRACK_MMCSS=1 (재등록).
    useMmcss = juce::SystemStats::getEnvironmentVariable ("SUPERRACK_MMCSS", "") == "1";

    // 오디오 스레드 1 + 잔여 2코어(GUI/Writer/플러그인 백그라운드/OS) → 나머지가 워커.
   #if JUCE_MAC
    // macOS/Apple Silicon: 물리 코어 수(M1 = 8)를 그대로 쓰면 안 된다 — 그중 4개는
    // 효율(E) 코어로 오디오 블록 데드라인을 못 맞춘다. 실효 병렬도는 P코어 수.
    // 또 macOS 는 저QoS 스레드(GUI/디스크/스캔)를 알아서 E코어로 밀어내므로
    // Windows 의 "여유 2코어"(A3) 대신 P코어 1개만 남겨도 된다 → P − 2.
    //   M1(P4)  → 워커 2 (오디오 1 + 워커 2 = P코어 3/4 사용, 1개 여유)
    //   M1 Pro/Max(P8) → 워커 6(상한)
    const int cores = getPerformanceCoreCount();
    int numWorkers = juce::jlimit (0, 6, cores - 2);
   #else
    const int cores = juce::SystemStats::getNumPhysicalCpus();
    int numWorkers = juce::jlimit (0, 6, cores - 3);
   #endif

    // 우선순위: 환경변수(진단) > 앱 설정(0=자동) > 자동.
    if (const int s = AppSettings::get().workerCountOverride(); s > 0)
        numWorkers = juce::jlimit (0, 6, s);

    if (const auto env = juce::SystemStats::getEnvironmentVariable ("SUPERRACK_WORKERS", "");
        env.isNotEmpty())
        numWorkers = juce::jlimit (0, 6, env.getIntValue());

    workers.reserve ((size_t) numWorkers);
    for (int i = 0; i < numWorkers; ++i)
    {
        auto w = std::make_unique<Worker>();
        auto* self = w.get();   // unique_ptr 이라 벡터 재배치와 무관하게 주소 안정
        w->thread = std::thread ([this, self, i] { workerLoop (*self, i); });
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
void AudioWorkerPool::drainJobs (int executorId) noexcept
{
    const juce::ScopedNoDenormals noDenormals;   // 스레드별 FPU 플래그

    for (;;)
    {
        // acquire: 게이트 release-store 이전에 기록된 잡 배열/개수를 가시화.
        const auto idx = ticket.fetch_add (1, std::memory_order_acquire);
        if (idx >= (juce::uint64) jobCount.load (std::memory_order_relaxed))
            return;   // 게이트 닫힘(GATE_CLOSED+k) 또는 잡 소진

        // A3 계측: 슬롯 idx 는 이 스레드만 쓴다. 기록은 jobsDone(release) 이전
        // → 조인(acquire) 후 오디오 스레드에서 가시.
        JobStat* st = idx < (juce::uint64) maxSnapshotJobs ? &statScratch[idx] : nullptr;
        if (st != nullptr)
        {
            st->startUs  = usSince (blockT0);
            st->executor = (juce::int8) executorId;
        }

        const Job& j = jobs[idx];
        if (j.strip != nullptr && j.in != nullptr && j.out != nullptr)
            j.strip->process (j.in, j.out, j.numSamples);

        if (st != nullptr)
            st->endUs = usSince (blockT0);

        jobsDone.fetch_add (1, std::memory_order_release);
    }
}

void AudioWorkerPool::maybeCaptureSpike (juce::uint32 totalUs, int numJobs, bool parallel) noexcept
{
    if (totalUs < spikeThresholdUs.load (std::memory_order_relaxed)
        || totalUs <= snapWorstUs.load (std::memory_order_relaxed))
        return;

    // seqlock 쓰기(오디오 스레드 전용) — 리더는 홀수/변경 감지 시 재시도.
    snapSeq.fetch_add (1, std::memory_order_release);

    snap.totalUs  = totalUs;
    snap.numJobs  = numJobs;
    snap.parallel = parallel;
    const int n = juce::jmin (numJobs, maxSnapshotJobs);
    for (int i = 0; i < n; ++i)
        snap.stats[i] = statScratch[i];

    snapSeq.fetch_add (1, std::memory_order_release);
    snapWorstUs.store (totalUs, std::memory_order_relaxed);
}

bool AudioWorkerPool::readSpikeSnapshot (SpikeSnapshot& out) const noexcept
{
    if (snapWorstUs.load (std::memory_order_relaxed) == 0)
        return false;

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto s0 = snapSeq.load (std::memory_order_acquire);
        if ((s0 & 1u) != 0)
            continue;
        out = snap;
        std::atomic_thread_fence (std::memory_order_acquire);
        if (snapSeq.load (std::memory_order_relaxed) == s0)
            return true;
    }
    return false;   // 계속 쓰는 중 — 진단용이라 포기 허용
}

void AudioWorkerPool::processJobs (const Job* jobList, int numJobs) noexcept
{
    if (numJobs <= 0)
        return;

    blockT0 = juce::Time::getHighResolutionTicks();

    // 병렬 비활성·워커 없음·잡 1개면 인라인 직렬 실행 (계측은 동일 — A/B 대조용).
    if (! enabled.load (std::memory_order_relaxed) || workers.empty() || numJobs <= 1)
    {
        for (int i = 0; i < numJobs; ++i)
        {
            JobStat* st = i < maxSnapshotJobs ? &statScratch[i] : nullptr;
            if (st != nullptr) { st->startUs = usSince (blockT0); st->executor = -1; }

            const Job& j = jobList[i];
            if (j.strip != nullptr && j.in != nullptr && j.out != nullptr)
                j.strip->process (j.in, j.out, j.numSamples);

            if (st != nullptr)
                st->endUs = usSince (blockT0);
        }

        maybeCaptureSpike (usSince (blockT0), numJobs, false);
        return;
    }

    // 게시: 잡 → 카운터 리셋 → 게이트 오픈(release) 순서가 워커 가시성을 보장.
    jobs = jobList;
    jobCount.store (numJobs, std::memory_order_relaxed);
    jobsDone.store (0, std::memory_order_relaxed);
    ticket.store (0, std::memory_order_release);   // ── 게이트 오픈

    for (auto& w : workers)
        w->wake.signal();   // 자고 있으면 깨움, 스핀 중이면 no-op 수준

    drainJobs (-1);         // 오디오 스레드도 처리에 참여

    // 조인: 모든 잡 완료까지 스핀 (남은 잡 = 다른 스레드가 실행 중인 것뿐).
    while (jobsDone.load (std::memory_order_acquire) < numJobs)
        cpuPause();

    ticket.store (GATE_CLOSED, std::memory_order_relaxed);   // ── 게이트 클로즈

    maybeCaptureSpike (usSince (blockT0), numJobs, true);
}

//==============================================================================
void AudioWorkerPool::setRealtimeBlockTiming (double sampleRate, int blockSize) noexcept
{
    if (sampleRate <= 0.0 || blockSize <= 0)
        return;

    rtPeriodNs.store ((double) blockSize * 1.0e9 / sampleRate, std::memory_order_release);
    rtPolicyGen.fetch_add (1, std::memory_order_release);

    for (auto& w : workers)
        w->wake.signal();   // 스핀 중이 아니라 자고 있어도 즉시 적용되게
}

//==============================================================================
void AudioWorkerPool::workerLoop (Worker& self, int workerIndex)
{
   #if JUCE_WINDOWS
    // 오디오 스레드급 우선순위 — 프로세스 클래스는 건드리지 않는다(리서치 §3).
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (useMmcss)
    {
        DWORD mmcssTaskIndex = 0;
        AvSetMmThreadCharacteristicsW (L"Pro Audio", &mmcssTaskIndex);
    }
   #elif JUCE_MAC
    // 장치가 열리기 전(블록 주기 미확정)에는 QoS 만. 장치 시작 시
    // setRealtimeBlockTiming 이 세대를 올리면 아래 루프에서 time-constraint 로 승격.
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
    juce::uint32 appliedPolicyGen = 0;
   #endif

    while (! shouldExit.load (std::memory_order_acquire))
    {
       #if JUCE_MAC
        if (const auto gen = rtPolicyGen.load (std::memory_order_acquire);
            gen != appliedPolicyGen)
        {
            appliedPolicyGen = gen;

            if (const double ns = rtPeriodNs.load (std::memory_order_acquire); ns > 0.0)
                applyTimeConstraintPolicy (ns);
        }
       #endif

        drainJobs (workerIndex);

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
