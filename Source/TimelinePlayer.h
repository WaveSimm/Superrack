#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "Util.h"
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
/** 녹음된 Dry 스템(chNN_dry.wav)을 통합 타임라인으로 재생 (Phase: 재생).

    - 모든 채널을 같은 read 위치로 동기 재생(오프셋 없음).
    - 디스크 읽기는 백그라운드 스레드(BufferingAudioSource 의 read-ahead)에서,
      오디오 스레드는 미리 채워진 버퍼에서만 읽는다.
    - load()/unload()/setPosition() 은 메시지 스레드, readBlock() 은 오디오 스레드.
*/
class TimelinePlayer
{
public:
    TimelinePlayer();
    ~TimelinePlayer();

    //== 메시지 스레드 ==========================================================
    /** dir 안의 chNN_dry.wav 스템들을 열고 재생 준비. 실패 시 error 채우고 false. */
    bool load (const juce::File& dir, int blockSize, juce::String& error);
    void unload();

    bool         isLoaded()       const noexcept { return loaded; }
    int          getNumChannels() const noexcept { return numCh; }
    juce::int64  getTotalSamples() const noexcept { return totalSamples; }
    double       getSampleRate()  const noexcept { return sr; }

    juce::int64  getPosition() const;          // 현재 재생 위치(샘플)
    void         setPosition (juce::int64 s);  // 시크

    //== 오디오 스레드 ==========================================================
    /** 내부 스크래치에 numSamples 만큼 채운다(무할당). 이후 getChannel() 로 접근. */
    void readBlock (int numSamples) noexcept;
    const float* getChannel (int ch) const noexcept;

private:
    juce::TimeSliceThread   readThread { "Superrack Stem Reader" };
    juce::AudioFormatManager formatMgr;

    std::unique_ptr<juce::BufferingAudioSource> buffering;   // StemSource 를 소유·삭제
    juce::AudioBuffer<float> scratch;

    std::atomic<bool> loaded { false };
    sr::CallbackFence fence;   // R1: unload() 가 in-flight readBlock 종료를 확인
    int         numCh = 0;
    double      sr = 48000.0;
    juce::int64 totalSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelinePlayer)
};
