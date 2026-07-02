#include "CpuProfiler.h"
#include "Util.h"

using sr::u8;

//==============================================================================
bool CpuProfiler::start (juce::String& error)
{
    if (running)
    {
        error = u8 ("프로파일이 이미 실행 중입니다.");
        return false;
    }

    const int active = engine.getActiveChannelCount();
    if (active <= 0 || engine.getCurrentSampleRate() <= 0.0)
    {
        error = u8 ("오디오 장치가 없습니다. 먼저 ASIO 장치를 선택하세요.");
        return false;
    }

    if (engine.getTransportState() != AudioEngine::tsStopped)
    {
        error = u8 ("정지 상태에서만 프로파일할 수 있습니다.");
        return false;
    }

    // 측정 단계: 활성 채널 수부터 32 까지 (활성 이하 단계는 의미 없음 → 제외)
    stepChannels.clear();
    stepChannels.add (active);
    for (const int n : { 4, 8, 16, 24, 32 })
        if (n > active)
            stepChannels.add (n);

    report = {};
    stepIndex = 0;
    running   = true;

    // Ch1 체인을 합성 채널 전체에 복제 (수 초 걸릴 수 있음 — 시작 시 1회)
    progress (u8 ("프로파일 준비: Ch1 체인을 32채널에 복제 중..."));
    engine.prepareSyntheticStrips (AudioEngine::maxChannels, report.prepareErrors);

    beginStep();
    startTimer (100);
    return true;
}

void CpuProfiler::abort()
{
    if (! running)
        return;
    endRun (false);
    progress (u8 ("프로파일 중단됨."));
}

//==============================================================================
void CpuProfiler::beginStep()
{
    const int n = stepChannels[stepIndex];
    engine.setSyntheticChannels (n);
    measuring  = false;
    phaseStart = juce::Time::getMillisecondCounter();
    progress (juce::String (n) + u8 (" ch 안정화 중..."));
}

void CpuProfiler::timerCallback()
{
    // 장치 정지·트랜스포트 개입 시 측정 무효 → 중단
    if (engine.getCurrentSampleRate() <= 0.0
        || engine.getTransportState() != AudioEngine::tsStopped)
    {
        abort();
        return;
    }

    const auto elapsed = (int) (juce::Time::getMillisecondCounter() - phaseStart);

    if (! measuring)
    {
        if (elapsed < settleMs)
            return;

        measuring   = true;
        phaseStart  = juce::Time::getMillisecondCounter();
        engine.resetDspLoadStats();
        xrunBase    = engine.getDeviceXRuns();
        overrunBase = engine.getDspOverruns();
        return;
    }

    const int n = stepChannels[stepIndex];
    progress (juce::String (n) + u8 (" ch 측정 중... DSP ")
              + juce::String (juce::roundToInt (engine.getDspLoadAvg() * 100.0f)) + "% (pk "
              + juce::String (juce::roundToInt (engine.getDspLoadPeak() * 100.0f)) + "%)  "
              + juce::String ((measureMs - elapsed) / 1000 + 1) + "s");

    if (elapsed < measureMs)
        return;

    StepResult r;
    r.channels = n;
    r.avgLoad  = engine.getDspLoadAvg();
    r.peakLoad = engine.getDspLoadPeak();
    r.xruns    = engine.getDeviceXRuns() - xrunBase;
    r.overruns = engine.getDspOverruns() - overrunBase;
    report.steps.add (r);

    if (++stepIndex >= stepChannels.size())
    {
        endRun (true);
        return;
    }

    beginStep();
}

//==============================================================================
void CpuProfiler::endRun (bool completed)
{
    stopTimer();
    running = false;

    engine.setSyntheticChannels (0);
    engine.clearSyntheticStrips();

    if (! completed)
        return;

    report.completed = true;

    // 한계 채널 수 = 처음부터 연속 안정인 마지막 단계
    report.maxStableChannels = 0;
    for (const auto& s : report.steps)
    {
        if (! s.isStable())
            break;
        report.maxStableChannels = s.channels;
    }

    report.file = writeReport();

    if (onFinished != nullptr)
        onFinished (report);
}

juce::File CpuProfiler::writeReport()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                         .getChildFile ("Superrack").getChildFile ("profiles");
    dir.createDirectory();

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    const auto file  = dir.getChildFile ("cpu-profile-" + stamp + ".md");

    juce::String md;
    md << "# Superrack CPU 프로파일 — " << juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S") << "\n\n";

    // 환경
    md << u8 ("## 환경\n");
    if (auto* dev = engine.getDeviceManager().getCurrentAudioDevice())
    {
        md << u8 ("- 장치: ") << dev->getName() << "\n"
           << "- SR: "   << juce::String (dev->getCurrentSampleRate() / 1000.0, 1) << " kHz\n"
           << u8 ("- 버퍼: ") << dev->getCurrentBufferSizeSamples() << u8 (" 샘플 (예산 ")
           << juce::String (dev->getCurrentBufferSizeSamples() * 1000.0 / dev->getCurrentSampleRate(), 2)
           << " ms)\n";
    }
    md << u8 ("- 물리 활성 채널: ") << engine.getActiveChannelCount() << "\n";

    // 측정에 쓰인 체인 (Ch1 기준 — 전 합성 채널에 복제)
    md << u8 ("- 체인(Ch1, 전 채널 복제): ");
    if (auto* s0 = engine.getStrip (0))
    {
        if (s0->getNumPlugins() == 0)
            md << u8 ("(비어 있음 — 패스스루 부하만 측정됨)");
        else
            for (int i = 0; i < s0->getNumPlugins(); ++i)
                md << (i > 0 ? " → " : "") << s0->getPluginName (i);
    }
    md << "\n";

    if (! report.prepareErrors.isEmpty())
        md << u8 ("- 복제 오류: ") << report.prepareErrors.joinIntoString (" / ") << "\n";

    md << u8 ("- 측정: 단계별 안정화 ") << (settleMs / 1000) << u8 ("초 + 측정 ")
       << (measureMs / 1000) << u8 ("초, 합성 부하(입력 복제, 출력 폐기)\n\n");

    // 결과 표
    md << u8 ("## 결과\n\n");
    md << u8 ("| 채널 | DSP avg | DSP peak | 예산 초과 | 장치 xrun | 판정 |\n");
    md << "|---:|---:|---:|---:|---:|:---|\n";
    for (const auto& s : report.steps)
        md << "| " << s.channels
           << " | " << juce::String (s.avgLoad  * 100.0, 1) << "%"
           << " | " << juce::String (s.peakLoad * 100.0, 1) << "%"
           << " | " << s.overruns
           << " | " << s.xruns
           << " | " << (s.isStable() ? u8 ("안정") : u8 ("**불안정**")) << " |\n";

    md << "\n";
    md << u8 ("## 판정\n");
    if (report.maxStableChannels >= AudioEngine::maxChannels)
        md << u8 ("**32채널 전 단계 안정** — 현 체인·버퍼 설정으로 32ch 여유 있음.\n");
    else if (report.maxStableChannels > 0)
        md << u8 ("**한계 채널 수: ") << report.maxStableChannels
           << u8 ("ch** (기준: 측정 8초간 xrun 0 · 예산 초과 0 · peak < 95%). ")
           << u8 ("초과 시 버퍼 상향 또는 체인 경량화 필요.\n");
    else
        md << u8 ("**전 단계 불안정** — 체인이 과중하거나 버퍼가 너무 작음. 버퍼 상향 권장.\n");

    file.replaceWithText (md);
    return file;
}

void CpuProfiler::progress (const juce::String& text)
{
    if (onProgress != nullptr)
        onProgress (text);
}
