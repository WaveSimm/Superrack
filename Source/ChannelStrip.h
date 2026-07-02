#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
/** 한 입력 채널에 적용되는 VST3 직렬 체인 (Phase 1).

    - 채널을 스테레오(2ch) 내부 버퍼로 처리: 모노 입력을 L/R 로 복제 → 체인 처리
      → 좌채널을 모노 출력(ASIO Out[ch])으로 보냄. (대부분의 모노/스테레오 FX 호환)
    - 플러그인 추가/제거/순서변경은 **메시지 스레드**에서만.
    - 오디오 콜백은 lock-free: reconfiguring 플래그가 켜진 동안엔 dry 패스스루.
      (메시지 스레드가 플래그를 켜고 잠깐 대기해 in-flight 콜백을 흘려보낸 뒤 체인을 수정)
*/
class ChannelStrip
{
public:
    explicit ChannelStrip (juce::AudioPluginFormatManager& formatManager);
    ~ChannelStrip();

    /** 장치 시작 시 호출(메시지 스레드). 내부 버퍼/플러그인 prepare. */
    void prepare (double sampleRate, int maxBlockSize);
    void releaseResources();

    //==========================================================================
    // ── 오디오 스레드 ──────────────────────────────────────────────────────
    /** 모노 in → 체인 → (출력 게인) → 모노 out. numSamples <= maxBlockSize. 무할당/무락. */
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
    /** var 의 plugins 배열 + 게인으로 체인을 통째로 교체(한 번의 reconfigure). */
    void loadChain (const juce::var& pluginsArray, float gainDb, juce::StringArray& errors);

private:
    struct Node
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        std::atomic<bool> bypassed { false };
        juce::String filePath;        // 세션 복원용 .vst3 경로
    };

    /** path+state 로 노드 1개 생성(prepare·setState 포함). 실패 시 nullptr. */
    std::unique_ptr<Node> makeNode (const juce::String& path, bool bypass,
                                    const juce::String& base64State, juce::String& err);

    juce::AudioPluginFormatManager& formats;

    std::vector<std::unique_ptr<Node>> nodes;          // 메시지 스레드 소유, 오디오 스레드 읽기
    std::atomic<bool> reconfiguring { false };

    std::atomic<float> outGainLin { 1.0f };            // 오디오 스레드용 선형 게인
    std::atomic<float> outGainDb  { 0.0f };            // GUI/세션 readback
    juce::String       channelName;                    // 사용자 지정 이름

    double sampleRate = 0.0;
    int    maxBlock   = 0;

    juce::AudioBuffer<float> stereoBuffer;             // 2 x maxBlock (사전 할당)
    juce::MidiBuffer         midiScratch;              // 빈 MIDI 재사용

    /** reconfiguring 가드로 체인을 안전하게 수정. */
    template <typename Fn>
    void reconfigure (Fn&& fn);

    void prepareNode (Node& node);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
