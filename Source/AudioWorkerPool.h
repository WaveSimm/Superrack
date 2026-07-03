#pragma once

#include <juce_core/juce_core.h>
#include "ChannelStrip.h"
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

//==============================================================================
/** 채널 병렬 DSP 워커 풀 (BACKLOG A2).

    채널이 완전 독립(1:1, 합산 없음)이므로 "채널 = 잡" fork-join 으로 나눈다.
    오디오 콜백이 잡 배열을 게시하면 워커들과 **오디오 스레드 자신**이 공유
    티켓 카운터에서 잡을 가져가 처리하고, 전부 끝날 때까지 스핀 조인한다.

    설계 근거 (docs/research §3, A3 실측으로 개정):
    - 대기 전략 = 짧은 spin → 이벤트 대기 (1ms 예산에서 웨이크업 비용 최소화)
    - 워커 = TIME_CRITICAL 만 (A3 실측: MMCSS 등록 시 예약 윈도 스로틀 ~2.3ms 스톨)
    - 워커 수 = 물리코어 − 3 (A3 실측: 잔여 1코어면 저순위 스레드 굶음 → 10~28ms 스톨)
    - 범용 태스크 라이브러리(taskflow 등)는 RT 보장이 없어 배제

    동기화 (무락, 콜백 내 무할당):
    - `ticket` : 단조 증가 클레임 카운터. 게이트 열림 = store(0, release),
      닫힘 = store(GATE_CLOSED). 닫힌 동안의 클레임은 idx >= jobCount 로 자연 폐기
      → 블록 경계에서 늦게 깬 워커가 잡을 이중 실행할 수 없다.
    - 잡 배열/개수는 게이트 release-store 이전에 기록, 클레임의 acquire 로 가시화.
    - 조인: jobsDone == numJobs 스핀. 클레임 1건 = 완료 증가 1건이 1:1 이므로
      워커가 잡 실행 중이면 조인이 끝나지 않는다(다음 블록 상태 변경과 격리).

    스레드 규약: processJobs() 는 오디오 스레드 전용. setEnabled 는 아무 스레드.
    한 Job(=한 ChannelStrip)은 블록당 정확히 한 스레드가 실행한다. */
class AudioWorkerPool
{
public:
    struct Job
    {
        ChannelStrip* strip = nullptr;
        const float*  in    = nullptr;
        float*        out   = nullptr;
        int           numSamples = 0;
    };

    AudioWorkerPool();
    ~AudioWorkerPool();

    /** 병렬 처리 on/off (off 면 processJobs 가 인라인 직렬 실행). */
    void setEnabled (bool b) noexcept { enabled.store (b, std::memory_order_relaxed); }
    bool isEnabled() const noexcept   { return enabled.load (std::memory_order_relaxed); }

    int getNumWorkers() const noexcept { return (int) workers.size(); }

    /** 잡 전부 완료까지 반환하지 않는다(오디오 스레드 전용, 무할당). */
    void processJobs (const Job* jobList, int numJobs) noexcept;

    //==========================================================================
    // ── A3 스톨 계측 ─────────────────────────────────────────────────────────
    // 블록마다 잡별 {클레임 시각, 종료 시각, 실행 스레드} 를 기록하고(QPC 2회/잡),
    // 블록 총 시간이 임계(예산)를 넘으면 그 블록의 스냅샷을 보관한다(구간 내 최악 1개).
    // 늦은 start = 클레임/웨이크 지연, 긴 (end-start) = 실행 중 스톨 — 원인이 갈린다.

    struct JobStat
    {
        juce::uint32 startUs = 0;    // 블록 t0 기준 µs (클레임 직후 = 실행 시작)
        juce::uint32 endUs   = 0;    // 실행 종료
        juce::int8   executor = -1;  // -1 = 오디오 스레드, 0.. = 워커 인덱스
    };

    static constexpr int maxSnapshotJobs = 64;

    struct SpikeSnapshot
    {
        juce::uint32 totalUs = 0;    // 블록 전체(게시→조인 완료)
        int          numJobs = 0;
        bool         parallel = false;
        JobStat      stats[maxSnapshotJobs];
    };

    /** 스파이크 판정 임계(µs). 장치 시작 시 블록 예산으로 설정. 0xffffffff = 끔. */
    void setSpikeThresholdUs (juce::uint32 us) noexcept
        { spikeThresholdUs.store (us, std::memory_order_relaxed); }

    /** 보관된 최악 블록을 버리고 새 관찰 구간 시작 (메시지 스레드). */
    void resetSpikeCapture() noexcept { snapWorstUs.store (0, std::memory_order_relaxed); }

    /** 관찰 구간의 최악 블록 스냅샷 읽기 (메시지 스레드). 없으면 false.
        seqlock — 드물게 쓰기와 겹치면 재시도, 진단용이라 유실 허용. */
    bool readSpikeSnapshot (SpikeSnapshot& out) const noexcept;

private:
    static constexpr juce::uint64 GATE_CLOSED = juce::uint64 (1) << 40;

    struct Worker;
    void workerLoop (Worker& self, int workerIndex);
    void drainJobs (int executorId) noexcept;   // 티켓에서 잡을 가져와 실행 (워커·오디오 공용)

    /** 블록 종료 시(오디오 스레드) 총 시간이 임계 초과·구간 최악이면 스냅샷 보관. */
    void maybeCaptureSpike (juce::uint32 totalUs, int numJobs, bool parallel) noexcept;

    juce::uint32 usSince (juce::int64 t0) const noexcept
        { return (juce::uint32) juce::jmax ((juce::int64) 0,
              (juce::int64) ((juce::Time::getHighResolutionTicks() - t0) * usPerTick)); }

    const Job*                jobs { nullptr };   // 게이트 오픈 전 기록
    std::atomic<int>          jobCount { 0 };
    std::atomic<int>          jobsDone { 0 };
    std::atomic<juce::uint64> ticket   { GATE_CLOSED };

    // ── A3 계측 상태 ──
    // statScratch[i] 는 잡 i 를 클레임한 스레드만 쓴다(경쟁 없음). blockT0 은
    // 게이트 release-store 이전 기록 → 클레임 acquire 로 가시화.
    double        usPerTick = 0.0;
    juce::int64   blockT0   = 0;
    JobStat       statScratch[maxSnapshotJobs];
    std::atomic<juce::uint32> spikeThresholdUs { 0xffffffffu };
    std::atomic<juce::uint32> snapWorstUs { 0 };
    std::atomic<juce::uint32> snapSeq { 0 };     // 짝수 = 안정, 홀수 = 쓰는 중
    SpikeSnapshot snap;

    std::atomic<bool> enabled    { true };
    std::atomic<bool> shouldExit { false };
    bool              useMmcss   = false;  // A3: 기본 미등록, SUPERRACK_MMCSS=1 로 재등록

    struct Worker
    {
        std::thread         thread;
        juce::WaitableEvent wake;   // auto-reset
    };
    std::vector<std::unique_ptr<Worker>> workers;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioWorkerPool)
};
