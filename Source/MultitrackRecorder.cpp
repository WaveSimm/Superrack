#include "MultitrackRecorder.h"

//==============================================================================
MultitrackRecorder::MultitrackRecorder()
{
    // 백그라운드 디스크 쓰기 스레드. 오디오 스레드보다 낮은 우선순위.
    writerThread.startThread (juce::Thread::Priority::normal);
}

MultitrackRecorder::~MultitrackRecorder()
{
    stop();
    writerThread.stopThread (2000);
}

juce::File MultitrackRecorder::getRecordBaseDir()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Superrack")
               .getChildFile ("rec");
}

//==============================================================================
bool MultitrackRecorder::start (double sampleRate, int numChannels, const juce::File& targetDir, juce::String& error)
{
    if (active.load (std::memory_order_acquire))
        return true;   // 이미 녹음 중

    if (sampleRate <= 0.0 || numChannels <= 0)
    {
        error = juce::String (juce::CharPointer_UTF8 ("장치가 준비되지 않았습니다."));
        return false;
    }

    // 지정 폴더(스크래치). 기존 chNN_dry.wav 정리 후 새로 씀.
    auto dir = targetDir;
    if (! dir.createDirectory())
    {
        error = juce::String (juce::CharPointer_UTF8 ("녹음 폴더 생성 실패: ")) + dir.getFullPathName();
        return false;
    }
    {
        juce::Array<juce::File> old;
        dir.findChildFiles (old, juce::File::findFiles, false, "ch*_dry.wav");
        for (auto& f : old)
            f.deleteFile();
    }

    // 채널별 모노 32-bit float WAV writer + ThreadedWriter(락프리 FIFO) 생성.
    const int fifoSamples = (int) juce::jmax (sampleRate * 3.0, 48000.0);   // ~3초 버퍼

    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> built;
    built.reserve ((size_t) numChannels);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto file = dir.getChildFile ("ch" + juce::String (ch + 1).paddedLeft ('0', 2) + "_dry.wav");

        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
        {
            error = juce::String (juce::CharPointer_UTF8 ("파일 열기 실패: ")) + file.getFullPathName();
            return false;   // built 은 스코프 종료 시 정리
        }

        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate (sampleRate)
                                 .withNumChannels (1)
                                 .withBitsPerSample (32)
                                 .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

        std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (stream, options));
        if (writer == nullptr)
        {
            error = juce::String (juce::CharPointer_UTF8 ("WAV writer 생성 실패 (ch")) + juce::String (ch + 1) + ")";
            return false;
        }

        built.push_back (std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
            writer.release(), writerThread, fifoSamples));
    }

    // 오디오 스레드가 관측하기 전에 상태를 모두 확정한다.
    writers        = std::move (built);
    numRecChannels = numChannels;
    currentSR      = sampleRate;
    sessionDir     = dir;
    xruns.store (0, std::memory_order_relaxed);
    samplesWritten.store (0, std::memory_order_relaxed);

    active.store (true, std::memory_order_release);   // 이 시점부터 writeBlock 활성
    return true;
}

void MultitrackRecorder::stop()
{
    if (! active.load (std::memory_order_acquire))
        return;

    active.store (false, std::memory_order_release);

    // in-flight 오디오 콜백이 active=false 를 관측하고 빠져나갈 시간을 준다.
    // (ChannelStrip::reconfigure 와 동일한 가드 — 이후 writers 접근 없음)
    juce::Thread::sleep (30);

    writers.clear();   // ThreadedWriter 소멸 → FIFO 플러시 + WAV 헤더 마감
    numRecChannels = 0;
}

//==============================================================================
void MultitrackRecorder::writeBlock (const float* const* inputChannelData,
                                     int numInputChannels, int numSamples) noexcept
{
    if (! active.load (std::memory_order_acquire))
        return;

    const int n = juce::jmin (numRecChannels, numInputChannels);
    bool overflow = false;

    for (int ch = 0; ch < n; ++ch)
    {
        const float* p = inputChannelData[ch];
        auto* w = writers[(size_t) ch].get();

        if (p != nullptr && w != nullptr)
        {
            // ThreadedWriter::write 는 락프리 — FIFO 가 꽉 차면 false(블록 드롭).
            if (! w->write (&p, numSamples))
                overflow = true;
        }
    }

    if (overflow)
        xruns.fetch_add (1, std::memory_order_relaxed);

    samplesWritten.fetch_add (numSamples, std::memory_order_relaxed);
}

double MultitrackRecorder::getRecordedSeconds() const noexcept
{
    const auto s = samplesWritten.load (std::memory_order_relaxed);
    return currentSR > 0.0 ? (double) s / currentSR : 0.0;
}
