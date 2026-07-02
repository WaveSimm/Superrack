#include "TimelinePlayer.h"
#include "Util.h"

using sr::u8;

//==============================================================================
/** N개의 모노 리더를 같은 위치에서 읽어 다채널 버퍼로 내보내는 소스. */
class StemSource : public juce::PositionableAudioSource
{
public:
    explicit StemSource (std::vector<std::unique_ptr<juce::AudioFormatReader>> rdrs)
        : readers (std::move (rdrs))
    {
        for (auto& r : readers)
            total = juce::jmax (total, r != nullptr ? (juce::int64) r->lengthInSamples : (juce::int64) 0);
    }

    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
        {
            float* dest = info.buffer->getWritePointer (ch, info.startSample);
            if (ch < (int) readers.size() && readers[(size_t) ch] != nullptr)
                readers[(size_t) ch]->read (&dest, 1, pos, info.numSamples);   // 모노, 끝 넘으면 0 채움
            else
                juce::FloatVectorOperations::clear (dest, info.numSamples);
        }
        pos += info.numSamples;
    }

    void        setNextReadPosition (juce::int64 p) override { pos = p; }
    juce::int64 getNextReadPosition() const override        { return pos; }
    juce::int64 getTotalLength() const override             { return total; }
    bool        isLooping() const override                  { return false; }

    int getNumChannels() const noexcept { return (int) readers.size(); }
    double getReaderSampleRate() const noexcept
    {
        return (! readers.empty() && readers[0] != nullptr) ? readers[0]->sampleRate : 0.0;
    }

private:
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    juce::int64 pos = 0, total = 0;
};

//==============================================================================
TimelinePlayer::TimelinePlayer()
{
    formatMgr.registerBasicFormats();
    readThread.startThread (juce::Thread::Priority::normal);
}

TimelinePlayer::~TimelinePlayer()
{
    unload();
    readThread.stopThread (2000);
}

bool TimelinePlayer::load (const juce::File& dir, int blockSize, juce::String& error)
{
    unload();

    if (! dir.isDirectory())
    {
        error = u8 ("재생할 녹음 폴더가 없습니다.");
        return false;
    }

    juce::Array<juce::File> files;
    dir.findChildFiles (files, juce::File::findFiles, false, "ch*_dry.wav");
    files.sort();   // ch01, ch02, ...

    if (files.isEmpty())
    {
        error = u8 ("스템 파일(chNN_dry.wav)이 없습니다: ") + dir.getFullPathName();
        return false;
    }

    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    for (auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> r (formatMgr.createReaderFor (f));
        if (r == nullptr)
        {
            error = u8 ("스템을 열 수 없습니다: ") + f.getFileName();
            return false;
        }
        readers.push_back (std::move (r));
    }

    auto stem = std::make_unique<StemSource> (std::move (readers));
    numCh        = stem->getNumChannels();
    totalSamples = stem->getTotalLength();
    sr           = stem->getReaderSampleRate();
    if (sr <= 0.0)
        sr = 48000.0;

    const int fifo = (int) juce::jmax (sr * 3.0, 48000.0);   // ~3초 read-ahead
    buffering = std::make_unique<juce::BufferingAudioSource> (
        stem.release(), readThread, true /*delete source*/, fifo, numCh, true);

    scratch.setSize (juce::jmax (1, numCh), juce::jmax (1, blockSize), false, false, true);

    buffering->prepareToPlay (blockSize, sr);
    buffering->setNextReadPosition (0);

    loaded.store (true, std::memory_order_release);
    return true;
}

void TimelinePlayer::unload()
{
    loaded.store (false, std::memory_order_release);
    juce::Thread::sleep (20);   // in-flight readBlock 통과 대기 (오디오 스레드 접근 차단)

    if (buffering != nullptr)
    {
        buffering->releaseResources();
        buffering.reset();
    }
    numCh = 0;
    totalSamples = 0;
}

juce::int64 TimelinePlayer::getPosition() const
{
    return buffering != nullptr ? buffering->getNextReadPosition() : 0;
}

void TimelinePlayer::setPosition (juce::int64 s)
{
    if (buffering != nullptr)
        buffering->setNextReadPosition (juce::jlimit ((juce::int64) 0, totalSamples, s));
}

//==============================================================================
void TimelinePlayer::readBlock (int numSamples) noexcept
{
    if (! loaded.load (std::memory_order_acquire) || buffering == nullptr)
    {
        scratch.clear();
        return;
    }

    juce::AudioSourceChannelInfo info (&scratch, 0, juce::jmin (numSamples, scratch.getNumSamples()));
    buffering->getNextAudioBlock (info);
}

const float* TimelinePlayer::getChannel (int ch) const noexcept
{
    if (ch >= 0 && ch < scratch.getNumChannels())
        return scratch.getReadPointer (ch);
    return nullptr;
}
