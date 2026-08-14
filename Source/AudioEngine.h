#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "AudioWorkerPool.h"
#include "ChannelStrip.h"
#include "MultitrackRecorder.h"
#include "TimelinePlayer.h"
#include "TakeManager.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

//==============================================================================
/** ASIO 입출력 엔진 (Phase 1).

    채널마다 ChannelStrip(VST3 체인)을 두고, 입력 i → 체인 → 출력 i 로 1:1 송출.
    채널별 입력 peak 을 atomic 으로 기록해 GUI 가 폴링한다. UI 로직은 분리.
*/
class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::ChangeListener
{
public:
    /** 미터링/패스스루 최대 채널 수. (32채널 확장 대비) */
    static constexpr int maxChannels = 32;

    AudioEngine();
    ~AudioEngine() override;

    void initialise();
    void shutdown();

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    /** 채널 입력 peak (0.0~) — GUI 스레드에서 폴링. */
    float getInputPeak (int channel) const noexcept;

    /** 채널 VST3 체인 접근 (GUI). strips 는 maxChannels 개 미리 생성(주소 안정). */
    int getNumStrips() const noexcept { return (int) strips.size(); }
    ChannelStrip* getStrip (int channel) noexcept;

    /** 현재 장치가 여는 활성 채널 수(입출력 min, 1:1). GUI 행/녹음 개수 기준. */
    int getActiveChannelCount() const noexcept;

    /** 활성 채널 수가 바뀌면 호출(메시지 스레드). GUI 가 행을 다시 만든다. */
    std::function<void()> onChannelCountChanged;

    //==========================================================================
    // ── 루프백 라운드트립 레이턴시 테스트 ────────────────────────────────────
    // 출력 outChannel 에 임펄스를 송출하고 입력 inChannel 에서 도착을 검출해
    // 실측 왕복 지연(샘플)을 측정한다. 측정 중엔 모든 출력을 무음 처리한다.
    void startLatencyTest (int outChannel, int inChannel) noexcept;

    enum LatencyState { ltIdle = 0, ltMeasuring = 1, ltDone = 2, ltTimeout = 3 };
    int  getLatencyState()        const noexcept { return ltState.load (std::memory_order_acquire); }
    int  getLatencyResultSamples() const noexcept { return ltResult.load (std::memory_order_acquire); }

    /** 드라이버가 보고하는 입력+출력 레이턴시 합(샘플). 없으면 -1. */
    int    getDriverRoundTripSamples() noexcept;
    double getCurrentSampleRate() noexcept;

    //==========================================================================
    // ── Dry 멀티트랙 녹음 (Phase 2) ─────────────────────────────────────────
    /** 활성 채널 수만큼 Dry 녹음 시작. 실패 시 error 채우고 false (메시지 스레드). */
    bool startRecording (juce::String& error);
    void stopRecording();
    bool isRecording()        const noexcept { return recorder.isRecording(); }
    int  getRecordXruns()     const noexcept { return recorder.getXrunCount(); }
    double getRecordSeconds() const noexcept { return recorder.getRecordedSeconds(); }
    juce::File getLastRecordDir() const { return recorder.getLastSessionDir(); }

    //==========================================================================
    // ── 통합 타임라인 트랜스포트 (녹음/재생) ────────────────────────────────
    // tsPunchPass = 펀치 패스(§5.12 개정): 재생이 주도하고 녹음은 구간 창에서만 실효.
    // 프리롤(부르며 들어옴) → 구간 진입 시 녹음 → 구간 통과 후에도 재생 계속 → ■ 에서 커밋.
    enum TransportState { tsStopped = 0, tsRecording = 1, tsPlaying = 2, tsPunchPass = 3 };
    int  getTransportState() const noexcept { return transport.load (std::memory_order_acquire); }

    bool transportRecord (juce::String& error);   // 녹음 시작
    bool transportPlay   (juce::String& error);   // 최근 테이크 재생
    void transportStop();                          // 정지(녹음 마감/재생 정지)
    void transportRewind();                        // 처음으로
    void transportToEnd();                         // 끝으로
    /** 재생이 콜백에서 자동 종료됐을 때 playhead 를 재생 위치로 동기화(GUI 호출). */
    void syncPlayheadFromPlayer() noexcept;

    /** 진행바 시크(초). 재생 위치 이동. */
    void   setPlayPositionSeconds (double s);
    double getTimelineSeconds()       const;   // 현재 위치(초)
    double getTimelineLengthSeconds() const;   // 전체 길이(초)
    bool   isTakeLoaded() const noexcept { return player.isLoaded(); }

    //== 구간 반복 (가상 리허설) ==============================================
    /** 반복 구간 설정(초). end<=start 면 구간 없음 = 반복 꺼짐.
        재생 중 호출하면 현재 위치가 구간 밖일 때만 구간 시작으로 점프한다. */
    void   setLoopRange (double startSec, double endSec);
    void   setLoopEnabled (bool b);
    bool   isLoopEnabled()      const noexcept { return loopEnabled; }
    bool   hasLoopRange()       const noexcept { return loopEndSec > loopStartSec; }
    double getLoopStartSeconds() const noexcept { return loopStartSec; }
    double getLoopEndSeconds()   const noexcept { return loopEndSec; }

    /** 재생 시 채널 VST3 체인 통과 여부(옵션). */
    void setPlaybackThroughChain (bool b) noexcept { playThroughChain.store (b, std::memory_order_relaxed); }
    bool isPlaybackThroughChain() const noexcept   { return playThroughChain.load (std::memory_order_relaxed); }

    //== 채널 M/S/R (DESIGN §5.12) ============================================
    // M/S 는 **재생 스템 전용** — 라이브 입력 경로는 불가침. 솔로는 가산식.
    // 오디오 스레드는 atomic 마스크만 읽는다. R 은 다음 녹음이 덮어쓸 채널(기본 전 채널).
    void setChannelMuted  (int ch, bool b) noexcept { setMaskBit (muteMask, ch, b); }
    void setChannelSoloed (int ch, bool b) noexcept { setMaskBit (soloMask, ch, b); }
    void setChannelArmed  (int ch, bool b) noexcept { setMaskBit (armMask,  ch, b); }
    bool isChannelMuted  (int ch) const noexcept { return ((muteMask.load (std::memory_order_relaxed) >> (juce::uint32) ch) & 1u) != 0; }
    bool isChannelSoloed (int ch) const noexcept { return ((soloMask.load (std::memory_order_relaxed) >> (juce::uint32) ch) & 1u) != 0; }
    bool isChannelArmed  (int ch) const noexcept { return ((armMask.load  (std::memory_order_relaxed) >> (juce::uint32) ch) & 1u) != 0; }

    //== ⏺ 구간녹음 (로케이터 펀치 패스, §5.12) ================================
    /** on + 구간 존재 시 ● 는 펀치 패스를 시작한다: 커서에서 재생 시작(프리롤),
        구간 [in,out) 만 armed 채널에 녹음(경계는 커밋에서 샘플 정확 절단),
        out 을 지나도 재생은 계속(포스트롤) — ■ 로 끝낸다. */
    void setPunchRecordEnabled (bool b) noexcept { punchRecord = b; }
    bool isPunchRecordEnabled() const noexcept   { return punchRecord; }
    /** 이번 패스의 실효 구간(부분 펀치 반영) — GUI 상태 표기용. */
    double getPunchInSeconds()  const noexcept { return punchEffInSec; }
    double getPunchOutSeconds() const noexcept { return punchEffOutSec; }
    /** 펀치 패스가 테이크 끝에 닿았는지 — GUI 타이머가 확인하고 true 면 transportStop()
        (커밋은 메시지 스레드 몫이라 콜백이 직접 정지하지 못한다). */
    bool shouldAutoStopRecording() const noexcept
    {
        return transport.load (std::memory_order_acquire) == tsPunchPass
            && player.isLoaded()
            && player.getPosition() >= player.getTotalSamples();
    }

    //==========================================================================
    // ── 테이크 (녹음 프로젝트) ──────────────────────────────────────────────
    /** 새 빈 테이크 생성 → 현재 테이크 (세션·환경 스냅샷 포함). */
    void newTake();
    juce::Array<TakeManager::TakeInfo> listTakes() { return takeMgr.listTakes(); }
    juce::File getCurrentTakeDir() const { return currentTakeDir; }
    /** 현재 테이크 메타(이력 포함) — UI 시각화용, 테이크 변경 시에만 호출(파일 읽음). */
    TakeManager::TakeInfo getCurrentTakeInfo() const { return takeMgr.readTake (currentTakeDir); }
    /** 과거 테이크를 현재로 로드 → 세션 자동복원. 환경 불일치 시 warning 채움. */
    void loadTake (const juce::File& takeDir, juce::String& warning);
    /** 테이크 폴더 삭제(영구). 현재 테이크면 정지·언로드 후 다른 테이크로 전환. */
    void deleteTake (const juce::File& takeDir);
    /** 테이크 목록/현재 테이크가 바뀌면 호출(메시지 스레드). */
    std::function<void()> onTakesChanged;

    // ── 녹음 undo/redo (1단계 — 커밋 직전 상태로 복구/재적용) ────────────────
    bool canUndoRecording() const { return currentTakeDir.isDirectory() && takeMgr.hasUndoState (currentTakeDir); }
    /** true = 다음 동작이 "실행취소"(직전 복구), false = "다시실행". */
    bool undoIsRestore() const { return takeMgr.isCurrentNewer (currentTakeDir); }
    /** 현재 테이크를 커밋 직전 상태와 스왑(정지 후). 실패 시 error 채우고 false. */
    bool undoRecording (juce::String& error);

    /** 오디오 CPU 사용률(0~1)과 장치 xrun(드롭아웃) 카운트 — GUI 표면화. */
    double getCpuUsage()    const { return deviceManager.getCpuUsage(); }
    int    getDeviceXRuns() const noexcept { return deviceManager.getXRunCount(); }
    /** 재생 스트리밍 언더런(디스크 지연으로 무음 낸 블록 수) — GUI 표면화. */
    int    getPlayerUnderruns() const noexcept { return player.getUnderrunCount(); }

    //==========================================================================
    // ── DSP 부하 실측 + 합성 부하 프로파일링 (Phase 4) ──────────────────────
    // 콜백 전체(체인+녹음 tap+합성 부하)의 벽시계 시간 / 버퍼 예산 = DSP 부하.
    // getCpuUsage() 보다 정밀(피크·예산 초과 횟수 포함). GUI/프로파일러가 폴링.
    float getDspLoadAvg()  const noexcept { return dspAvg.load  (std::memory_order_relaxed); }
    float getDspLoadPeak() const noexcept { return dspPeak.load (std::memory_order_relaxed); }
    int   getDspOverruns() const noexcept { return dspOverruns.load (std::memory_order_relaxed); }
    void  resetDspLoadStats() noexcept;   // peak·overrun 리셋 (avg 는 EMA 유지)

    /** 합성 부하 채널 수(총 채널 목표, 0=끔). 물리 채널 이후 strips[routed..N) 을
        입력 복제로 실제 처리(출력 폐기)해 다채널 CPU 부하를 재현한다. */
    void setSyntheticChannels (int totalChannels) noexcept
        { synthChannels.store (juce::jlimit (0, maxChannels, totalChannels), std::memory_order_relaxed); }
    int  getSyntheticChannels() const noexcept { return synthChannels.load (std::memory_order_relaxed); }

    /** Ch1 체인을 strips[active..totalChannels) 에 복제(메시지 스레드, 프로파일 준비). */
    void prepareSyntheticStrips (int totalChannels, juce::StringArray& errors);
    /** 합성 채널 체인 제거(프로파일 종료 시). */
    void clearSyntheticStrips();

    //==========================================================================
    // ── 채널 병렬 DSP (A2) ─────────────────────────────────────────────────
    void setParallelDsp (bool b) noexcept { workerPool.setEnabled (b); }
    bool isParallelDsp() const noexcept   { return workerPool.isEnabled(); }
    int  getNumDspWorkers() const noexcept { return workerPool.getNumWorkers(); }

    // ── A3 스파이크 계측 (프로파일러가 사용, 메시지 스레드) ──────────────────
    void resetDspSpikeCapture() noexcept { workerPool.resetSpikeCapture(); }
    bool readDspSpikeSnapshot (AudioWorkerPool::SpikeSnapshot& out) const noexcept
        { return workerPool.readSpikeSnapshot (out); }

    //==========================================================================
    // ── 세션 저장/로드 (Phase 3) ────────────────────────────────────────────
    /** 현재 채널 체인+게인을 %APPDATA%/Superrack/session.json 에 자동 저장. */
    void autoSaveSession();
    /** 임의 .superrack 파일로 저장/로드 (수동). */
    bool saveSessionToFile (const juce::File& file);
    bool loadSessionFromFile (const juce::File& file, juce::StringArray& errors);

    //==========================================================================
    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    // ChangeListener — 장치 설정이 바뀔 때마다 저장
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    /** 장치 설정 저장 파일 (%APPDATA%/Superrack/audio-settings.xml). */
    static juce::File getSettingsFile();
    void saveSettings();

    static juce::File getSessionFile();
    juce::var getSessionVar();
    void applySessionVar (const juce::var& root, juce::StringArray& errors);

    int lastChannelCount = -1;   // 변화 감지용 (메시지 스레드)

    // 구간 반복 (메시지 스레드 소유 — player 로 내려보내는 값의 원본)
    bool   loopEnabled  = false;
    double loopStartSec = 0.0;
    double loopEndSec   = 0.0;
    void   applyLoopToPlayer();

    juce::AudioDeviceManager        deviceManager;
    juce::AudioPluginFormatManager  formatManager;
    PluginScanCache                 scanCache;        // R3: 전 스트립 공유 스캔 캐시
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    MultitrackRecorder              recorder;
    TimelinePlayer                  player;

    std::atomic<int>  transport        { tsStopped };
    std::atomic<bool> playThroughChain { true };

    TakeManager takeMgr;
    juce::File   currentTakeDir;       // 재생/펀치인 기준 테이크
    double       punchInSeconds = 0.0; // 이번 녹음의 시작 위치(펀치인)

    // ── 펀치 패스 상태 (§5.12 개정) — 메시지 스레드 기록, 콜백은 atomic 창만 읽음 ──
    double passStartSec   = 0.0;   // 패스(재생) 시작 위치 = 커서
    double punchEffInSec  = 0.0;   // 실효 in (커서가 구간 안이면 커서 = 부분 펀치)
    double punchEffOutSec = 0.0;
    std::atomic<juce::int64> punchWinStart { 0 }, punchWinEnd { 0 };   // 장치 샘플 도메인
    bool startPunchPass (juce::String& error);
    void reloadTakeAtPlayhead();

    // ── 채널 M/S/R 마스크 (§5.12) — 비트 = 채널. 오디오 스레드는 load 만. ──
    static void setMaskBit (std::atomic<juce::uint32>& m, int ch, bool b) noexcept
    {
        if (! juce::isPositiveAndBelow (ch, 32))
            return;
        if (b) m.fetch_or  (1u << (juce::uint32) ch, std::memory_order_relaxed);
        else   m.fetch_and (~(1u << (juce::uint32) ch), std::memory_order_relaxed);
    }
    std::atomic<juce::uint32> muteMask { 0 }, soloMask { 0 };
    std::atomic<juce::uint32> armMask { 0xffffffffu };   // 기본 전 채널 armed = 현행 동작
    bool punchRecord = false;          // ⏺ 구간녹음 토글 (메시지 스레드)
    double       playheadSeconds = 0.0;// 정지 시 유지되는 타임라인 위치
    double       currentTakeLengthSeconds = 0.0;   // 재생기 미로드 시 길이 표시용

    static juce::File getRecTmpDir();          // 녹음 스크래치 폴더
    TakeManager::TakeEnv currentEnv();         // 현재 장치 환경 스냅샷
    void ensureCurrentTake();                  // 현재 테이크 없으면 생성
    void refreshTakeLength();                  // currentTakeDir 길이 캐시 갱신

    std::array<std::atomic<float>, maxChannels> inputPeaks;

    // 레이턴시 테스트 — GUI↔오디오 공유(atomic)
    std::atomic<bool> ltRequested { false };
    std::atomic<int>  ltState     { ltIdle };
    std::atomic<int>  ltResult    { -1 };
    std::atomic<int>  ltOutCh     { 0 };
    std::atomic<int>  ltInCh      { 1 };
    // 오디오 스레드 전용
    long long ltSampleCounter = 0;
    long long ltEmitSample    = -1;
    bool      ltEmitted       = false;
    long long ltTimeoutSamples = 48000;

    void runLatencyBlock (const float* const* in, int numIn,
                          float* const* out, int numOut, int numSamples) noexcept;

    /** 콜백 본체 (audioDeviceIOCallbackWithContext 가 DSP 부하 타이밍으로 감싼다). */
    void processAudioBlock (const float* const* inputChannelData, int numInputChannels,
                            float* const* outputChannelData, int numOutputChannels,
                            int numSamples) noexcept;

    void resetPeaks() noexcept;
    /** 블록 peak(SIMD) 을 채널 미터 atomic 에 기록. 오디오 스레드 전용. */
    void updateInputPeak (int channel, const float* data, int numSamples) noexcept;

    // ── 채널 병렬 DSP (A2) ──
    AudioWorkerPool workerPool;
    std::array<AudioWorkerPool::Job, maxChannels> jobScratch;   // 오디오 스레드 전용

    // ── DSP 부하 측정 + 합성 부하 (Phase 4) ──
    std::atomic<int>   synthChannels { 0 };
    juce::AudioBuffer<float> synthScratch;     // maxChannels x maxBlock — 합성 채널별 출력 폐기 버퍼
                                               // (병렬 처리 시 채널 간 공유 금지)
    double             cbSampleRate = 0.0;     // 오디오 스레드 전용 (aboutToStart 에서 설정)
    float              dspAvgLocal  = 0.0f;    // 오디오 스레드 전용 EMA
    std::atomic<float> dspAvg      { 0.0f };
    std::atomic<float> dspPeak     { 0.0f };
    std::atomic<int>   dspOverruns { 0 };      // 버퍼 예산 초과(load>1) 콜백 수

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
