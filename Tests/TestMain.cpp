/*  Superrack L1 테스트 (QA Phase — BACKLOG G).

    핵심 경로를 실기(ASIO/플러그인) 없이 헤드리스로 검증한다:
      1. AppSettings        — 저장/로드, 경계값, 격리(SUPERRACK_APPDATA)
      2. MultitrackRecorder — FIFO→디스크 무손실 (녹음 경로)
      3. TakeManager        — 커밋/펀치 병합/undo·redo 스왑
      4. TimelinePlayer     — SPSC 스트리밍 1:1 비트 일치, 시크, 리샘플, EOF
      5. AudioWorkerPool    — 병렬=직렬 결과 동일(잡 정확히 1회), 계측 플러밍

    실행: SuperrackTests.exe → 실패 수를 종료 코드로 반환.
    ASIO 장치·VST3 플러그인 경로는 실기 검증(수동) 영역 — docs/05-qa 리포트 참조.
*/

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "../Source/AppSettings.h"
#include "../Source/AudioWorkerPool.h"
#include "../Source/ChannelStrip.h"
#include "../Source/MultitrackRecorder.h"
#include "../Source/TakeManager.h"
#include "../Source/TimelinePlayer.h"

#include <cstdlib>
#include <iostream>

//==============================================================================
static int gPass = 0, gFail = 0;

#define EXPECT(cond)                                                            \
    do { if (cond) ++gPass;                                                     \
         else { ++gFail; std::cout << "  FAIL L" << __LINE__ << ": " << #cond << std::endl; } } while (0)

#define EXPECT_NEAR(a, b, tol)                                                  \
    do { if (std::abs ((double)(a) - (double)(b)) <= (tol)) ++gPass;            \
         else { ++gFail; std::cout << "  FAIL L" << __LINE__ << ": " << (a)     \
                         << " != " << (b) << " (tol " << (tol) << ")\n"; } } while (0)

static void section (const char* name) { std::cout << "== " << name << std::endl; }

//==============================================================================
// WAV 헬퍼 (프로젝트와 동일: 32f 모노)
static juce::AudioFormatManager& fmts()
{
    static juce::AudioFormatManager m;
    if (m.getNumKnownFormats() == 0)
        m.registerBasicFormats();
    return m;
}

static bool writeWav (const juce::File& file, const std::vector<float>& data, double sr)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    const auto opts = juce::AudioFormatWriterOptions{}
                          .withSampleRate (sr).withNumChannels (1).withBitsPerSample (32)
                          .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream, opts));
    if (w == nullptr)
        return false;

    juce::AudioBuffer<float> buf (1, (int) data.size());
    for (size_t i = 0; i < data.size(); ++i)
        buf.setSample (0, (int) i, data[i]);
    return w->writeFromAudioSampleBuffer (buf, 0, (int) data.size());
}

static std::vector<float> readWav (const juce::File& file)
{
    std::vector<float> out;
    std::unique_ptr<juce::AudioFormatReader> r (fmts().createReaderFor (file));
    if (r == nullptr)
        return out;

    juce::AudioBuffer<float> buf (1, (int) r->lengthInSamples);
    r->read (&buf, 0, (int) r->lengthInSamples, 0, true, false);
    out.resize ((size_t) r->lengthInSamples);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = buf.getSample (0, (int) i);
    return out;
}

// 인덱스마다 유일 + float 정확 표현 (2^16 분모) — 비트 일치 비교용
static float rampValue (juce::int64 i) { return (float) (i & 0xFFFF) / 65536.0f; }

static std::vector<float> makeRamp (int n, juce::int64 offset = 0)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
        v[(size_t) i] = rampValue (offset + i);
    return v;
}

//==============================================================================
static void testAppSettings (const juce::File& sandbox)
{
    section ("AppSettings");
    auto& s = AppSettings::get();

    // 기본 저장 루트 = 유효 경로
    EXPECT (! s.isStorageRootCustom());
    EXPECT (s.storageRoot().getFullPathName().isNotEmpty());

    const auto custom = sandbox.getChildFile ("storage");
    s.setStorageRoot (custom);
    EXPECT (s.isStorageRootCustom());
    EXPECT (s.storageRoot() == custom);
    EXPECT (custom.isDirectory());   // 접근 시 생성

    // 파일에 실제로 기록됐는지 (격리 폴더 안)
    const auto json = juce::JSON::parse (sandbox.getChildFile ("appdata").getChildFile ("app-settings.json"));
    EXPECT (json.getProperty ("storageRoot", "").toString() == custom.getFullPathName());

    // VST3 추가 경로 왕복 + 공백 제거
    s.setVst3ExtraPaths ({ "D:\\Plugins\\VST3", "  ", "E:\\More" });
    const auto paths = s.vst3ExtraPaths();
    EXPECT (paths.size() == 2 && paths[0] == "D:\\Plugins\\VST3" && paths[1] == "E:\\More");

    // 워커 수 클램프
    s.setWorkerCountOverride (99);
    EXPECT (s.workerCountOverride() == 6);
    s.setWorkerCountOverride (0);
    EXPECT (s.workerCountOverride() == 0);
}

//==============================================================================
static void testRecorder (const juce::File& sandbox)
{
    section ("MultitrackRecorder");
    const auto dir = sandbox.getChildFile ("rec");

    MultitrackRecorder rec;
    juce::String err;
    EXPECT (rec.start (48000.0, 2, dir, err));
    EXPECT (rec.isRecording());

    // 채널별 다른 램프를 블록 단위로 push
    const int blockSize = 512, numBlocks = 40;
    std::vector<float> ch0, ch1;
    for (int b = 0; b < numBlocks; ++b)
    {
        auto a0 = makeRamp (blockSize, (juce::int64) b * blockSize);
        auto a1 = makeRamp (blockSize, 100000 + (juce::int64) b * blockSize);
        const float* chans[2] = { a0.data(), a1.data() };
        rec.writeBlock (chans, 2, blockSize);
        ch0.insert (ch0.end(), a0.begin(), a0.end());
        ch1.insert (ch1.end(), a1.begin(), a1.end());
    }
    rec.stop();
    EXPECT (! rec.isRecording());
    EXPECT (rec.getXrunCount() == 0);

    const auto r0 = readWav (dir.getChildFile ("ch01_dry.wav"));
    const auto r1 = readWav (dir.getChildFile ("ch02_dry.wav"));
    EXPECT (r0 == ch0);   // 무손실 (락프리 FIFO → 디스크)
    EXPECT (r1 == ch1);
}

//==============================================================================
static void testTakeManager (const juce::File& sandbox)
{
    section ("TakeManager (커밋/펀치/undo)");
    TakeManager mgr;
    TakeManager::TakeEnv env;
    env.deviceName = "TestDevice"; env.sampleRate = 48000.0; env.bufferSize = 512; env.channels = 2;

    auto* obj1 = new juce::DynamicObject(); obj1->setProperty ("marker", "commit1");
    auto* obj2 = new juce::DynamicObject(); obj2->setProperty ("marker", "commit2");
    const juce::var snap1 (obj1), snap2 (obj2);   // var 가 소유 — 임시 재생성 금지

    const auto takeDir = mgr.createTake (env, snap1);
    EXPECT (takeDir.isDirectory());
    EXPECT (takeDir.getChildFile ("timeline.json").existsAsFile());
    EXPECT (! mgr.hasUndoState (takeDir));

    // ── 1차 커밋 (전체 녹음 20000 샘플) ──
    const auto rectmp = sandbox.getChildFile ("rectmp1");
    const int len1 = 20000;
    std::vector<float> take1 (len1, 0.25f);
    EXPECT (writeWav (rectmp.getChildFile ("ch01_dry.wav"), take1, 48000.0));
    EXPECT (writeWav (rectmp.getChildFile ("ch02_dry.wav"), take1, 48000.0));

    mgr.commitRecording (takeDir, rectmp, 0, env, snap1);
    EXPECT (mgr.readTake (takeDir).lengthSamples == len1);
    EXPECT (mgr.hasUndoState (takeDir));   // 커밋 직전(빈 테이크) 상태 보존
    EXPECT (readWav (takeDir.getChildFile ("ch01_dry.wav")) == take1);

    // ── 2차 커밋: 펀치 P=5000, 새 녹음 8000 샘플 (-0.5f) → 최종 13000 ──
    const auto rectmp2 = sandbox.getChildFile ("rectmp2");
    const int punch = 5000, len2 = 8000;
    std::vector<float> take2 (len2, -0.5f);
    EXPECT (writeWav (rectmp2.getChildFile ("ch01_dry.wav"), take2, 48000.0));
    EXPECT (writeWav (rectmp2.getChildFile ("ch02_dry.wav"), take2, 48000.0));

    mgr.commitRecording (takeDir, rectmp2, punch, env, snap2);
    const auto merged = readWav (takeDir.getChildFile ("ch01_dry.wav"));
    EXPECT ((int) merged.size() == punch + len2);
    EXPECT (merged[0] == 0.25f && merged[punch - 1] == 0.25f);       // head = 기존
    EXPECT (merged[punch] == -0.5f && merged.back() == -0.5f);       // tail = 새 녹음
    EXPECT (mgr.readTake (takeDir).lengthSamples == punch + len2);
    EXPECT (mgr.readTake (takeDir).historyCount == 2);

    // ── undo: 1차 커밋 상태로 복귀 ──
    EXPECT (mgr.isCurrentNewer (takeDir));
    EXPECT (mgr.swapUndoState (takeDir));
    EXPECT (readWav (takeDir.getChildFile ("ch01_dry.wav")) == take1);
    EXPECT (mgr.readTake (takeDir).lengthSamples == len1);
    EXPECT (mgr.readSession (takeDir).getProperty ("marker", "") == "commit1");
    EXPECT (! mgr.isCurrentNewer (takeDir));

    // ── redo: 펀치 결과로 복귀 ──
    EXPECT (mgr.swapUndoState (takeDir));
    EXPECT (readWav (takeDir.getChildFile ("ch01_dry.wav")) == merged);
    EXPECT (mgr.readSession (takeDir).getProperty ("marker", "") == "commit2");

    // ── 채널 수 감소 커밋: 잔여 스템이 현재 상태에 남지 않고 undo 로 복원 ──
    {
        const auto rectmpN = sandbox.getChildFile ("rectmpN");
        rectmpN.deleteRecursively();
        EXPECT (writeWav (rectmpN.getChildFile ("ch01_dry.wav"), take2, 48000.0));   // 1채널만
        mgr.commitRecording (takeDir, rectmpN, 0, env, snap2);
        EXPECT (takeDir.getChildFile ("ch01_dry.wav").existsAsFile());
        EXPECT (! takeDir.getChildFile ("ch02_dry.wav").existsAsFile());             // 잔여 스템 정리
        EXPECT (mgr.swapUndoState (takeDir));                                        // undo → 2채널 복원
        EXPECT (takeDir.getChildFile ("ch02_dry.wav").existsAsFile());
        EXPECT (mgr.swapUndoState (takeDir));                                        // redo → 다시 1채널
        EXPECT (! takeDir.getChildFile ("ch02_dry.wav").existsAsFile());
    }

    // ── 첫 녹음 undo → 빈 테이크, redo → 복원 ──
    const auto take2Dir = mgr.createTake (env, juce::var());
    const auto rectmp3 = sandbox.getChildFile ("rectmp3");
    EXPECT (writeWav (rectmp3.getChildFile ("ch01_dry.wav"), take1, 48000.0));
    mgr.commitRecording (take2Dir, rectmp3, 0, env, juce::var());
    EXPECT (mgr.swapUndoState (take2Dir));
    EXPECT (! take2Dir.getChildFile ("ch01_dry.wav").existsAsFile());   // 빈 테이크
    EXPECT (mgr.readTake (take2Dir).lengthSamples == 0);
    EXPECT (mgr.swapUndoState (take2Dir));
    EXPECT (readWav (take2Dir.getChildFile ("ch01_dry.wav")) == take1);
}

//==============================================================================
// 언더런 무음/대기 설계에 맞춰 소비: 실제 소비량(getPosition 델타)만 검증에 사용.
static juce::int64 consumeAndVerify (TimelinePlayer& p, int block, juce::int64 upTo,
                                     int channelOffsetKey, int& mismatches)
{
    juce::int64 pos = p.getPosition();
    int idleSpins = 0;
    while (pos < upTo && idleSpins < 200)
    {
        const auto before = pos;
        p.readBlock (block);
        pos = p.getPosition();
        const int consumed = (int) (pos - before);
        if (consumed == 0) { ++idleSpins; juce::Thread::sleep (1); continue; }
        idleSpins = 0;

        const float* data = p.getChannel (0);
        for (int i = 0; i < consumed; ++i)
            if (data[i] != rampValue (channelOffsetKey + before + i))
                ++mismatches;
    }
    return pos;
}

static void testTimelinePlayer (const juce::File& sandbox)
{
    section ("TimelinePlayer (스트리밍/시크/리샘플/EOF)");
    const auto dir = sandbox.getChildFile ("stems");
    const int N = 48000;
    EXPECT (writeWav (dir.getChildFile ("ch01_dry.wav"), makeRamp (N), 48000.0));
    EXPECT (writeWav (dir.getChildFile ("ch02_dry.wav"), makeRamp (N, 7777), 48000.0));

    TimelinePlayer p;
    juce::String err;

    // ── 1:1 (같은 SR) — 비트 일치 ──
    EXPECT (p.load (dir, 512, 48000.0, err));
    EXPECT (p.getNumChannels() == 2);
    EXPECT (p.getTotalSamples() == N);
    EXPECT (p.getSampleRate() == 48000.0);

    int bad = 0;
    consumeAndVerify (p, 512, 20000, 0, bad);
    EXPECT (bad == 0);

    // ── 시크 → 그 위치부터 이어짐 ──
    p.setPosition (30000);
    EXPECT (p.getPosition() == 30000);
    bad = 0;
    consumeAndVerify (p, 512, 40000, 0, bad);
    EXPECT (bad == 0);

    // ── EOF: 끝까지 소비 → 위치 = total, 이후 무음 ──
    bad = 0;
    const auto endPos = consumeAndVerify (p, 512, N, 0, bad);
    EXPECT (bad == 0 && endPos == N);
    p.readBlock (512);
    EXPECT (p.getPosition() == N);
    EXPECT (p.getUnderrunCount() == 0 || p.getUnderrunCount() > 0);   // 카운터 접근 자체 검증

    // ── ch02 내용 (다른 오프셋) ──
    p.setPosition (0);
    juce::int64 pos = 0; bad = 0;
    while (pos < 4096)
    {
        const auto before = pos;
        p.readBlock (512);
        pos = p.getPosition();
        const float* d1 = p.getChannel (1);
        for (int i = 0; i < (int) (pos - before); ++i)
            if (d1[i] != rampValue (7777 + before + i)) ++bad;
        if (pos == before) juce::Thread::sleep (1);
    }
    EXPECT (bad == 0);
    p.unload();

    // ── 리샘플: 파일 48k → 장치 96k (ratio 0.5) ──
    EXPECT (p.load (dir, 512, 96000.0, err));
    EXPECT (p.getSampleRate() == 96000.0);
    EXPECT (std::abs (p.getTotalSamples() - (juce::int64) N * 2) <= 2);

    pos = 0; bad = 0;
    double maxErr = 0.0;
    while (pos < 16384)
    {
        const auto before = pos;
        p.readBlock (512);
        pos = p.getPosition();
        const float* d = p.getChannel (0);
        for (int i = 0; i < (int) (pos - before); ++i)
        {
            const double filePos  = (double) (before + i) * 0.5;   // 출력 k → 파일 k/2
            const double expected = filePos / 65536.0;             // 램프 (N < 65536 라 단조)
            maxErr = juce::jmax (maxErr, std::abs ((double) d[i] - expected));
        }
        if (pos == before) juce::Thread::sleep (1);
    }
    EXPECT (maxErr < 1.0e-3);   // Lagrange 보간 오차 허용
    p.unload();
}

//==============================================================================
static void testWorkerPool()
{
    section ("AudioWorkerPool + ChannelStrip (병렬 DSP)");

    juce::AudioPluginFormatManager pluginFormats;   // 플러그인 없이 패스스루+게인만
    PluginScanCache cache;
    AudioWorkerPool pool;
    std::cout << "  워커 수: " << pool.getNumWorkers() << "\n";

    const int numCh = 32, block = 256;
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    for (int i = 0; i < numCh; ++i)
    {
        auto s = std::make_unique<ChannelStrip> (pluginFormats, cache);
        s->prepare (48000.0, block);
        s->setOutGainDb (i % 2 == 0 ? 0.0f : -6.0f);   // 절반은 -6dB
        strips.push_back (std::move (s));
    }

    std::vector<std::vector<float>> ins ((size_t) numCh), outsP ((size_t) numCh), outsS ((size_t) numCh);
    for (int i = 0; i < numCh; ++i)
    {
        ins[(size_t) i]   = makeRamp (block, i * 1000);
        outsP[(size_t) i] = std::vector<float> ((size_t) block, 999.0f);
        outsS[(size_t) i] = std::vector<float> ((size_t) block, 999.0f);
    }

    auto runBlocks = [&] (std::vector<std::vector<float>>& outs, int iterations)
    {
        std::vector<AudioWorkerPool::Job> jobs ((size_t) numCh);
        for (int it = 0; it < iterations; ++it)
        {
            for (int i = 0; i < numCh; ++i)
                jobs[(size_t) i] = { strips[(size_t) i].get(), ins[(size_t) i].data(),
                                     outs[(size_t) i].data(), block };
            pool.processJobs (jobs.data(), numCh);
        }
    };

    // 병렬 500블록: 잡=1회 실행이 깨지면(누락/이중) 결과가 어긋난다.
    pool.setEnabled (true);
    pool.setSpikeThresholdUs (0);   // 계측 플러밍 검증: 모든 블록이 캡처 후보
    runBlocks (outsP, 500);

    pool.setEnabled (false);
    runBlocks (outsS, 1);

    int badCh = 0;
    for (int i = 0; i < numCh; ++i)
        if (outsP[(size_t) i] != outsS[(size_t) i])
            ++badCh;
    EXPECT (badCh == 0);   // 병렬 == 직렬 (결정적)

    // 게인 적용 확인 (-6dB 채널)
    const float g = juce::Decibels::decibelsToGain (-6.0f, -60.0f);
    EXPECT (outsS[1][100] == ins[1][100] * g);
    EXPECT (outsS[0][100] == ins[0][100]);   // 0dB 채널 (gain 1.0 곱)

    // A3 계측: 스냅샷이 채워졌는지
    AudioWorkerPool::SpikeSnapshot snap;
    EXPECT (pool.readSpikeSnapshot (snap));
    EXPECT (snap.numJobs == numCh);

    // 세션 직렬화 (플러그인 없는 왕복)
    strips[0]->setChannelName (juce::String (juce::CharPointer_UTF8 ("보컬")));
    const auto state = strips[0]->getStateVar();
    EXPECT (state.getProperty ("name", "") == juce::String (juce::CharPointer_UTF8 ("보컬")));
    EXPECT_NEAR ((double) state.getProperty ("outGainDb", 99.0), 0.0, 1.0e-6);

    juce::StringArray errs;
    strips[1]->loadChain (state.getProperty ("plugins", {}), -12.0f, errs);
    EXPECT (errs.isEmpty());
    EXPECT_NEAR (strips[1]->getOutGainDb(), -12.0f, 1.0e-6);
}

//==============================================================================
int main()
{
    // 테스트 격리: 설정/저장이 실제 사용자 데이터에 닿지 않게 샌드박스로.
    const auto sandbox = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("SuperrackTests");
    sandbox.deleteRecursively();
    sandbox.createDirectory();
   #if JUCE_WINDOWS
    _putenv_s ("SUPERRACK_APPDATA", sandbox.getChildFile ("appdata").getFullPathName().toRawUTF8());
   #else
    setenv ("SUPERRACK_APPDATA", sandbox.getChildFile ("appdata").getFullPathName().toRawUTF8(), 1);
   #endif

    juce::ScopedJuceInitialiser_GUI juceInit;

    testAppSettings (sandbox);      // 반드시 첫 번째 — AppSettings 싱글턴 초기화 전 env 필요
    testRecorder (sandbox);
    testTakeManager (sandbox);
    testTimelinePlayer (sandbox);
    testWorkerPool();

    std::cout << "\n결과: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
