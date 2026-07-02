#include "ChannelStrip.h"
#include "Util.h"

using sr::u8;

//==============================================================================
ChannelStrip::ChannelStrip (juce::AudioPluginFormatManager& formatManager, PluginScanCache& cache)
    : formats (formatManager), scanCache (cache)
{
}

ChannelStrip::~ChannelStrip()
{
    releaseResources();
}

//==============================================================================
void ChannelStrip::prepare (double newSampleRate, int newMaxBlock)
{
    // 장치 콜백 시작 전(메시지 스레드) — RT 접근 없음.
    sampleRate = newSampleRate;
    maxBlock   = newMaxBlock;

    stereoBuffer.setSize (2, juce::jmax (1, maxBlock), false, false, true);
    midiScratch.ensureSize (256);   // 사전 확보 — 오디오 스레드에서 재할당 방지

    for (auto& n : editList)
        prepareNode (*n);
}

void ChannelStrip::releaseResources()
{
    // 장치 정지 후(메시지 스레드) — RT 접근 없음.
    for (auto& n : editList)
        if (n->plugin != nullptr)
            n->plugin->releaseResources();
}

void ChannelStrip::prepareNode (Node& node)
{
    if (node.plugin == nullptr || sampleRate <= 0.0 || maxBlock <= 0)
        return;

    node.plugin->setPlayConfigDetails (2, 2, sampleRate, maxBlock);
    node.plugin->prepareToPlay (sampleRate, maxBlock);
}

//==============================================================================
void ChannelStrip::process (const float* in, float* out, int numSamples) noexcept
{
    // 미준비·협상 초과 블록(R2)이면 dry 패스스루.
    // 부분 처리(클램프)는 out 꼬리를 미기록으로 남기므로 블록 전체를 통과시킨다.
    jassert (numSamples <= stereoBuffer.getNumSamples() || maxBlock <= 0);
    if (maxBlock <= 0 || numSamples > stereoBuffer.getNumSamples())
    {
        juce::FloatVectorOperations::copy (out, in, numSamples);
        return;
    }

    // 모노 입력 → 스테레오 내부 버퍼 (L/R 복제)
    float* left  = stereoBuffer.getWritePointer (0);
    float* right = stereoBuffer.getWritePointer (1);
    juce::FloatVectorOperations::copy (left,  in, numSamples);
    juce::FloatVectorOperations::copy (right, in, numSamples);

    float* chans[2] = { left, right };
    juce::AudioBuffer<float> proxy (chans, 2, numSamples);

    midiScratch.clear();

    {
        // D2: wait-free 체인 획득 — 편집 중에도 항상 유효한(직전) 체인을 본다.
        RtChain::ScopedAccess<farbot::ThreadType::realtime> nodes (chain);

        for (const auto& n : *nodes)
            if (n->plugin != nullptr && ! n->bypassed.load (std::memory_order_relaxed))
                n->plugin->processBlock (proxy, midiScratch);
    }

    // 좌채널을 모노 출력으로 (출력 게인 적용)
    const float g = outGainLin.load (std::memory_order_relaxed);
    juce::FloatVectorOperations::copyWithMultiply (out, proxy.getReadPointer (0), g, numSamples);
}

void ChannelStrip::setOutGainDb (float db) noexcept
{
    db = juce::jlimit (-60.0f, 12.0f, db);
    outGainDb.store (db, std::memory_order_relaxed);
    outGainLin.store (juce::Decibels::decibelsToGain (db, -60.0f), std::memory_order_relaxed);
}

//==============================================================================
std::shared_ptr<ChannelStrip::Node> ChannelStrip::makeNode (const juce::String& path, bool bypass,
                                                            const juce::String& base64State,
                                                            juce::String& err)
{
    // R3: 경로당 1회만 파일 스캔 — 여러 채널에 같은 플러그인 로드 시(세션 복원·
    // 프로파일 체인 복제) 반복 스캔을 제거한다.
    juce::PluginDescription desc;
    if (const auto it = scanCache.byPath.find (path); it != scanCache.byPath.end())
    {
        desc = it->second;
    }
    else
    {
        juce::OwnedArray<juce::PluginDescription> types;
        juce::VST3PluginFormat fmt;
        fmt.findAllTypesForFile (types, path);

        if (types.isEmpty())
        {
            err = u8 ("VST3 타입을 찾지 못했습니다: ") + juce::File (path).getFileName();
            return nullptr;
        }

        desc = *types[0];
        scanCache.byPath[path] = desc;
    }

    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int    bs = maxBlock   > 0   ? maxBlock   : 512;

    juce::String e;
    std::unique_ptr<juce::AudioPluginInstance> inst (
        formats.createPluginInstance (desc, sr, bs, e));

    if (inst == nullptr)
    {
        err = u8 ("플러그인 로드 실패: ") + e;
        return nullptr;
    }

    inst->setPlayConfigDetails (2, 2, sr, bs);

    if (inst->getTotalNumInputChannels() > 2 || inst->getTotalNumOutputChannels() > 2)
    {
        err = u8 ("스테레오(2ch) 이하 플러그인만 지원합니다: ") + inst->getName();
        return nullptr;
    }

    inst->prepareToPlay (sr, bs);

    if (base64State.isNotEmpty())
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (base64State))
            inst->setStateInformation (mb.getData(), (int) mb.getSize());
    }

    auto node = std::make_shared<Node>();
    node->plugin = std::move (inst);
    node->filePath = path;
    node->bypassed.store (bypass, std::memory_order_relaxed);
    return node;
}

bool ChannelStrip::addPlugin (const juce::File& vst3File, juce::String& errorMessage)
{
    auto node = makeNode (vst3File.getFullPathName(), false, {}, errorMessage);
    if (node == nullptr)
        return false;

    editList.push_back (std::move (node));
    publishChain();
    return true;
}

void ChannelStrip::removePlugin (int index)
{
    if (index < 0 || index >= (int) editList.size())
        return;

    auto removed = editList[(size_t) index];   // 파괴는 publish 후 이 스코프(메시지 스레드)에서
    editList.erase (editList.begin() + index);
    publishChain();

    if (removed != nullptr && removed->plugin != nullptr)
        removed->plugin->releaseResources();
}

void ChannelStrip::setBypass (int index, bool shouldBypass)
{
    // Node 는 RT 뷰와 공유되므로 atomic 만 바꾸면 즉시 반영 — 스왑 불필요.
    if (index >= 0 && index < (int) editList.size())
        editList[(size_t) index]->bypassed.store (shouldBypass, std::memory_order_relaxed);
}

void ChannelStrip::loadChain (const juce::var& pluginsArray, float gainDb, juce::StringArray& errors)
{
    // 새 노드들을 먼저 만든다(생성·prepare·setState — 오디오는 그동안 기존 체인 유지).
    NodeList newList;
    if (auto* arr = pluginsArray.getArray())
    {
        for (auto& e : *arr)
        {
            const auto path  = e.getProperty ("path", "").toString();
            const bool byp   = (bool) e.getProperty ("bypass", false);
            const auto state = e.getProperty ("state", "").toString();

            juce::String err;
            if (auto n = makeNode (path, byp, state, err))
                newList.push_back (std::move (n));
            else
                errors.add (err);
        }
    }

    NodeList old = std::move (editList);
    editList = std::move (newList);
    publishChain();   // 한 번의 스왑 — 구버전 벡터는 여기(메시지 스레드)서 해제

    for (auto& n : old)
        if (n != nullptr && n->plugin != nullptr)
            n->plugin->releaseResources();

    setOutGainDb (gainDb);
}

//==============================================================================
int ChannelStrip::getNumPlugins() const
{
    return (int) editList.size();
}

juce::String ChannelStrip::getPluginName (int index) const
{
    if (index >= 0 && index < (int) editList.size() && editList[(size_t) index]->plugin != nullptr)
        return editList[(size_t) index]->plugin->getName();
    return {};
}

bool ChannelStrip::isBypassed (int index) const
{
    if (index >= 0 && index < (int) editList.size())
        return editList[(size_t) index]->bypassed.load (std::memory_order_relaxed);
    return false;
}

juce::AudioPluginInstance* ChannelStrip::getPlugin (int index) const
{
    if (index >= 0 && index < (int) editList.size())
        return editList[(size_t) index]->plugin.get();
    return nullptr;
}

//==============================================================================
juce::var ChannelStrip::getStateVar()
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name",      channelName);
    obj->setProperty ("outGainDb", (double) getOutGainDb());

    juce::Array<juce::var> plugins;
    for (auto& n : editList)
    {
        if (n == nullptr || n->plugin == nullptr)
            continue;

        auto* p = new juce::DynamicObject();
        p->setProperty ("path",   n->filePath);
        p->setProperty ("bypass", n->bypassed.load (std::memory_order_relaxed));

        juce::MemoryBlock mb;
        n->plugin->getStateInformation (mb);
        p->setProperty ("state", mb.toBase64Encoding());

        plugins.add (juce::var (p));
    }
    obj->setProperty ("plugins", plugins);
    return juce::var (obj);
}
