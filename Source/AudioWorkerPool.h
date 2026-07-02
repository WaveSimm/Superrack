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

    설계 근거 (docs/research §3):
    - 대기 전략 = 짧은 spin → 이벤트 대기 (1ms 예산에서 웨이크업 비용 최소화)
    - 워커 = TIME_CRITICAL + MMCSS "Pro Audio" (프로세스 전체 우선순위 부작용 없음)
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

private:
    static constexpr juce::uint64 GATE_CLOSED = juce::uint64 (1) << 40;

    struct Worker;
    void workerLoop (Worker& self);
    void drainJobs() noexcept;   // 티켓에서 잡을 가져와 실행 (워커·오디오 공용)

    const Job*                jobs { nullptr };   // 게이트 오픈 전 기록
    std::atomic<int>          jobCount { 0 };
    std::atomic<int>          jobsDone { 0 };
    std::atomic<juce::uint64> ticket   { GATE_CLOSED };

    std::atomic<bool> enabled    { true };
    std::atomic<bool> shouldExit { false };

    struct Worker
    {
        std::thread         thread;
        juce::WaitableEvent wake;   // auto-reset
    };
    std::vector<std::unique_ptr<Worker>> workers;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioWorkerPool)
};
