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
#include "../Source/BetaGate.h"
#include "../Source/ChannelStrip.h"
#include "../Source/MultitrackRecorder.h"
#include "../Source/PluginCatalog.h"
#include "../Source/TakeManager.h"
#include "../Source/TimelinePlayer.h"
#include "../Source/TimelineSegments.h"

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

    // ── 2차 커밋: 펀치 P=5000, 새 녹음 8000 샘플 (-0.5f) — 구간 대체(§5.12):
    //    [5000,13000) 만 새 녹음, 꼬리 [13000,20000) 은 기존 유지 → 길이 불변 ──
    const auto rectmp2 = sandbox.getChildFile ("rectmp2");
    const int punch = 5000, len2 = 8000;
    std::vector<float> take2 (len2, -0.5f);
    EXPECT (writeWav (rectmp2.getChildFile ("ch01_dry.wav"), take2, 48000.0));
    EXPECT (writeWav (rectmp2.getChildFile ("ch02_dry.wav"), take2, 48000.0));

    mgr.commitRecording (takeDir, rectmp2, punch, env, snap2);
    const auto merged = readWav (takeDir.getChildFile ("ch01_dry.wav"));
    EXPECT ((int) merged.size() == len1);                            // 길이는 절대 줄지 않는다
    EXPECT (merged[0] == 0.25f && merged[punch - 1] == 0.25f);       // head = 기존
    EXPECT (merged[punch] == -0.5f && merged[punch + len2 - 1] == -0.5f);   // [P,P+8000) = 새 녹음
    EXPECT (merged[punch + len2] == 0.25f && merged.back() == 0.25f);       // 꼬리 = 기존 보존
    EXPECT (mgr.readTake (takeDir).lengthSamples == len1);
    EXPECT (mgr.readTake (takeDir).historyCount == 2);
    EXPECT (mgr.readTake (takeDir).history.getLast().endSample == punch + len2);   // 대체 구간

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

    // ── armed 부분집합 + 펀치아웃 절단 (§5.12) ──
    {
        const auto tDir = mgr.createTake (env, snap1);
        const auto rectmpA = sandbox.getChildFile ("rectmpA");
        rectmpA.deleteRecursively();
        EXPECT (writeWav (rectmpA.getChildFile ("ch01_dry.wav"), take1, 48000.0));
        EXPECT (writeWav (rectmpA.getChildFile ("ch02_dry.wav"), take1, 48000.0));
        mgr.commitRecording (tDir, rectmpA, 0, env, snap1);

        // ch2(비트1)만 armed, P=5000, 새 8000 을 trim 4000 으로 절단 → [5000,9000) 만 대체
        const auto rectmpB = sandbox.getChildFile ("rectmpB");
        rectmpB.deleteRecursively();
        EXPECT (writeWav (rectmpB.getChildFile ("ch01_dry.wav"), take2, 48000.0));
        EXPECT (writeWav (rectmpB.getChildFile ("ch02_dry.wav"), take2, 48000.0));
        mgr.commitRecording (tDir, rectmpB, punch, env, snap2, 4000, 0b10u);

        EXPECT (readWav (tDir.getChildFile ("ch01_dry.wav")) == take1);   // 미선택 채널 불가침
        const auto c2 = readWav (tDir.getChildFile ("ch02_dry.wav"));
        EXPECT ((int) c2.size() == len1);                                  // 길이 불변
        EXPECT (c2[punch - 1] == 0.25f && c2[punch] == -0.5f);             // in 경계
        EXPECT (c2[punch + 3999] == -0.5f && c2[punch + 4000] == 0.25f);   // out 경계 = trim 절단
        const auto info = mgr.readTake (tDir);
        EXPECT (info.lengthSamples == len1);
        EXPECT (info.history.getLast().endSample == punch + 4000);
        EXPECT (info.history.getLast().channels.size() == 1
             && info.history.getLast().channels[0] == 1);                  // 만진 채널 기록

        // 부분 undo: 만지지 않은 ch01 은 불가침, ch02 만 원복 — redo 로 재적용
        EXPECT (mgr.swapUndoState (tDir));
        EXPECT (readWav (tDir.getChildFile ("ch01_dry.wav")) == take1);
        EXPECT (readWav (tDir.getChildFile ("ch02_dry.wav")) == take1);
        EXPECT (mgr.swapUndoState (tDir));
        EXPECT (readWav (tDir.getChildFile ("ch01_dry.wav")) == take1);
        EXPECT (readWav (tDir.getChildFile ("ch02_dry.wav")) == c2);
    }

    // ── 펀치 패스 커밋: srcOffset(프리롤) 건너뛰고 창만 절단 (§5.12 개정) ──
    {
        const auto tDir = mgr.createTake (env, snap1);
        const auto rectmpC = sandbox.getChildFile ("rectmpC");
        rectmpC.deleteRecursively();
        EXPECT (writeWav (rectmpC.getChildFile ("ch01_dry.wav"), take1, 48000.0));
        mgr.commitRecording (tDir, rectmpC, 0, env, snap1);   // 원본 20000 @0.25

        // 패스: 커서 3000 에서 시작(프리롤 2000), 창 [5000,9000) — 캡처는 패스 전체.
        // 캡처를 인덱스 램프로 만들어 창 절단이 "캡처의 어느 부분"인지 비트 검증한다.
        const auto rectmpD = sandbox.getChildFile ("rectmpD");
        rectmpD.deleteRecursively();
        EXPECT (writeWav (rectmpD.getChildFile ("ch01_dry.wav"), makeRamp (10000), 48000.0));
        mgr.commitRecording (tDir, rectmpD, 5000, env, snap2,
                             4000 /*trim=out-in*/, 0xffffffffu, 2000 /*srcOffset=in-커서*/);

        const auto c1 = readWav (tDir.getChildFile ("ch01_dry.wav"));
        EXPECT ((int) c1.size() == len1);                       // 길이 불변
        EXPECT (c1[4999] == 0.25f && c1[9000] == 0.25f);        // 창 밖 = 원본
        EXPECT (c1[5000] == rampValue (2000));                  // 창 시작 = 캡처의 srcOffset 지점
        EXPECT (c1[8999] == rampValue (5999));                  // 창 끝 = srcOffset+trim-1
        EXPECT (mgr.readTake (tDir).history.getLast().endSample == 9000);
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


//==============================================================================
// 구간 반복: 생산(리더)과 소비(오디오)가 **같은 경계**로 되감아야 한다.
// 어긋나면 표시 위치와 실제 소리가 벌어지므로 내용까지 대조한다.
static void testTimelinePlayerLoop (const juce::File& sandbox)
{
    section ("TimelinePlayer 구간 반복");
    const auto dir = sandbox.getChildFile ("stems_loop");
    const int N = 48000;
    EXPECT (writeWav (dir.getChildFile ("ch01_dry.wav"), makeRamp (N), 48000.0));

    TimelinePlayer p;
    juce::String err;
    EXPECT (p.load (dir, 512, 48000.0, err));

    const juce::int64 A = 10000, B = 18000;
    p.setLoop (true, A, B);
    p.setPosition (A);
    EXPECT (p.isLoopEnabled());
    EXPECT (p.getPosition() == A);

    juce::int64 logical = A;      // 되감기를 반영한 기대 위치
    int bad = 0, wraps = 0, idle = 0, outOfRange = 0;

    for (int blk = 0; blk < 400 && idle < 200; ++blk)
    {
        p.readBlock (512);
        const auto now = p.getPosition();

        if (now < A || now >= B)
            ++outOfRange;

        const int consumed = (int) (now >= logical ? now - logical
                                                   : (B - logical) + (now - A));
        if (consumed == 0) { ++idle; juce::Thread::sleep (1); continue; }
        idle = 0;

        const float* d = p.getChannel (0);
        for (int i = 0; i < consumed; ++i)
        {
            auto expected = logical + i;
            if (expected >= B) expected = A + (expected - B);
            if (d[i] != rampValue (expected))
                ++bad;
        }

        if (now < logical) ++wraps;
        logical = now;
    }

    EXPECT (bad == 0);            // 위치와 소리가 끝까지 일치
    EXPECT (outOfRange == 0);     // 구간 밖으로 나가지 않음
    EXPECT (wraps >= 2);          // 8000 샘플 구간을 400블록(=204800) 소비 → 여러 번 되감김

    // ── 반복 해제 → 구간 끝을 넘어 계속 진행 ──
    p.setLoop (false, A, B);
    EXPECT (! p.isLoopEnabled());
    p.setPosition (B - 1000);
    int bad2 = 0;
    const auto endPos = consumeAndVerify (p, 512, B + 4000, 0, bad2);
    EXPECT (bad2 == 0);
    EXPECT (endPos >= B + 4000);  // 경계를 넘어 진행

    // ── 잘못된 구간(끝<=시작)은 반복 없음으로 취급 ──
    p.setLoop (true, 5000, 5000);
    EXPECT (! p.isLoopEnabled());
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
//==============================================================================
// 8. TimelineSegments — 유효 구간 스위프 (L + 펀치아웃, DESIGN §5.11/§5.12)
//    history 항목 = 대체된 구간 [start,end). 스팬하는 기존 구간은 셋으로 쪼개진다.
//    구버전 테이크(꼬리 절단 시절)는 totalSamples 클램프로 옛 의미와 일치.
//==============================================================================
static void testTimelineSegments()
{
    section ("TimelineSegments — 구간 대체 스위프");

    // 변수명이 sr 이면 네임스페이스 sr:: 을 가린다.
    // static: MSVC 는 constexpr 지역변수를 캡처 없는 람다에서 못 쓴다 (C3493).
    static constexpr double kSR = 48000.0;
    auto smp = [] (double s) { return (juce::int64) (s * kSR); };

    auto entry = [&] (const char* op, double s, double e)
    {
        TakeManager::HistoryEntry h;
        h.op = op; h.startSample = smp (s); h.endSample = smp (e);
        return h;
    };
    auto near = [] (double a, double b) { return std::abs (a - b) <= 1.0e-9; };

    // 이력 없음 / SR 0 → 빈 지도
    EXPECT (sr::computeEffectiveSegments ({}, kSR).isEmpty());
    EXPECT (sr::computeEffectiveSegments ({ entry ("record", 0, 30) }, 0.0).isEmpty());

    // 최초 녹음 하나 — 그대로
    {
        const auto segs = sr::computeEffectiveSegments ({ entry ("record", 0, 30) }, kSR);
        EXPECT (segs.size() == 1);
        EXPECT (near (segs[0].startSec, 0.0) && near (segs[0].endSec, 30.0));
        EXPECT (segs[0].gen == 0 && ! segs[0].isPunch);
    }

    // 핵심: [10,15) 펀치가 [0,30) 을 **셋으로 쪼갠다** — 꼬리 [15,30) 은 옛 회차로 남는다
    {
        juce::Array<TakeManager::HistoryEntry> h;
        h.add (entry ("record", 0, 30));
        h.add (entry ("punch", 10, 15));

        const auto segs = sr::computeEffectiveSegments (h, kSR);
        EXPECT (segs.size() == 3);
        EXPECT (near (segs[0].startSec,  0.0) && near (segs[0].endSec, 10.0) && segs[0].gen == 0);
        EXPECT (near (segs[1].startSec, 10.0) && near (segs[1].endSec, 15.0) && segs[1].gen == 1 && segs[1].isPunch);
        EXPECT (near (segs[2].startSec, 15.0) && near (segs[2].endSec, 30.0) && segs[2].gen == 0);
    }

    // 구버전 테이크 호환 — 같은 이력이라도 꼬리 절단 시절에는 길이가 15초.
    // totalSamples 클램프가 존재하지 않는 [15,30) 을 걸러 옛 의미와 일치시킨다.
    {
        juce::Array<TakeManager::HistoryEntry> h;
        h.add (entry ("record", 0, 30));
        h.add (entry ("punch", 10, 15));

        const auto segs = sr::computeEffectiveSegments (h, kSR, smp (15.0));
        EXPECT (segs.size() == 2);
        EXPECT (near (segs[1].endSec, 15.0) && segs[1].gen == 1);
    }

    // 여러 펀치 — 겹치는 부분만 대체되고 나머지 회차는 살아남는다
    {
        juce::Array<TakeManager::HistoryEntry> h;
        h.add (entry ("record", 0, 30));
        h.add (entry ("punch", 20, 25));
        h.add (entry ("punch",  5, 12));

        const auto segs = sr::computeEffectiveSegments (h, kSR);
        EXPECT (segs.size() == 5);   // [0,5)g0 [5,12)g2 [12,20)g0 [20,25)g1 [25,30)g0
        EXPECT (segs[0].gen == 0 && segs[1].gen == 2 && segs[2].gen == 0
             && segs[3].gen == 1 && segs[4].gen == 0);
        EXPECT (near (segs[1].startSec, 5.0) && near (segs[1].endSec, 12.0));
        EXPECT (near (segs[3].startSec, 20.0) && near (segs[3].endSec, 25.0));
    }

    // 통째로 덮이면 소멸
    {
        juce::Array<TakeManager::HistoryEntry> h;
        h.add (entry ("record", 0, 30));
        h.add (entry ("punch", 10, 12));
        h.add (entry ("punch",  5, 20));   // g1 을 완전히 덮는다

        const auto segs = sr::computeEffectiveSegments (h, kSR);
        for (const auto& g : segs)
            EXPECT (g.gen != 1);
    }

    // 불변식: 시간 오름차순 · 무겹침 · 전체 스팬 보존 (길이는 절대 줄지 않는다)
    {
        juce::Array<TakeManager::HistoryEntry> h;
        h.add (entry ("record", 0, 40));
        h.add (entry ("punch", 30, 36));
        h.add (entry ("punch", 12, 33));
        h.add (entry ("punch", 25, 31));

        const auto segs = sr::computeEffectiveSegments (h, kSR);
        bool ordered = true;
        for (int i = 1; i < segs.size(); ++i)
            if (segs[i].startSec < segs[i - 1].endSec - 1.0e-9)
                ordered = false;
        EXPECT (ordered);
        EXPECT (near (segs.getFirst().startSec, 0.0));
        EXPECT (near (segs.getLast().endSec, 40.0));   // 꼬리 보존 → 스팬 유지

        bool hasG3 = false;
        for (const auto& g : segs)
            if (g.gen == 3)
                hasG3 = near (g.startSec, 25.0) && near (g.endSec, 31.0);
        EXPECT (hasG3);

        // 길이 0 커밋은 지도를 바꾸지 않는다
        h.add (entry ("punch", 31, 31));
        EXPECT (sr::computeEffectiveSegments (h, kSR).size() == segs.size());
    }
}

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
static void testBetaGate()
{
    section ("BetaGate (베타 만료 판정 — 순수 로직)");
    const juce::int64 d0 = 20000;   // 임의 epoch 일

    // 최초 실행일 = 0일차: 15일 전체 사용 가능
    auto s = BetaGate::evaluate (d0, d0, d0, 15);
    EXPECT (! s.expired && s.daysLeft == 15);

    // 14일차: 마지막 사용 가능일
    s = BetaGate::evaluate (d0, d0 + 13, d0 + 14, 15);
    EXPECT (! s.expired && s.daysLeft == 1);

    // 15일차: 만료 ("15일 뒤 자동으로 실행 안 됨")
    s = BetaGate::evaluate (d0, d0 + 14, d0 + 15, 15);
    EXPECT (s.expired && s.daysLeft == 0);

    // 훨씬 뒤
    s = BetaGate::evaluate (d0, d0 + 14, d0 + 100, 15);
    EXPECT (s.expired);

    // 시계 되돌림: 마지막 실행일보다 2일 이상 과거 → 만료 처리
    s = BetaGate::evaluate (d0, d0 + 10, d0 + 5, 15);
    EXPECT (s.expired);

    // 하루 되돌림은 허용 (시간대/서머타임 여유)
    s = BetaGate::evaluate (d0, d0 + 6, d0 + 5, 15);
    EXPECT (! s.expired);
}

//==============================================================================
//==============================================================================
static void testPluginSystem (const juce::File& sandbox)
{
    section ("PluginSystem (waves-shell-support)");

    // ── pickByUid: 다중 클래스 목록에서 uid 선택 ─────────────────────────────
    std::vector<juce::PluginDescription> types;
    {
        juce::PluginDescription a; a.name = "SubA"; a.uniqueId = 111;
        juce::PluginDescription b; b.name = "SubB"; b.uniqueId = 222; b.deprecatedUid = 22;
        types.push_back (a);
        types.push_back (b);
    }
    EXPECT (ChannelStrip::pickByUid (types, 0)   == &types[0]);   // uid 미기록(구세션) → 첫 항목
    EXPECT (ChannelStrip::pickByUid (types, 222) == &types[1]);   // uniqueId 일치
    EXPECT (ChannelStrip::pickByUid (types, 22)  == &types[1]);   // deprecatedUid 일치
    EXPECT (ChannelStrip::pickByUid (types, 999) == nullptr);     // 불일치 → 폴백 유도
    EXPECT (ChannelStrip::pickByUid ({}, 0)      == nullptr);     // 빈 목록

    // ── scanPath: 실패(부재) 결과도 캐시 — 반복 스캔 없음 ────────────────────
    juce::AudioPluginFormatManager fm;
    PluginScanCache cache;
    ChannelStrip strip (fm, cache);
    const auto missing = sandbox.getChildFile ("nope.vst3").getFullPathName();
    const auto& r1 = strip.scanPath (missing);
    EXPECT (r1.empty());
    EXPECT (cache.byPath.count (missing) == 1);
    EXPECT (&r1 == &strip.scanPath (missing));   // 동일 캐시 엔트리 (재스캔 없음)

    // ── PluginCatalog: save→load 라운드트립 + 블랙리스트 보존 ────────────────
    {
        PluginCatalog cat;                        // SUPERRACK_APPDATA 샌드박스 사용
        juce::PluginDescription d;
        d.name = "FakeShell Sub";
        d.pluginFormatName = "VST3";
        d.manufacturerName = "Waves";
        d.uniqueId = 12345;
        d.fileOrIdentifier = "C:/fake/shell.vst3";
        cat.list().addType (d);
        cat.list().addToBlacklist ("C:/fake/bad.vst3");
        cat.save();
    }
    {
        PluginCatalog cat2;
        EXPECT (cat2.list().getNumTypes() == 1);
        EXPECT (cat2.list().getTypes()[0].uniqueId == 12345);
        EXPECT (cat2.list().getBlacklistedFiles().contains ("C:/fake/bad.vst3"));
    }

    // ── 브라우저 검색 필터: 이름/제조사 부분 일치 ────────────────────────────
    juce::PluginDescription w;
    w.name = "CLA-76";
    w.manufacturerName = "Waves";
    EXPECT (PluginCatalog::matchesFilter (w, ""));
    EXPECT (PluginCatalog::matchesFilter (w, "cla"));
    EXPECT (PluginCatalog::matchesFilter (w, "WAVES"));
    EXPECT (! PluginCatalog::matchesFilter (w, "fabfilter"));
}

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
    testPluginSystem (sandbox);     // waves-shell-support: 다중 클래스 스캔/uid/카탈로그
    testRecorder (sandbox);
    testTakeManager (sandbox);
    testTimelinePlayer (sandbox);
    testTimelinePlayerLoop (sandbox);
    testTimelineSegments();         // 순수 함수 — 파일 불필요
    testWorkerPool();
    testBetaGate();                 // 순수 판정만 — checkAndTouch 는 실제 APPDATA/레지스트리를 써서 제외

    std::cout << "\n결과: " << gPass << " passed, " << gFail << " failed\n";
    return gFail == 0 ? 0 : 1;
}
