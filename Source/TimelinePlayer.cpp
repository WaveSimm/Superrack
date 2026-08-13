#include "TimelinePlayer.h"

using sr::u8;

//==============================================================================
TimelinePlayer::TimelinePlayer()
{
    formatMgr.registerBasicFormats();
    readerThread = std::thread ([this] { readerLoop(); });
}

TimelinePlayer::~TimelinePlayer()
{
    unload();
    shouldExit.store (true, std::memory_order_release);
    wakeReader.signal();
    if (readerThread.joinable())
        readerThread.join();
}

//==============================================================================
bool TimelinePlayer::load (const juce::File& dir, int blockSize, double deviceSampleRate,
                           juce::String& error)
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

    std::vector<std::unique_ptr<juce::AudioFormatReader>> newReaders;
    for (auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> r (formatMgr.createReaderFor (f));
        if (r == nullptr)
        {
            error = u8 ("스템을 열 수 없습니다: ") + f.getFileName();
            return false;
        }
        newReaders.push_back (std::move (r));
    }

    {
        const std::lock_guard<std::mutex> lock (configMutex);

        readers = std::move (newReaders);
        numCh   = (int) readers.size();

        fileTotal = 0;
        for (auto& r : readers)
            fileTotal = juce::jmax (fileTotal, (juce::int64) r->lengthInSamples);

        fileSr = readers[0]->sampleRate;
        if (fileSr <= 0.0)
            fileSr = 48000.0;
        outSr = deviceSampleRate > 0.0 ? deviceSampleRate : fileSr;
        ratio = fileSr / outSr;
        totalSamples = (juce::int64) std::llround ((double) fileTotal / ratio);

        // 링 = 장치 SR 기준 ~3초 (32ch x 96k x 3s x 4B ≈ 37MB 상한)
        const int ringSize = (int) juce::jmax (outSr * 3.0, (double) chunkSamples * 4);
        ring.setSize (numCh, ringSize, false, false, true);
        fifo.setTotalSize (ringSize);
        fifo.reset();

        interps.resize ((size_t) numCh);
        for (auto& ip : interps)
            ip.reset();

        // 리샘플 입력 스크래치: 청크당 필요 입력 + 여유
        inputTemp.setSize (numCh, (int) std::ceil (chunkSamples * juce::jmax (ratio, 1.0)) + 16,
                           false, false, true);

        fileReadPos  = 0;
        producedPos  = 0;
        producedAll  = false;
        streamActive = true;
    }

    scratch.setSize (juce::jmax (1, numCh), juce::jmax (1, blockSize), false, false, true);
    scratch.clear();

    eofReached.store (false, std::memory_order_relaxed);
    playPos.store (0, std::memory_order_relaxed);
    underruns.store (0, std::memory_order_relaxed);

    // 위치 0 기준 초기 리필 (setPosition 과 동일한 시크 경로 재사용)
    loaded.store (true, std::memory_order_release);
    setPosition (0);
    return true;
}

void TimelinePlayer::unload()
{
    loaded.store (false, std::memory_order_release);
    // R1: in-flight readBlock 종료 확인 (재생 중이 아니면 세대가 안 전진 → 타임아웃 복귀)
    fence.waitForIdle (20);

    const std::lock_guard<std::mutex> lock (configMutex);   // 리더 refill 과 직렬화
    streamActive = false;
    readers.clear();
    interps.clear();
    fifo.reset();
    numCh = 0;
    fileTotal = 0;
    totalSamples = 0;
}

void TimelinePlayer::setPosition (juce::int64 s)
{
    if (! loaded.load (std::memory_order_acquire))
        return;

    s = juce::jlimit ((juce::int64) 0, totalSamples, s);

    {
        const std::lock_guard<std::mutex> lock (configMutex);
        pendingSeek = s;
    }
    epoch.fetch_add (1, std::memory_order_release);   // 오디오 소비 중단 신호
    wakeReader.signal();

    playPos.store (s, std::memory_order_relaxed);
    eofReached.store (false, std::memory_order_relaxed);

    // 리더의 리셋+첫 청크 리필 완료까지 잠깐 대기 → 재생 시작이 무음 없이 붙는다.
    const auto target = epoch.load (std::memory_order_relaxed);
    const auto t0 = juce::Time::getMillisecondCounter();
    while (validEpoch.load (std::memory_order_acquire) != target
           && (int) (juce::Time::getMillisecondCounter() - t0) < 50)
        juce::Thread::sleep (1);
}

//==============================================================================
// 오디오 스레드 — FIFO 소비만. 락·디스크·할당 없음.
void TimelinePlayer::setLoop (bool enabled, juce::int64 startSample, juce::int64 endSample)
{
    const auto total = totalSamples;
    auto s = juce::jlimit ((juce::int64) 0, total, startSample);
    auto e = juce::jlimit ((juce::int64) 0, total, endSample);
    const bool ok = enabled && e > s;

    // 경계를 먼저 싣고 플래그를 나중에 켜야, 오디오 스레드가 반쯤 갱신된 구간을
    // 보고 엉뚱한 위치로 되감지 않는다(끌 때는 반대 순서).
    if (! ok)
    {
        loopOn.store (false, std::memory_order_release);
        loopStart.store (s, std::memory_order_relaxed);
        loopEnd.store   (e, std::memory_order_relaxed);
        return;
    }

    loopStart.store (s, std::memory_order_relaxed);
    loopEnd.store   (e, std::memory_order_relaxed);
    loopOn.store    (true, std::memory_order_release);
}

void TimelinePlayer::readBlock (int numSamples) noexcept
{
    fence.bump();   // unload 가드용 콜백 세대 기록

    const int want = juce::jmin (numSamples, scratch.getNumSamples());

    const auto e = epoch.load (std::memory_order_acquire);
    ackEpoch.store (e, std::memory_order_release);   // 시크 핸드셰이크: 이 epoch 를 봤음

    if (! loaded.load (std::memory_order_acquire)
        || validEpoch.load (std::memory_order_acquire) != e)
    {
        scratch.clear();   // 시크 리필 중 — 무음
        return;
    }

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (want, start1, size1, start2, size2);
    const int avail = size1 + size2;

    for (int ch = 0; ch < scratch.getNumChannels(); ++ch)
    {
        float* dest = scratch.getWritePointer (ch);
        if (size1 > 0) juce::FloatVectorOperations::copy (dest,         ring.getReadPointer (ch, start1), size1);
        if (size2 > 0) juce::FloatVectorOperations::copy (dest + size1, ring.getReadPointer (ch, start2), size2);
        if (avail < want)
            juce::FloatVectorOperations::clear (dest + avail, want - avail);
    }
    fifo.finishedRead (avail);

    if (avail < want)
    {
        if (eofReached.load (std::memory_order_relaxed))
        {
            playPos.store (totalSamples, std::memory_order_relaxed);   // 끝 도달 → 정지 유도
            return;
        }
        underruns.fetch_add (1, std::memory_order_relaxed);   // 디스크 지연 — 무음으로 대기
    }

    // 생산 측(fillChunk)이 loopEnd 에서 loopStart 로 되감으므로 소비 위치도 같은
    // 규칙으로 되감아야 표시 위치와 실제 오디오가 어긋나지 않는다.
    auto pos = playPos.load (std::memory_order_relaxed) + avail;
    if (loopOn.load (std::memory_order_relaxed))
    {
        const auto ls = loopStart.load (std::memory_order_relaxed);
        const auto le = loopEnd.load   (std::memory_order_relaxed);
        if (le > ls && pos >= le)
            pos = ls + (pos - le) % (le - ls);
    }
    playPos.store (pos, std::memory_order_relaxed);

    wakeReader.signal();   // 리필 촉구 (SetEvent ~1µs)
}

const float* TimelinePlayer::getChannel (int ch) const noexcept
{
    if (ch >= 0 && ch < scratch.getNumChannels())
        return scratch.getReadPointer (ch);
    return nullptr;
}

//==============================================================================
// 리더 스레드 — 디스크 prefetch + 시크 처리. RT 아님(normal priority).
void TimelinePlayer::readerLoop()
{
    while (! shouldExit.load (std::memory_order_acquire))
    {
        if (epoch.load (std::memory_order_acquire) != validEpoch.load (std::memory_order_relaxed))
            handleSeek();
        else
            refill();

        wakeReader.wait (5);   // 신호 또는 5ms 폴링
    }
}

void TimelinePlayer::handleSeek()
{
    const auto e = epoch.load (std::memory_order_acquire);

    // 오디오가 이 epoch 를 보고 소비를 멈췄음을 확인(ack) — 재생 중이 아니면
    // ack 가 안 오므로 타임아웃(30ms > 최대 블록시간) 후 진행해도 안전하다.
    const auto t0 = juce::Time::getMillisecondCounter();
    while (ackEpoch.load (std::memory_order_acquire) != e
           && (int) (juce::Time::getMillisecondCounter() - t0) < 30
           && ! shouldExit.load (std::memory_order_relaxed))
        juce::Thread::sleep (1);

    {
        const std::lock_guard<std::mutex> lock (configMutex);
        if (! streamActive)
        {
            validEpoch.store (e, std::memory_order_release);
            return;
        }

        fifo.reset();                       // 소비자 파킹 확인 후라 안전
        for (auto& ip : interps)
            ip.reset();
        fileReadPos = (juce::int64) std::llround ((double) pendingSeek * ratio);
        producedPos = pendingSeek;
        producedAll = false;

        // 첫 청크 즉시 리필 — setPosition 대기가 이걸 기다린다.
        fillChunk (chunkSamples);
    }

    // epoch 가 그 사이 또 올랐으면 다음 루프에서 재처리.
    if (epoch.load (std::memory_order_acquire) == e)
        validEpoch.store (e, std::memory_order_release);
}

void TimelinePlayer::refill()
{
    const std::lock_guard<std::mutex> lock (configMutex);
    if (! streamActive || producedAll)
        return;

    while (fifo.getFreeSpace() >= chunkSamples)
        if (! fillChunk (chunkSamples))
            break;
}

// 출력 도메인 outSamples 만큼 생산해 FIFO 에 쓴다. configMutex 보유 상태로 호출.
bool TimelinePlayer::fillChunk (int outSamples)
{
    if (producedAll || readers.empty())
        return false;

    // 남은 생산량 (출력 도메인). 반복 중이면 상한이 총길이가 아니라 loopEnd 다.
    const bool        loop  = loopOn.load (std::memory_order_relaxed);
    const juce::int64 ls    = loopStart.load (std::memory_order_relaxed);
    const juce::int64 le    = loopEnd.load   (std::memory_order_relaxed);
    const bool        loopOk = loop && le > ls;
    const juce::int64 limit = loopOk ? juce::jmin (le, totalSamples) : totalSamples;

    const int n = (int) juce::jmin ((juce::int64) outSamples, limit - producedPos);
    if (n <= 0)
    {
        if (loopOk)
        {
            // 구간 끝 도달 — 시크(핸드셰이크·FIFO 리셋) 없이 읽기 위치만 되감는다.
            // 오디오 스레드도 같은 경계로 playPos 를 되감으므로 위치가 유지된다.
            producedPos = ls;
            fileReadPos = (juce::int64) std::llround ((double) ls * ratio);
            for (auto& ip : interps)
                ip.reset();   // 불연속 지점 — 보간 상태를 끌고 가면 클릭이 난다
            return true;      // 다음 호출에서 실제 생산
        }

        producedAll = true;
        eofReached.store (true, std::memory_order_release);
        return false;
    }

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (n, start1, size1, start2, size2);
    if (size1 + size2 < n)
        return false;   // 여유 부족 — 다음 루프에서

    const bool resample = std::abs (ratio - 1.0) > 1.0e-9;
    const int  needIn   = resample ? juce::jmin (inputTemp.getNumSamples(),
                                                 (int) std::ceil (n * ratio) + 8)
                                   : n;

    juce::int64 usedIn = 0;
    for (int ch = 0; ch < numCh; ++ch)
    {
        auto* rdr = readers[(size_t) ch].get();

        if (! resample)
        {
            // 1:1 — 링에 직접 read (AudioFormatReader 가 EOF 이후는 0 채움)
            float* d1 = ring.getWritePointer (ch, start1);
            if (size1 > 0) rdr->read (&d1, 1, fileReadPos, size1);
            float* d2 = ring.getWritePointer (ch, start2);
            if (size2 > 0) rdr->read (&d2, 1, fileReadPos + size1, size2);
            usedIn = n;
        }
        else
        {
            // 리샘플 — 입력 스크래치로 읽고 Lagrange 로 n 출력 생산.
            float* in = inputTemp.getWritePointer (ch);
            rdr->read (&in, 1, fileReadPos, needIn);

            float tmp[chunkSamples];   // 스택 32KB — 리더 스레드라 무방
            const int used = interps[(size_t) ch].process (ratio, in, tmp, n);
            usedIn = used;   // 전 채널 동일 (같은 ratio·같은 n)

            if (size1 > 0) juce::FloatVectorOperations::copy (ring.getWritePointer (ch, start1), tmp,         size1);
            if (size2 > 0) juce::FloatVectorOperations::copy (ring.getWritePointer (ch, start2), tmp + size1, size2);
        }
    }

    fifo.finishedWrite (n);
    fileReadPos += usedIn;
    producedPos += n;
    return true;
}
