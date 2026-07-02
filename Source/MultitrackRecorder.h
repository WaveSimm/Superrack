#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "Util.h"
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
/** 채널별 Dry(원음) 멀티트랙 녹음기 (Phase 2, 실시간 안전).

    설계(DESIGN 3.4):
    - 오디오 스레드는 락프리 FIFO push 만, 실제 디스크 쓰기는 Writer 스레드.
      (JUCE `AudioFormatWriter::ThreadedWriter` = 내부 락프리 FIFO + TimeSliceThread)
    - Dry tap = 채널 입력 직후(스트립 처리 전). Wet 녹음 없음.
    - 32-bit float WAV, 채널당 모노 파일: `<Documents>/Superrack/rec/YYYYMMDD_HHMMSS/chNN_dry.wav`
    - 전 트랙 동시 arm → 같은 콜백 경계에서 시작, 세션 타임스탬프 기록.
    - FIFO 오버플로 시 해당 블록 드롭 + xrun 카운터 증가(오디오 무중단).

    스레드 규약:
    - start()/stop() 는 메시지 스레드 전용.
    - writeBlock() 은 오디오 스레드 전용(무할당·무락).
    - active 플래그가 켜진 동안 writers 벡터는 불변 → 오디오 스레드는 락 없이 읽는다.
      stop() 은 active 를 끈 뒤 in-flight 콜백을 흘려보내고(짧은 대기) 파일을 닫는다.
      (ChannelStrip 의 reconfigure 가드와 동일한 패턴)
*/
class MultitrackRecorder
{
public:
    MultitrackRecorder();
    ~MultitrackRecorder();

    //==========================================================================
    // ── 메시지 스레드 ──────────────────────────────────────────────────────
    /** targetDir 에 채널 numChannels 개의 Dry 녹음 시작(기존 chNN_dry.wav 정리). 실패 시 error. */
    bool start (double sampleRate, int numChannels, const juce::File& targetDir, juce::String& error);

    /** 녹음 정지 + FIFO 플러시 + 파일 마감. */
    void stop();

    /** 마지막 세션 폴더(없으면 비어있음). */
    juce::File getLastSessionDir() const { return sessionDir; }

    /** 녹음 루트 폴더 (<Documents>/Superrack/rec). 재생기가 최근 테이크 탐색에 사용. */
    static juce::File getRecordBaseDir();

    //==========================================================================
    // ── 오디오 스레드 ──────────────────────────────────────────────────────
    /** Dry 입력 블록을 채널별 FIFO 로 push. 무할당·무락. */
    void writeBlock (const float* const* inputChannelData,
                     int numInputChannels, int numSamples) noexcept;

    //==========================================================================
    // ── GUI 폴링 (atomic) ─────────────────────────────────────────────────
    bool isRecording()      const noexcept { return active.load (std::memory_order_acquire); }
    int  getXrunCount()     const noexcept { return xruns.load (std::memory_order_relaxed); }
    /** 녹음 경과 시간(초). */
    double getRecordedSeconds() const noexcept;

private:
    juce::TimeSliceThread writerThread { "Superrack Dry Writer" };
    juce::WavAudioFormat  wavFormat;

    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> writers;

    std::atomic<bool>          active         { false };
    std::atomic<int>           xruns          { 0 };
    std::atomic<juce::int64>   samplesWritten { 0 };
    sr::CallbackFence          fence;   // R1: stop() 이 in-flight writeBlock 종료를 확인

    int    numRecChannels = 0;
    double currentSR      = 48000.0;
    juce::File sessionDir;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackRecorder)
};
