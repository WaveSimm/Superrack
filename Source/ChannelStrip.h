#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <farbot/RealtimeObject.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

//==============================================================================
/** VST3 스캔 결과 캐시 (설계 R3): .vst3 경로 → PluginDescription.

    findAllTypesForFile 은 파일 스캔이라 느리다 — 같은 플러그인을 여러 채널에
    로드할 때(세션 복원·프로파일 체인 복제) 경로당 1회만 스캔한다.
    메시지 스레드 전용. AudioEngine 이 소유하고 전 스트립이 공유한다. */
struct PluginScanCache
{
    std::map<juce::String, juce::PluginDescription> byPath;
};

//==============================================================================
/** 한 입력 채널에 적용되는 VST3 직렬 체인 (Phase 1 · D2 재설계).

    - 채널을 스테레오(2ch) 내부 버퍼로 처리: 모노 입력을 L/R 로 복제 → 체인 처리
      → 좌채널을 모노 출력(ASIO Out[ch])으로 보냄. (대부분의 모노/스테레오 FX 호환)
    - 플러그인 추가/제거/순서변경은 **메시지 스레드**에서만.

    체인 동기화 (D2, farbot::RealtimeObject nonRealtimeMutatable):
    - 메시지 스레드가 원본(editList)을 수정한 뒤 publishChain() 으로 복사 스왑.
      스왑은 RT 가 체인을 놓을 때까지 정확히 대기(스핀)하고, 구버전 벡터는
      메시지 스레드에서 해제된다.
    - 오디오/워커 스레드의 획득은 **wait-free** (ScopedAccess<realtime>).
      이전 설계(reconfiguring 플래그 + 대기 가드)와 달리 편집 중에도
      dry 패스스루 갭이 생기지 않는다 — 항상 유효한 체인을 본다.
    - Node 는 shared_ptr — 교체된 노드의 파괴는 항상 메시지 스레드에서 일어난다.

    스레드 규약: process() 는 블록당 한 스레드(오디오 또는 워커 1개)만 호출.
    서로 다른 스트립은 병렬 처리 가능(내부 버퍼가 스트립별로 독립). */
class ChannelStrip
{
public:
    ChannelStrip (juce::AudioPluginFormatManager& formatManager, PluginScanCache& scanCache);
    ~ChannelStrip();

    /** 장치 시작 시 호출(메시지 스레드, 콜백 시작 전). 내부 버퍼/플러그인 prepare. */
    void prepare (double sampleRate, int maxBlockSize);
    void releaseResources();

    //==========================================================================
    // ── 오디오/워커 스레드 ────────────────────────────────────────────────
    /** 모노 in → 체인 → (출력 게인) → 모노 out. 무할당·wait-free 획득.
        협상 초과 블록(R2)·미준비 상태는 dry 패스스루. */
    void process (const float* in, float* out, int numSamples) noexcept;

    /** 출력 게인 (dB). 오디오 스레드에 atomic 반영. GUI/세션에서 호출. */
    void  setOutGainDb (float db) noexcept;
    float getOutGainDb() const noexcept { return outGainDb.load (std::memory_order_relaxed); }

    /** 사용자 지정 채널 이름 (메시지 스레드 전용, 세션 저장). */
    void         setChannelName (const juce::String& n) { channelName = n; }
    juce::String getChannelName() const                 { return channelName; }

    //==========================================================================
    // ── 메시지 스레드 (GUI) ────────────────────────────────────────────────
    /** .vst3 파일을 로드해 체인 끝에 추가. 실패 시 errorMessage 채우고 false. */
    bool addPlugin (const juce::File& vst3File, juce::String& errorMessage);
    void removePlugin (int index);
    void setBypass (int index, bool shouldBypass);

    int  getNumPlugins() const;
    juce::String getPluginName (int index) const;
    bool isBypassed (int index) const;
    /** 네이티브 에디터 호스팅용 — GUI 가 에디터 창을 만든다. */
    juce::AudioPluginInstance* getPlugin (int index) const;

    //==========================================================================
    // ── 세션 직렬화 (메시지 스레드) ─────────────────────────────────────────
    /** 이 채널 상태를 var(DynamicObject)로: { outGainDb, plugins:[{path,bypass,state}] }. */
    juce::var getStateVar();
    /** var 의 plugins 배열 + 게인으로 체인을 통째로 교체(한 번의 스왑). */
    void loadChain (const juce::var& pluginsArray, float gainDb, juce::StringArray& errors);

private:
    struct Node
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        std::atomic<bool> bypassed { false };
        juce::String filePath;        // 세션 복원용 .vst3 경로
    };

    /** shared_ptr 벡터 = 복사 가능(farbot 요구) + 노드 파괴는 마지막 참조 소유
        스레드에서 — publishChain 구조상 항상 메시지 스레드. */
    using NodeList = std::vector<std::shared_ptr<Node>>;
    using RtChain  = farbot::RealtimeObject<NodeList, farbot::RealtimeObjectOptions::nonRealtimeMutatable>;

    /** path+state 로 노드 1개 생성(prepare·setState 포함). 실패 시 nullptr. */
    std::shared_ptr<Node> makeNode (const juce::String& path, bool bypass,
                                    const juce::String& base64State, juce::String& err);
    void prepareNode (Node& node);

    /** editList 를 RT 뷰로 복사 스왑(메시지 스레드). 구버전은 여기서 해제. */
    void publishChain() { chain.nonRealtimeReplace (editList); }

    juce::AudioPluginFormatManager& formats;
    PluginScanCache&                scanCache;         // 전 스트립 공유 (메시지 스레드 전용)

    NodeList editList;   // 메시지 스레드 소유 원본 — GUI 조회·세션 직렬화 대상
    RtChain  chain;      // 오디오/워커 스레드용 RT 뷰

    std::atomic<float> outGainLin { 1.0f };            // 오디오 스레드용 선형 게인
    std::atomic<float> outGainDb  { 0.0f };            // GUI/세션 readback
    juce::String       channelName;                    // 사용자 지정 이름

    double sampleRate = 0.0;
    int    maxBlock   = 0;

    juce::AudioBuffer<float> stereoBuffer;             // 2 x maxBlock (사전 할당)
    juce::MidiBuffer         midiScratch;              // 빈 MIDI 재사용

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
