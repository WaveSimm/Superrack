#pragma once

#include <juce_events/juce_events.h>
#include "AudioEngine.h"

//==============================================================================
/** 채널 수 단계별 CPU 부하 자동 프로파일러 (Phase 4).

    물리 채널이 2개뿐이어도(UR22mkII) 합성 부하(AudioEngine::setSyntheticChannels)로
    2→4→8→16→24→32 채널의 실제 체인 처리 부하를 재현·측정한다.

    절차(메시지 스레드 Timer):
      1. Ch1 체인을 합성 채널 전체에 복제 (prepareSyntheticStrips)
      2. 단계마다: 합성 채널 수 설정 → 안정화 1초 → 통계 리셋 → 8초 측정
         (DSP avg/peak, 예산 초과 콜백 수, 장치 xrun 델타)
      3. 종료: 합성 부하 해제 + 체인 제거, 마크다운 리포트 저장
         (<Documents>/Superrack/profiles/cpu-profile-*.md)

    안정 판정: xrun 0 && 예산 초과 0 && peak < 0.95.
    한계 채널 수 = 처음부터 연속으로 안정인 마지막 단계.
*/
class CpuProfiler : private juce::Timer
{
public:
    struct StepResult
    {
        int    channels = 0;
        double avgLoad  = 0.0;   // 0~1
        double peakLoad = 0.0;   // 0~1+
        int    xruns    = 0;     // 측정 구간 장치 xrun 델타
        int    overruns = 0;     // 측정 구간 예산 초과 콜백 수

        bool hasSpike = false;   // A3: 예산 초과 블록의 잡별 타임라인 (구간 최악 1개)
        AudioWorkerPool::SpikeSnapshot spike;

        bool isStable() const noexcept
        {
            return xruns == 0 && overruns == 0 && peakLoad < 0.95;
        }
    };

    struct Report
    {
        juce::Array<StepResult> steps;
        int  maxStableChannels = 0;   // 0 = 전 단계 불안정
        bool completed = false;       // false = 중단됨
        juce::File file;              // 저장된 리포트 (없으면 File())
        juce::StringArray prepareErrors;
    };

    explicit CpuProfiler (AudioEngine& engineRef) : engine (engineRef) {}
    ~CpuProfiler() override { abort(); }

    bool isRunning() const noexcept { return running; }

    /** 프로파일 시작(메시지 스레드). 실패 시 error 채우고 false.
        조건: 활성 채널 >= 1, 트랜스포트 정지 상태. */
    bool start (juce::String& error);

    /** 진행 중이면 중단(합성 부하 해제 포함). onFinished 는 호출되지 않는다. */
    void abort();

    std::function<void (const juce::String&)> onProgress;   // 진행 문구 (메시지 스레드)
    std::function<void (const Report&)>       onFinished;   // 완주 시에만 (메시지 스레드)

    static constexpr int settleMs  = 1000;
    static constexpr int measureMs = 8000;

private:
    void timerCallback() override;
    void beginStep();
    void endRun (bool completed);   // 합성 부하 해제 + (완주 시) 리포트 저장·통지
    juce::File writeReport();
    void progress (const juce::String& text);

    AudioEngine& engine;
    juce::Array<int> stepChannels;
    Report report;

    int    stepIndex   = 0;
    bool   measuring   = false;
    bool   running     = false;
    juce::uint32 phaseStart = 0;   // Time::getMillisecondCounter 기준
    int    xrunBase    = 0;
    int    overrunBase = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CpuProfiler)
};
