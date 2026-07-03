#include "AudioEngine.h"
#include "Util.h"
#include <cmath>

using sr::u8;

//==============================================================================
AudioEngine::AudioEngine()
{
    for (auto& p : inputPeaks)
        p.store (0.0f, std::memory_order_relaxed);

    // JUCE 8.0.11: addDefaultFormats() 는 삭제됨 → 자유 함수 사용. VST3 등록.
    juce::addDefaultFormatsToManager (formatManager);

    // strips 는 maxChannels 개를 미리 생성한다(주소 안정 → 오디오/GUI 리사이즈 경쟁 없음).
    // 실제 처리/표시 채널 수는 getActiveChannelCount() 로 제한한다.
    strips.reserve (maxChannels);
    for (int i = 0; i < maxChannels; ++i)
        strips.push_back (std::make_unique<ChannelStrip> (formatManager, scanCache));
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

juce::File AudioEngine::getSettingsFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Superrack")
               .getChildFile ("audio-settings.xml");
}

void AudioEngine::initialise()
{
    // 저장된 장치 설정 복원 — 없으면 기본 장치로.
    std::unique_ptr<juce::XmlElement> savedState;
    const auto file = getSettingsFile();
    if (file.existsAsFile())
        savedState = juce::parseXML (file);

    deviceManager.initialise (2, 2, savedState.get(), true);
    deviceManager.addAudioCallback (this);     // 여기서 audioDeviceAboutToStart → strips 에 sr/bs 설정됨
    deviceManager.addChangeListener (this);    // 설정 변경 시 저장

    lastChannelCount = getActiveChannelCount();

    // 자동 저장된 세션(플러그인 체인+게인) 복원. strips 는 위에서 sr/bs 준비됨.
    const auto sessionFile = getSessionFile();
    if (sessionFile.existsAsFile())
    {
        juce::StringArray errors;
        loadSessionFromFile (sessionFile, errors);
        // 복원 실패 항목은 무시(경로 이동/미설치 플러그인 등). GUI 는 로드된 것만 표시.
    }

    // 가장 최근 테이크를 현재 테이크로 (재시작 후에도 재생 가능).
    if (auto takes = takeMgr.listTakes(); ! takes.isEmpty())
    {
        currentTakeDir = takes.getReference (0).dir;
        refreshTakeLength();
    }
}

void AudioEngine::shutdown()
{
    transport.store (tsStopped, std::memory_order_release);
    player.unload();
    recorder.stop();                 // 진행 중 녹음 플러시/마감
    autoSaveSession();               // 종료 시 최종 플러그인 상태 캡처
    saveSettings();
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

//==============================================================================
bool AudioEngine::startRecording (juce::String& error)
{
    return recorder.start (getCurrentSampleRate(), getActiveChannelCount(), getRecTmpDir(), error);
}

void AudioEngine::stopRecording()
{
    recorder.stop();
}

//==============================================================================
juce::File AudioEngine::getRecTmpDir()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("Superrack")
               .getChildFile (".rectmp");
}

void AudioEngine::refreshTakeLength()
{
    currentTakeLengthSeconds = currentTakeDir.isDirectory()
                                   ? takeMgr.readTake (currentTakeDir).lengthSeconds() : 0.0;
}

TakeManager::TakeEnv AudioEngine::currentEnv()
{
    TakeManager::TakeEnv env;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        env.deviceName = dev->getName();
        env.sampleRate = dev->getCurrentSampleRate();
        env.bufferSize = dev->getCurrentBufferSizeSamples();
    }
    env.channels = getActiveChannelCount();
    return env;
}

void AudioEngine::ensureCurrentTake()
{
    if (currentTakeDir.isDirectory() && currentTakeDir.getChildFile ("timeline.json").existsAsFile())
        return;
    currentTakeDir = takeMgr.createTake (currentEnv(), getSessionVar());
    refreshTakeLength();
    if (onTakesChanged != nullptr)
        onTakesChanged();
}

void AudioEngine::newTake()
{
    transportStop();
    currentTakeDir = takeMgr.createTake (currentEnv(), getSessionVar());
    player.unload();
    playheadSeconds = 0.0;
    refreshTakeLength();
    if (onTakesChanged != nullptr)
        onTakesChanged();
}

void AudioEngine::loadTake (const juce::File& takeDir, juce::String& warning)
{
    transportStop();

    const auto info = takeMgr.readTake (takeDir);
    if (! info.isValid())
    {
        warning = u8 ("테이크를 읽을 수 없습니다.");
        return;
    }

    currentTakeDir = takeDir;
    player.unload();
    playheadSeconds = 0.0;
    refreshTakeLength();

    // 세션 스냅샷 자동복원 (플러그인 체인·게인·이름)
    const auto sess = takeMgr.readSession (takeDir);
    if (sess.isObject())
    {
        juce::StringArray errs;
        applySessionVar (sess, errs);
    }

    // 환경 불일치 경고 (장치는 자동 변경하지 않음)
    const double curSr = getCurrentSampleRate();
    if (info.env.sampleRate > 0.0 && curSr > 0.0
        && std::abs (info.env.sampleRate - curSr) > 1.0)
    {
        warning = juce::String (juce::CharPointer_UTF8 ("환경 불일치: 이 테이크는 "))
                    + juce::String (info.env.sampleRate / 1000.0, 1) + "kHz"
                    + juce::String (juce::CharPointer_UTF8 (" 로 녹음됨 (현재 장치 "))
                    + juce::String (curSr / 1000.0, 1) + juce::String (juce::CharPointer_UTF8 ("kHz) — 재생은 장치 SR 로 리샘플됩니다. 펀치인 녹음은 SR 을 맞춘 뒤 하세요."));
    }

    if (onTakesChanged != nullptr)
        onTakesChanged();
}

void AudioEngine::deleteTake (const juce::File& takeDir)
{
    if (! takeDir.isDirectory())
        return;

    const bool isCurrent = (takeDir == currentTakeDir);
    if (isCurrent)
    {
        transportStop();
        player.unload();        // 리더 해제 후 삭제
    }

    takeDir.deleteRecursively();

    if (isCurrent)
    {
        currentTakeDir = juce::File();
        playheadSeconds = 0.0;
        if (auto takes = takeMgr.listTakes(); ! takes.isEmpty())
            currentTakeDir = takes.getReference (0).dir;   // 최근 테이크로 전환(세션은 유지)
        refreshTakeLength();
    }

    if (onTakesChanged != nullptr)
        onTakesChanged();
}

bool AudioEngine::transportRecord (juce::String& error)
{
    if (transport.load (std::memory_order_acquire) == tsPlaying)
        transportStop();

    ensureCurrentTake();   // 현재 테이크 없으면 새로 생성(세션·환경 스냅샷)

    // 펀치인 = 현재 테이크에 이미 녹음이 있고 playhead>0 일 때. 아니면 0(처음부터).
    const bool takeHasAudio = takeMgr.readTake (currentTakeDir).lengthSamples > 0;
    punchInSeconds = takeHasAudio ? playheadSeconds : 0.0;

    if (! startRecording (error))
    {
        punchInSeconds = 0.0;
        return false;
    }

    transport.store (tsRecording, std::memory_order_release);
    return true;
}

bool AudioEngine::transportPlay (juce::String& error)
{
    if (transport.load (std::memory_order_acquire) == tsRecording)
        transportStop();

    if (! player.isLoaded())
    {
        auto* dev = deviceManager.getCurrentAudioDevice();
        if (dev == nullptr)
        {
            error = u8 ("오디오 장치가 없습니다.");
            return false;
        }
        if (! currentTakeDir.isDirectory())
        {
            error = u8 ("재생할 테이크가 없습니다. 먼저 녹음하세요.");
            return false;
        }
        if (! player.load (currentTakeDir, dev->getCurrentBufferSizeSamples(),
                           dev->getCurrentSampleRate(), error))
            return false;
    }

    // playhead 부터 재생 (끝이면 처음부터).
    juce::int64 startPos = (juce::int64) (playheadSeconds * player.getSampleRate());
    if (startPos >= player.getTotalSamples())
        startPos = 0;
    player.setPosition (startPos);

    transport.store (tsPlaying, std::memory_order_release);
    return true;
}

void AudioEngine::transportStop()
{
    const int prev = transport.exchange (tsStopped, std::memory_order_acq_rel);

    if (prev == tsPlaying)
    {
        // 재생 정지: 멈춘 위치를 playhead 로 유지.
        if (player.isLoaded() && player.getSampleRate() > 0.0)
            playheadSeconds = player.getPosition() / player.getSampleRate();
        return;
    }

    if (prev != tsRecording)
        return;

    recorder.stop();
    const double recLen = recorder.getRecordedSeconds();

    player.unload();   // 커밋 전 테이크 파일에 대한 리더 해제

    // 스크래치 녹음을 현재 테이크에 제자리 커밋(펀치: 기존 head + 새 녹음) + 이력·세션 스냅샷.
    const double sr = getCurrentSampleRate() > 0.0 ? getCurrentSampleRate() : 48000.0;
    takeMgr.commitRecording (currentTakeDir, getRecTmpDir(),
                             (juce::int64) (punchInSeconds * sr), currentEnv(), getSessionVar());

    // 정지 위치 = 방금 녹음 끝. 그 자리에서 이어 녹음/재생 가능하게 테이크를 로드해 둔다.
    playheadSeconds = punchInSeconds + recLen;
    punchInSeconds = 0.0;

    refreshTakeLength();

    if (auto* dev = deviceManager.getCurrentAudioDevice(); dev != nullptr && currentTakeDir.isDirectory())
    {
        juce::String e;
        if (player.load (currentTakeDir, dev->getCurrentBufferSizeSamples(),
                         dev->getCurrentSampleRate(), e))
            player.setPosition ((juce::int64) (playheadSeconds * player.getSampleRate()));
        else
            player.unload();
    }

    if (onTakesChanged != nullptr)
        onTakesChanged();
}

void AudioEngine::transportRewind()
{
    playheadSeconds = 0.0;
    if (player.isLoaded())
        player.setPosition (0);
}

void AudioEngine::transportToEnd()
{
    playheadSeconds = getTimelineLengthSeconds();
    if (player.isLoaded())
        player.setPosition (player.getTotalSamples());
}

void AudioEngine::setPlayPositionSeconds (double s)
{
    playheadSeconds = juce::jmax (0.0, s);
    if (player.isLoaded())
        player.setPosition ((juce::int64) (playheadSeconds * player.getSampleRate()));
}

void AudioEngine::syncPlayheadFromPlayer() noexcept
{
    if (player.isLoaded() && player.getSampleRate() > 0.0)
        playheadSeconds = player.getPosition() / player.getSampleRate();
}

double AudioEngine::getTimelineSeconds() const
{
    switch (transport.load (std::memory_order_acquire))
    {
        case tsRecording: return punchInSeconds + recorder.getRecordedSeconds();   // 펀치인 위치부터
        case tsPlaying:   return player.getSampleRate() > 0.0 ? player.getPosition() / player.getSampleRate() : 0.0;
        default:          return playheadSeconds;   // 정지: 유지된 위치
    }
}

double AudioEngine::getTimelineLengthSeconds() const
{
    const double takeLen = player.isLoaded() && player.getSampleRate() > 0.0
                               ? player.getTotalSamples() / player.getSampleRate()
                               : currentTakeLengthSeconds;

    if (transport.load (std::memory_order_acquire) == tsRecording)
        return juce::jmax (takeLen, punchInSeconds + recorder.getRecordedSeconds());
    return takeLen;
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster*)
{
    saveSettings();

    // 활성 채널 수가 바뀌면 GUI 에 알린다(이 콜백은 메시지 스레드).
    const int n = getActiveChannelCount();
    if (n != lastChannelCount)
    {
        lastChannelCount = n;
        if (onChannelCountChanged != nullptr)
            onChannelCountChanged();
    }
}

int AudioEngine::getActiveChannelCount() const noexcept
{
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        const int ins  = dev->getActiveInputChannels().countNumberOfSetBits();
        const int outs = dev->getActiveOutputChannels().countNumberOfSetBits();
        return juce::jlimit (0, maxChannels, juce::jmin (ins, outs));   // 1:1 → 입출력 min
    }
    return 0;
}

//==============================================================================
juce::File AudioEngine::getSessionFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Superrack")
               .getChildFile ("session.json");
}

juce::var AudioEngine::getSessionVar()
{
    auto* root = new juce::DynamicObject();

    auto* dev = new juce::DynamicObject();
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        dev->setProperty ("name",       d->getName());
        dev->setProperty ("sampleRate", d->getCurrentSampleRate());
        dev->setProperty ("bufferSize", d->getCurrentBufferSizeSamples());
    }
    root->setProperty ("device", juce::var (dev));

    const int n = getActiveChannelCount();
    root->setProperty ("channels", n);

    juce::Array<juce::var> stripArr;
    for (int ch = 0; ch < n && ch < (int) strips.size(); ++ch)
    {
        auto v = strips[(size_t) ch]->getStateVar();
        if (auto* o = v.getDynamicObject())
        {
            o->setProperty ("input",  ch + 1);
            o->setProperty ("output", ch + 1);
        }
        stripArr.add (v);
    }
    root->setProperty ("strips", stripArr);
    return juce::var (root);
}

void AudioEngine::applySessionVar (const juce::var& root, juce::StringArray& errors)
{
    if (auto* arr = root.getProperty ("strips", {}).getArray())
    {
        for (int ch = 0; ch < arr->size() && ch < (int) strips.size(); ++ch)
        {
            const auto& s = (*arr)[ch];
            const float gainDb = (float) (double) s.getProperty ("outGainDb", 0.0);
            strips[(size_t) ch]->setChannelName (s.getProperty ("name", "").toString());
            strips[(size_t) ch]->loadChain (s.getProperty ("plugins", {}), gainDb, errors);
        }
    }
}

void AudioEngine::autoSaveSession()
{
    // 장치가 없으면(예: ASIO 점유 실패) 저장 생략 — 빈 데이터로 기존 세션 덮어쓰기 방지.
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return;

    const auto json = juce::JSON::toString (getSessionVar());
    const auto file = getSessionFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText (json);
}

bool AudioEngine::saveSessionToFile (const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (getSessionVar()));
}

bool AudioEngine::loadSessionFromFile (const juce::File& file, juce::StringArray& errors)
{
    const auto parsed = juce::JSON::parse (file);
    if (! parsed.isObject())
    {
        errors.add (u8 ("세션 파일을 읽을 수 없습니다: ") + file.getFullPathName());
        return false;
    }
    applySessionVar (parsed, errors);
    return true;
}

void AudioEngine::saveSettings()
{
    if (auto xml = deviceManager.createStateXml())
    {
        const auto file = getSettingsFile();
        file.getParentDirectory().createDirectory();
        xml->writeTo (file);
    }
}

ChannelStrip* AudioEngine::getStrip (int channel) noexcept
{
    if (channel >= 0 && channel < (int) strips.size())
        return strips[(size_t) channel].get();
    return nullptr;
}

float AudioEngine::getInputPeak (int channel) const noexcept
{
    if (channel < 0 || channel >= maxChannels)
        return 0.0f;
    return inputPeaks[(size_t) channel].load (std::memory_order_relaxed);
}

void AudioEngine::resetPeaks() noexcept
{
    for (auto& p : inputPeaks)
        p.store (0.0f, std::memory_order_relaxed);
}

void AudioEngine::updateInputPeak (int channel, const float* data, int numSamples) noexcept
{
    if (channel < 0 || channel >= maxChannels)
        return;

    const auto range = juce::FloatVectorOperations::findMinAndMax (data, numSamples);
    const float peak = juce::jmax (std::fabs (range.getStart()), std::fabs (range.getEnd()));
    inputPeaks[(size_t) channel].store (peak, std::memory_order_relaxed);
}

//==============================================================================
void AudioEngine::resetDspLoadStats() noexcept
{
    dspPeak.store (0.0f, std::memory_order_relaxed);
    dspOverruns.store (0, std::memory_order_relaxed);
}

void AudioEngine::prepareSyntheticStrips (int totalChannels, juce::StringArray& errors)
{
    const int active = getActiveChannelCount();
    totalChannels = juce::jlimit (0, maxChannels, totalChannels);
    if (active <= 0 || strips.empty())
        return;

    // Ch1 체인·게인을 합성 채널에 복제 (플러그인 인스턴스는 채널마다 독립 생성).
    const auto state   = strips[0]->getStateVar();
    const auto plugins = state.getProperty ("plugins", {});
    const float gainDb = (float) (double) state.getProperty ("outGainDb", 0.0);

    for (int ch = active; ch < totalChannels; ++ch)
        strips[(size_t) ch]->loadChain (plugins, gainDb, errors);
}

void AudioEngine::clearSyntheticStrips()
{
    const int active = getActiveChannelCount();
    juce::StringArray ignored;
    for (int ch = juce::jmax (0, active); ch < maxChannels; ++ch)
        if (strips[(size_t) ch]->getNumPlugins() > 0)
            strips[(size_t) ch]->loadChain (juce::var(), 0.0f, ignored);
}

//==============================================================================
void AudioEngine::startLatencyTest (int outChannel, int inChannel) noexcept
{
    ltOutCh.store (outChannel, std::memory_order_relaxed);
    ltInCh.store  (inChannel,  std::memory_order_relaxed);
    ltRequested.store (true, std::memory_order_release);
}

int AudioEngine::getDriverRoundTripSamples() noexcept
{
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        return dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();
    return -1;
}

double AudioEngine::getCurrentSampleRate() noexcept
{
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        return dev->getCurrentSampleRate();
    return 0.0;
}

void AudioEngine::runLatencyBlock (const float* const* in, int numIn,
                                   float* const* out, int numOut, int numSamples) noexcept
{
    constexpr float impulseAmp = 0.5f;
    constexpr float detectThresh = 0.05f;
    constexpr int   guardSamples = 8;   // 임펄스 직후 자기검출 방지

    // 측정 중에는 모든 출력을 무음으로
    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            juce::FloatVectorOperations::clear (out[ch], numSamples);

    const int outCh = ltOutCh.load (std::memory_order_relaxed);
    const int inCh  = ltInCh.load  (std::memory_order_relaxed);

    // 1) 임펄스 송출 (최초 1회)
    if (! ltEmitted)
    {
        if (outCh >= 0 && outCh < numOut && out[outCh] != nullptr)
        {
            out[outCh][0] = impulseAmp;
            ltEmitSample  = ltSampleCounter;
            ltEmitted     = true;
        }
        else
        {
            ltState.store (ltTimeout, std::memory_order_release);   // 잘못된 출력 채널
            return;
        }
    }

    // 2) 입력에서 도착 검출
    if (ltEmitted && inCh >= 0 && inCh < numIn && in[inCh] != nullptr)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const long long pos = ltSampleCounter + i;
            if (pos <= ltEmitSample + guardSamples)
                continue;

            if (std::fabs (in[inCh][i]) > detectThresh)
            {
                ltResult.store ((int) (pos - ltEmitSample), std::memory_order_release);
                ltState.store  (ltDone, std::memory_order_release);
                ltSampleCounter += numSamples;
                return;
            }
        }
    }

    // 3) 타임아웃
    if (ltEmitted && (ltSampleCounter - ltEmitSample) > ltTimeoutSamples)
        ltState.store (ltTimeout, std::memory_order_release);

    ltSampleCounter += numSamples;
}

//==============================================================================
void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused (context);

    // ── DSP 부하 실측: 콜백 벽시계 시간 / 버퍼 예산 (QPC — 무락·무할당) ──
    const auto t0 = juce::Time::getHighResolutionTicks();

    processAudioBlock (inputChannelData, numInputChannels,
                       outputChannelData, numOutputChannels, numSamples);

    if (cbSampleRate > 0.0 && numSamples > 0)
    {
        const double elapsed = juce::Time::highResolutionTicksToSeconds (
                                   juce::Time::getHighResolutionTicks() - t0);
        const float load = (float) (elapsed * cbSampleRate / (double) numSamples);

        dspAvgLocal += 0.05f * (load - dspAvgLocal);
        dspAvg.store (dspAvgLocal, std::memory_order_relaxed);

        float prev = dspPeak.load (std::memory_order_relaxed);
        while (load > prev
               && ! dspPeak.compare_exchange_weak (prev, load, std::memory_order_relaxed)) {}

        if (load > 1.0f)
            dspOverruns.fetch_add (1, std::memory_order_relaxed);
    }
}

void AudioEngine::processAudioBlock (const float* const* inputChannelData,
                                     int numInputChannels,
                                     float* const* outputChannelData,
                                     int numOutputChannels,
                                     int numSamples) noexcept
{
    const juce::ScopedNoDenormals noDenormals;

    // ── 레이턴시 테스트가 요청되면 측정 모드로 전환 ──
    if (ltState.load (std::memory_order_acquire) != ltMeasuring
        && ltRequested.exchange (false, std::memory_order_acq_rel))
    {
        ltSampleCounter = 0;
        ltEmitSample    = -1;
        ltEmitted       = false;
        ltResult.store (-1, std::memory_order_release);
        ltState.store (ltMeasuring, std::memory_order_release);
    }

    if (ltState.load (std::memory_order_acquire) == ltMeasuring)
    {
        runLatencyBlock (inputChannelData, numInputChannels,
                         outputChannelData, numOutputChannels, numSamples);
        return;   // 측정 중엔 일반 처리 생략
    }

    // ── 재생 모드 ── 라이브 입력 뮤트, 녹음된 스템을 (옵션: 체인 통과) 출력 ──
    if (transport.load (std::memory_order_acquire) == tsPlaying)
    {
        player.readBlock (numSamples);
        const bool thru = playThroughChain.load (std::memory_order_relaxed);
        const int  pch  = player.getNumChannels();

        int numJobs = 0;   // 체인 통과 채널만 잡으로 (A2 병렬)

        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            float* out = outputChannelData[ch];
            if (out == nullptr)
                continue;

            const float* stem = (ch < pch) ? player.getChannel (ch) : nullptr;
            if (stem != nullptr)
            {
                updateInputPeak (ch, stem, numSamples);

                if (thru && ch < (int) strips.size())
                    jobScratch[(size_t) numJobs++] = { strips[(size_t) ch].get(), stem, out, numSamples };
                else
                    juce::FloatVectorOperations::copy (out, stem, numSamples);
            }
            else
            {
                juce::FloatVectorOperations::clear (out, numSamples);
            }
        }

        workerPool.processJobs (jobScratch.data(), numJobs);

        // 끝에 도달하면 정지(라이브 모니터 복귀). 정리는 GUI/다음 재생에서.
        if (player.getPosition() >= player.getTotalSamples())
            transport.store (tsStopped, std::memory_order_release);

        return;
    }

    const int routed = juce::jmin (numInputChannels, numOutputChannels);
    int numJobs = 0;   // 채널 = 잡 (A2: 워커+오디오 스레드 분담)

    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        float* out = outputChannelData[ch];
        if (out == nullptr)
            continue;

        if (ch < routed && inputChannelData[ch] != nullptr)
        {
            const float* in = inputChannelData[ch];

            updateInputPeak (ch, in, numSamples);   // 입력 미터

            // 채널 VST3 체인 처리 (체인 비었으면 사실상 패스스루)
            if (ch < (int) strips.size())
                jobScratch[(size_t) numJobs++] = { strips[(size_t) ch].get(), in, out, numSamples };
            else
                juce::FloatVectorOperations::copy (out, in, numSamples);
        }
        else
        {
            juce::FloatVectorOperations::clear (out, numSamples);
        }
    }

    // ── 합성 부하 (Phase 4 프로파일링) ── 물리 채널 이후 strips 를 입력 복제로
    // 실제 처리해 다채널 CPU 부하를 재현한다. 출력은 채널별 스크래치에 버린다.
    const int synthN = juce::jmin (synthChannels.load (std::memory_order_relaxed),
                                   (int) strips.size());
    if (synthN > routed && numInputChannels > 0
        && synthScratch.getNumSamples() >= numSamples
        && synthScratch.getNumChannels() >= synthN)
    {
        for (int ch = routed; ch < synthN; ++ch)
        {
            const float* in = inputChannelData[ch % numInputChannels];
            if (in != nullptr)
                jobScratch[(size_t) numJobs++] = { strips[(size_t) ch].get(), in,
                                                   synthScratch.getWritePointer (ch), numSamples };
        }
    }

    workerPool.processJobs (jobScratch.data(), numJobs);

    // ── Dry tap ── 스트립은 in→out 으로 처리하므로 inputChannelData 는 원음 그대로다.
    // 채널별 원음을 락프리 FIFO 로 넘긴다(디스크 쓰기는 Writer 스레드).
    recorder.writeBlock (inputChannelData, numInputChannels, numSamples);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    resetPeaks();

    // 버퍼/SR 이 바뀔 수 있으니 재생 정지 + 테이크 언로드(다음 재생 때 새 버퍼로 로드).
    transport.store (tsStopped, std::memory_order_release);
    player.unload();

    const double sr = device->getCurrentSampleRate();
    const int    bs = device->getCurrentBufferSizeSamples();

    ltTimeoutSamples = (long long) (sr > 0.0 ? sr : 48000.0);   // 1초 타임아웃

    // DSP 부하 측정 준비 (콜백 시작 전 — 오디오 스레드와 경쟁 없음)
    cbSampleRate = sr;
    synthScratch.setSize (maxChannels, juce::jmax (1, bs), false, false, true);
    dspAvgLocal = 0.0f;
    resetDspLoadStats();
    dspAvg.store (0.0f, std::memory_order_relaxed);

    // A3: 잡 처리 시간이 블록 예산을 넘는 블록을 스파이크로 캡처.
    if (sr > 0.0 && bs > 0)
        workerPool.setSpikeThresholdUs ((juce::uint32) (bs * 1.0e6 / sr));

    for (auto& s : strips)
        s->prepare (sr, bs);
}

void AudioEngine::audioDeviceStopped()
{
    transport.store (tsStopped, std::memory_order_release);
    recorder.stop();   // 장치가 멈추면 녹음도 안전하게 마감
    player.unload();
    resetPeaks();

    for (auto& s : strips)
        s->releaseResources();
}
