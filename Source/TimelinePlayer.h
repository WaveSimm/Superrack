#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "Util.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

//==============================================================================
/** 녹음된 Dry 스템(chNN_dry.wav)을 통합 타임라인으로 재생 (BACKLOG B 재설계).

    JUCE BufferingAudioSource 는 백그라운드 스레드가 callbackLock 을 쥔 채 디스크
    read 를 수행(우선순위 역전, 기술검토 §5) → 채널 공유 SPSC 링버퍼 + 전용 리더
    스레드로 교체. 레퍼런스: Mixxx CachingReader, Ardour butler.

    - 오디오 스레드(readBlock)는 AbstractFifo 에서 소비만 한다 — 락·디스크·할당 없음.
      언더런이면 무음 + 카운터(재생 위치는 소비량만큼만 전진 = 디스크를 기다림).
    - 리더 스레드가 prefetch. 스템 SR ≠ 장치 SR 이면 리더에서 Lagrange 리샘플
      (B-2: 피치/길이 어긋남 제거). 위치·길이는 모두 장치 샘플 도메인.
    - 시크 = epoch 핸드셰이크: setPosition 이 epoch 를 올리면 오디오는 valid 될
      때까지 무음(소비 중단 ack), 리더가 ack/타임아웃 후 FIFO 리셋·리필.
    - load()/unload()/setPosition() 은 메시지 스레드, readBlock() 은 오디오 스레드.
      리더 스레드와 메시지 스레드는 configMutex 로 직렬화(오디오 스레드는 무관).
*/
class TimelinePlayer
{
public:
    TimelinePlayer();
    ~TimelinePlayer();

    //== 메시지 스레드 ==========================================================
    /** dir 안의 chNN_dry.wav 스템들을 열고 재생 준비. deviceSampleRate 가 스템
        SR 과 다르면 리샘플 재생. 실패 시 error 채우고 false. */
    bool load (const juce::File& dir, int blockSize, double deviceSampleRate,
               juce::String& error);
    void unload();

    bool         isLoaded()       const noexcept { return loaded.load (std::memory_order_acquire); }
    int          getNumChannels() const noexcept { return numCh; }
    /** 총 길이·위치·SR 은 장치 샘플 도메인 (리샘플 반영). */
    juce::int64  getTotalSamples() const noexcept { return totalSamples; }
    double       getSampleRate()  const noexcept { return outSr; }
    double       getFileSampleRate() const noexcept { return fileSr; }

    juce::int64  getPosition() const noexcept { return playPos.load (std::memory_order_relaxed); }
    void         setPosition (juce::int64 s);  // 시크 (리필 완료까지 ≤50ms 대기)

    /** 오디오 스레드가 FIFO 를 못 채워 무음을 낸 블록 수 (GUI 표면화용). */
    int getUnderrunCount() const noexcept { return underruns.load (std::memory_order_relaxed); }

    //== 오디오 스레드 ==========================================================
    /** 내부 스크래치에 numSamples 만큼 채운다(무할당·무락). 이후 getChannel(). */
    void readBlock (int numSamples) noexcept;
    const float* getChannel (int ch) const noexcept;

private:
    void readerLoop();
    void refill();                       // 리더: FIFO 여유가 있으면 디스크→링 채움
    void handleSeek();                   // 리더: epoch 변경 처리(파킹 확인→리셋→리필)
    bool fillChunk (int outSamples);     // 리더: 청크 1개 생산 (리샘플 포함)

    // ── 설정(메시지↔리더 스레드, configMutex 보호. 오디오는 접근 안 함) ──
    std::mutex configMutex;
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    std::vector<juce::LagrangeInterpolator> interps;
    juce::AudioBuffer<float> inputTemp;  // 리샘플 입력 스크래치 (리더 전용)
    juce::int64 fileReadPos   = 0;       // 파일 샘플 도메인
    juce::int64 fileTotal     = 0;
    double      ratio         = 1.0;     // 파일 샘플 / 장치 샘플
    bool        streamActive  = false;   // 리더가 refill 해도 되는가
    bool        producedAll   = false;   // total 까지 생산 완료

    // ── 링버퍼 (오디오 = 소비자, 리더 = 생산자) ──
    juce::AbstractFifo       fifo { 8 };
    juce::AudioBuffer<float> ring;

    // ── 시크 핸드셰이크 ──
    std::atomic<juce::uint32> epoch      { 0 };   // 메시지: 시크마다 +1
    std::atomic<juce::uint32> validEpoch { 0 };   // 리더: 리필 완료 표식
    std::atomic<juce::uint32> ackEpoch   { 0 };   // 오디오: 마지막으로 본 epoch
    juce::int64               pendingSeek = 0;    // configMutex 보호

    // ── 스레드 ──
    std::thread         readerThread;
    juce::WaitableEvent wakeReader;
    std::atomic<bool>   shouldExit { false };

    juce::AudioFormatManager formatMgr;
    juce::AudioBuffer<float> scratch;             // 오디오 스레드 출력용

    std::atomic<bool>        loaded { false };
    sr::CallbackFence        fence;               // R1: unload 가 in-flight readBlock 확인
    std::atomic<juce::int64> playPos { 0 };       // 장치 샘플 도메인
    std::atomic<int>         underruns { 0 };
    std::atomic<bool>        eofReached { false };

    int         numCh = 0;
    double      outSr = 48000.0, fileSr = 48000.0;
    juce::int64 totalSamples = 0;                 // 장치 샘플 도메인

    static constexpr int chunkSamples = 8192;     // 리필 단위 (출력 도메인)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelinePlayer)
};
