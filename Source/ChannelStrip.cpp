#include "ChannelStrip.h"
#include "AppSettings.h"
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
const std::vector<juce::PluginDescription>& ChannelStrip::scanPath (const juce::String& path)
{
    // R3: 경로당 1회만 파일 스캔 — 여러 채널에 같은 플러그인 로드 시(세션 복원·
    // 프로파일 체인 복제) 반복 스캔을 제거한다. 빈 결과도 캐시(반복 실패 방지).
    // Design Ref: §3.1 — WaveShell 등 다중 클래스 파일은 목록 전체를 보존한다.
    if (const auto it = scanCache.byPath.find (path); it != scanCache.byPath.end())
        return it->second;

    juce::OwnedArray<juce::PluginDescription> types;
    juce::VST3PluginFormat fmt;
    fmt.findAllTypesForFile (types, path);

    auto& cached = scanCache.byPath[path];    // map 노드는 이후 삽입에도 참조 안정
    cached.reserve ((size_t) types.size());
    for (const auto* t : types)
        cached.push_back (*t);
    return cached;
}

const juce::PluginDescription* ChannelStrip::pickByUid (const std::vector<juce::PluginDescription>& types,
                                                        int uid, const juce::String& name) noexcept
{
    // 이름을 함께 대조하는 것이 핵심이다. JUCE 는 실제 인스턴스화 시
    // VST3ModuleHandle::findClassMatchingDescription 에서 **이름과 uid 를 모두**
    // 요구한다. 여기서 uid 만 보고 고르면 WaveShell 처럼 클래스가 수백 개인
    // 파일에서 엉뚱한 서브플러그인이 선택될 수 있다 — 로드는 성공하므로
    // 사용자에게는 "플러그인이 조용히 바뀐" 것으로 보인다.
    if (types.empty())
        return nullptr;

    const bool haveUid  = uid != 0;
    const bool haveName = name.isNotEmpty();

    // ① uid + 이름 동시 일치 — 정상 경로.
    if (haveUid && haveName)
        for (const auto& d : types)
            if ((d.uniqueId == uid || d.deprecatedUid == uid) && d.name == name)
                return &d;

    // ② 이름만 일치 — 쉘 버전업으로 클래스 uid 가 바뀐 경우.
    if (haveName)
        for (const auto& d : types)
            if (d.name == name)
                return &d;

    // ③ uid 만 일치 — 이름을 기록하지 않은 구세션. 이름이 있는데 못 찾은
    //    경우는 여기 오지 않는다(위에서 이미 실패 = 다른 플러그인일 위험).
    if (haveUid && ! haveName)
        for (const auto& d : types)
            if (d.uniqueId == uid || d.deprecatedUid == uid)
                return &d;

    // ④ 단서가 전혀 없으면 단일 클래스 파일에 한해 첫 항목. 다중 클래스에서
    //    첫 항목을 집으면 임의의 플러그인이 되므로 실패시킨다.
    if (! haveUid && ! haveName && types.size() == 1)
        return &types.front();

    return nullptr;
}

bool ChannelStrip::findByFallback (const juce::String& originalPath, int uid,
                                   const juce::String& name, juce::PluginDescription& out)
{
    juce::VST3PluginFormat fmt;
    auto locations = fmt.getDefaultLocationsToSearch();

    // 사용자 지정 VST3 추가 경로 (앱 설정) — 표준 위치보다 먼저 검색.
    const auto extras = AppSettings::get().vst3ExtraPaths();
    for (int i = extras.size(); --i >= 0;)
        if (juce::File dir (extras[i]); dir.isDirectory())
            locations.add (dir, 0);

    // ① 빠른 경로: 표준 위치에서 같은 파일명(.vst3 는 파일 또는 번들 폴더).
    //    다른 드라이브/폴더에 설치된 흔한 케이스를 전체 스캔 없이 해결.
    //    다중 클래스 파일도 pickByUid 로 해당 서브플러그인을 찾는다 (uid==0 → 첫 항목).
    if (const auto fileName = juce::File (originalPath).getFileName(); fileName.isNotEmpty())
    {
        for (int i = 0; i < locations.getNumPaths(); ++i)
        {
            const auto dir = locations[i];
            if (! dir.isDirectory())
                continue;

            for (const auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories,
                                                     true, fileName))
                if (const auto* d = pickByUid (scanPath (f.getFullPathName()), uid, name))
                {
                    out = *d;
                    return true;
                }
        }
    }

    // ② uid 전체 스캔: 파일명까지 바뀐 경우(Waves 쉘 버전업 V15→V16 등). 표준 위치의
    //    모든 VST3 를 스캔하므로 느릴 수 있으나(1회성 폴백, scanCache 로 중복 제거)
    //    세션 유실보다 낫다.
    if (uid != 0 || name.isNotEmpty())
        for (const auto& id : fmt.searchPathsForPlugins (locations, true, false))
            if (const auto* d = pickByUid (scanPath (id), uid, name))
            {
                out = *d;
                return true;
            }

    return false;
}

std::shared_ptr<ChannelStrip::Node> ChannelStrip::makeNode (const juce::String& path, bool bypass,
                                                            const juce::String& base64State,
                                                            juce::String& err,
                                                            int uid, const juce::String& displayName)
{
    juce::PluginDescription desc;

    // ⓪ 카탈로그 조회 우선 — 아웃오브프로세스 스캔 결과라 이름·uid 가 정확하고
    //    파일 스캔이 없다. 인프로세스 findAllTypesForFile 은 WaveShell 같은 대형
    //    쉘에서 수십 초가 걸리고 결과도 불완전해, 기동 복원이 여기 걸리면
    //    창이 뜨기 전에 앱이 멈춘 것처럼 보인다.
    if (scanCache.resolveDescription != nullptr
        && scanCache.resolveDescription (path, uid, displayName, desc))
    {
        // 찾음 — 아래 스캔 경로를 건너뛴다.
    }
    else if (const auto* d = pickByUid (scanPath (path), uid, displayName))
    {
        desc = *d;
    }
    // 경로 유실(다른 머신 세션) 또는 경로는 살았지만 uid 불일치(쉘 버전업으로
    // 클래스 구성 변경) — uid/파일명으로 재탐색. Design Ref: §4.1
    else if (! findByFallback (path, uid, displayName, desc))
    {
        // 다중 클래스 파일을 단서 없이(파일에서 추가) 연 경우는 원인이 다르다 —
        // 첫 항목을 임의로 집지 않고 브라우저를 쓰도록 안내한다.
        if (uid == 0 && displayName.isEmpty() && scanPath (path).size() > 1)
            err = u8 ("플러그인이 여러 개 든 파일입니다 — 브라우저에서 선택하세요: ")
                  + juce::File (path).getFileName();
        else
            err = u8 ("플러그인을 찾지 못했습니다(경로·재탐색 모두 실패): ")
                  + (displayName.isNotEmpty() ? displayName
                                              : juce::File (path).getFileName());
        return nullptr;
    }

    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int    bs = maxBlock   > 0   ? maxBlock   : 512;

    // 만들어진 인스턴스가 정말 요청한 그 플러그인인지 확인한다. WaveShell 처럼
    // 클래스 목록을 지연 초기화하는 쉘은 **첫 인스턴스화에서 엉뚱한 서브플러그인을
    // 돌려준다** — desc(이름·uid·경로)가 정확해도 그렇다. 모듈이 한 번 로드되고
    // 나면 다음 시도는 맞으므로 몇 번 재시도한다.
    // (실측: Curves Resolve Stereo 요청 → 1차 Magma Lil Tube Stereo, 2차 정상.
    //  세션 복원에서 ch1 만 틀리고 ch2 는 맞던 비대칭도 이 순서 효과였다.)
    juce::String e;
    std::unique_ptr<juce::AudioPluginInstance> inst (
        formats.createPluginInstance (desc, sr, bs, e));

    if (inst == nullptr)
    {
        // FR-08: 이름 포함 — 미라이선스 Waves 서브플러그인이 대표 실패 케이스.
        err = u8 ("플러그인 로드 실패: ") + desc.name + u8 (" — ") + e;
        return nullptr;
    }

    // 인스턴스가 보고하는 이름이 요청과 다른 경우가 있다 — WaveShell 실측:
    // Curves Resolve Stereo 를 요청했는데 Magma Lil Tube Stereo 로 보고한다.
    // 즉시 재시도(4회)·메시지 루프 진입 후 지연 복원 모두 효과 없었고, 같은
    // desc 로 브라우저에서 추가하면 정상이다 — **원인 미확정**.
    // 로드 자체는 성공하므로 체인에는 넣되 경고로 표면화한다.
    if (displayName.isNotEmpty()
        && ! inst->getName().trim().equalsIgnoreCase (displayName.trim()))
        err = u8 ("⚠ 이름 불일치 — 요청: ") + displayName
              + u8 (" / 인스턴스 보고: ") + inst->getName()
              + u8 (" (로드는 진행했습니다. 소리가 다르면 지우고 다시 추가하세요.)");

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
    // 폴백으로 찾았으면 로컬 경로로 자기치유 — 다음 저장부터 이 머신 경로가 기록됨.
    node->filePath = desc.fileOrIdentifier.isNotEmpty() ? desc.fileOrIdentifier : path;
    node->uid = desc.uniqueId != 0 ? desc.uniqueId : desc.deprecatedUid;
    // 요청한 desc 의 이름을 보관 — UI 표시와 세션 저장의 기준. getName() 은
    // 위 주석대로 신뢰할 수 없어 세션 조회 키로 쓰면 오염이 누적된다.
    node->descName = desc.name.isNotEmpty() ? desc.name : node->plugin->getName();
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

bool ChannelStrip::addPlugin (const juce::PluginDescription& desc, juce::String& errorMessage)
{
    // 브라우저 선택은 uid 를 명시해 다중 클래스 파일(WaveShell)에서 정확한
    // 서브플러그인을 로드한다. Plan SC-1.
    const int uid = desc.uniqueId != 0 ? desc.uniqueId : desc.deprecatedUid;
    auto node = makeNode (desc.fileOrIdentifier, false, {}, errorMessage, uid, desc.name);
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
            const int  uid   = (int) e.getProperty ("uid", 0);        // 구버전 세션엔 없음(0)
            const auto pname = e.getProperty ("name", "").toString();

            juce::String err;
            auto n = makeNode (path, byp, state, err, uid, pname);

            // err 는 실패뿐 아니라 "로드했지만 이상함" 경고로도 채워진다.
            if (err.isNotEmpty())
                errors.add (err);

            if (n != nullptr)
                newList.push_back (std::move (n));
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
        return editList[(size_t) index]->descName;
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
        p->setProperty ("uid",    n->uid);                    // 머신 독립 식별자 (경로 폴백용)
        p->setProperty ("name",   n->descName);                // 복원 조회 키 + 에러 표시용
        p->setProperty ("bypass", n->bypassed.load (std::memory_order_relaxed));

        juce::MemoryBlock mb;
        n->plugin->getStateInformation (mb);
        p->setProperty ("state", mb.toBase64Encoding());

        plugins.add (juce::var (p));
    }
    obj->setProperty ("plugins", plugins);
    return juce::var (obj);
}
