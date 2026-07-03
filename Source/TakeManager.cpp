#include "TakeManager.h"
#include "AppSettings.h"

//==============================================================================
juce::File TakeManager::takesRoot()
{
    return AppSettings::get().storageRoot().getChildFile ("takes");
}

void TakeManager::ensureFormats() const
{
    if (formatMgr.getNumKnownFormats() == 0)
        formatMgr.registerBasicFormats();
}

juce::File TakeManager::timelineFile (const juce::File& takeDir)
{
    return takeDir.getChildFile ("timeline.json");
}

juce::var TakeManager::readTimeline (const juce::File& takeDir)
{
    return juce::JSON::parse (timelineFile (takeDir));
}

void TakeManager::writeTimeline (const juce::File& takeDir, const juce::var& v)
{
    timelineFile (takeDir).replaceWithText (juce::JSON::toString (v));
}

//==============================================================================
juce::File TakeManager::createTake (const TakeEnv& env, const juce::var& sessionSnapshot)
{
    const auto id = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    auto dir = takesRoot().getChildFile (id);

    // 동일 초에 중복되면 접미사
    int suffix = 1;
    while (dir.exists())
        dir = takesRoot().getChildFile (id + "_" + juce::String (++suffix));

    dir.createDirectory();

    auto* obj = new juce::DynamicObject();
    const auto nowIso = juce::Time::getCurrentTime().toISO8601 (true);
    obj->setProperty ("id",            dir.getFileName());
    obj->setProperty ("createdAt",     nowIso);
    obj->setProperty ("updatedAt",     nowIso);
    obj->setProperty ("deviceName",    env.deviceName);
    obj->setProperty ("sampleRate",    env.sampleRate);
    obj->setProperty ("bufferSize",    env.bufferSize);
    obj->setProperty ("channels",      env.channels);
    obj->setProperty ("lengthSamples", (juce::int64) 0);
    obj->setProperty ("history",       juce::Array<juce::var>{});
    writeTimeline (dir, juce::var (obj));

    // 세션 스냅샷(환경 재현용)
    dir.getChildFile ("session.json").replaceWithText (juce::JSON::toString (sessionSnapshot));

    return dir;
}

TakeManager::TakeInfo TakeManager::readTake (const juce::File& dir) const
{
    TakeInfo t;
    t.dir = dir;
    t.id  = dir.getFileName();

    const auto v = readTimeline (dir);
    if (v.isObject())
    {
        t.createdAt       = v.getProperty ("createdAt", "").toString();
        t.updatedAt       = v.getProperty ("updatedAt", t.createdAt).toString();
        t.env.deviceName  = v.getProperty ("deviceName", "").toString();
        t.env.sampleRate  = (double) v.getProperty ("sampleRate", 0.0);
        t.env.bufferSize  = (int) v.getProperty ("bufferSize", 0);
        t.env.channels    = (int) v.getProperty ("channels", 0);
        t.lengthSamples   = (juce::int64) (double) v.getProperty ("lengthSamples", 0.0);
        if (auto* h = v.getProperty ("history", {}).getArray())
            t.historyCount = h->size();
    }
    return t;
}

juce::var TakeManager::readSession (const juce::File& takeDir) const
{
    return juce::JSON::parse (takeDir.getChildFile ("session.json"));
}

juce::Array<TakeManager::TakeInfo> TakeManager::listTakes() const
{
    juce::Array<TakeInfo> takes;

    juce::Array<juce::File> dirs;
    takesRoot().findChildFiles (dirs, juce::File::findDirectories, false);
    for (auto& d : dirs)
        if (timelineFile (d).existsAsFile())
            takes.add (readTake (d));

    // 최신순 (id = 타임스탬프 문자열이라 역순 정렬)
    std::sort (takes.begin(), takes.end(),
               [] (const TakeInfo& a, const TakeInfo& b) { return a.id > b.id; });
    return takes;
}

//==============================================================================
void TakeManager::commitRecording (const juce::File& takeDir, const juce::File& recTmp,
                                   juce::int64 punchSamples, const TakeEnv& env,
                                   const juce::var& sessionSnapshot)
{
    ensureFormats();
    const double sampleRate = env.sampleRate;

    juce::Array<juce::File> newStems;
    recTmp.findChildFiles (newStems, juce::File::findFiles, false, "ch*_dry.wav");
    newStems.sort();
    if (newStems.isEmpty())
        return;

    juce::AudioBuffer<float> buf (1, 32768);
    juce::int64 finalLen = 0;
    juce::int64 newLen   = 0;

    for (auto& ns : newStems)
    {
        const auto name    = ns.getFileName();
        const auto dstFile = takeDir.getChildFile (name);
        const auto tmpOut  = takeDir.getChildFile (name + ".tmp");

        std::unique_ptr<juce::OutputStream> stream (tmpOut.createOutputStream());
        if (stream == nullptr)
            continue;

        const auto opts = juce::AudioFormatWriterOptions{}
                              .withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (32)
                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (stream, opts));
        if (writer == nullptr)
            continue;

        // 1) head: 기존 테이크 파일 [0..punchSamples), 부족분 무음 패딩
        juce::int64 written = 0;
        if (punchSamples > 0 && dstFile.existsAsFile())
        {
            if (auto* rr = formatMgr.createReaderFor (dstFile))
            {
                std::unique_ptr<juce::AudioFormatReader> old (rr);
                const juce::int64 copyN = juce::jmin (punchSamples, (juce::int64) old->lengthInSamples);
                while (written < copyN)
                {
                    const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), copyN - written);
                    old->read (&buf, 0, n, written, true, false);
                    writer->writeFromAudioSampleBuffer (buf, 0, n);
                    written += n;
                }
            }
            while (written < punchSamples)
            {
                const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), punchSamples - written);
                buf.clear();
                writer->writeFromAudioSampleBuffer (buf, 0, n);
                written += n;
            }
        }

        // 2) 새 녹음 전체 이어붙임
        juce::int64 thisNew = 0;
        if (auto* nr = formatMgr.createReaderFor (ns))
        {
            std::unique_ptr<juce::AudioFormatReader> nw (nr);
            const juce::int64 len = nw->lengthInSamples;
            juce::int64 pos = 0;
            while (pos < len)
            {
                const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), len - pos);
                nw->read (&buf, 0, n, pos, true, false);
                writer->writeFromAudioSampleBuffer (buf, 0, n);
                pos += n;
            }
            thisNew = len;
        }

        writer.reset();   // 파일 마감

        newLen   = juce::jmax (newLen, thisNew);
        finalLen = juce::jmax (finalLen, written + thisNew);

        // 스왑: 기존을 .prev 로 보존(undo 1단계) → tmp 를 정식 이름으로
        const auto prevFile = takeDir.getChildFile (name + ".prev");
        prevFile.deleteFile();
        if (dstFile.existsAsFile())
            dstFile.moveFileTo (prevFile);
        tmpOut.moveFileTo (dstFile);
    }

    // 채널 수가 줄었으면 이번 녹음에 없는 잔여 스템을 .prev 로 이동 —
    // 현재 상태(새 채널 구성)와 undo 상태(이전 구성)의 일관성 유지.
    {
        juce::Array<juce::File> existing;
        takeDir.findChildFiles (existing, juce::File::findFiles, false, "ch*_dry.wav");
        for (auto& f : existing)
        {
            bool inNewRecording = false;
            for (auto& ns : newStems)
                if (ns.getFileName() == f.getFileName()) { inNewRecording = true; break; }

            if (! inNewRecording)
            {
                const auto prev = takeDir.getChildFile (f.getFileName() + ".prev");
                prev.deleteFile();
                f.moveFileTo (prev);
            }
        }
    }

    // 메타데이터도 커밋 직전 상태를 .prev 로 보존 (스템과 세트로 스왑됨)
    for (const auto* metaName : { "timeline.json", "session.json" })
    {
        const auto cur  = takeDir.getChildFile (metaName);
        const auto prev = takeDir.getChildFile (juce::String (metaName) + ".prev");
        prev.deleteFile();
        if (cur.existsAsFile())
            cur.copyFileTo (prev);   // timeline 은 아래에서 이어서 갱신하므로 copy
    }

    // timeline.json 갱신: 이력 append + 길이/채널/SR
    auto v = readTimeline (takeDir);
    juce::DynamicObject::Ptr obj = v.getDynamicObject();
    if (obj == nullptr)
        obj = new juce::DynamicObject();

    juce::Array<juce::var> history;
    if (auto* h = v.getProperty ("history", {}).getArray())
        history = *h;

    auto* entry = new juce::DynamicObject();
    entry->setProperty ("op",          punchSamples > 0 ? "punch" : "record");
    entry->setProperty ("startSample", punchSamples);
    entry->setProperty ("endSample",   punchSamples + newLen);
    entry->setProperty ("at",          juce::Time::getCurrentTime().toISO8601 (true));
    history.add (juce::var (entry));

    // 펀치인은 펀치 지점부터 끝까지 덮어씀 → 최종 길이 = punch + 새 녹음(짧아질 수 있음).
    obj->setProperty ("deviceName",    env.deviceName);
    obj->setProperty ("sampleRate",    sampleRate);
    obj->setProperty ("bufferSize",    env.bufferSize);
    obj->setProperty ("channels",      newStems.size());
    obj->setProperty ("lengthSamples", finalLen);
    obj->setProperty ("updatedAt",     juce::Time::getCurrentTime().toISO8601 (true));   // 녹음 끝날 때마다 갱신
    obj->setProperty ("history",       history);
    if (! obj->hasProperty ("id"))        obj->setProperty ("id", takeDir.getFileName());
    if (! obj->hasProperty ("createdAt")) obj->setProperty ("createdAt", juce::Time::getCurrentTime().toISO8601 (true));

    writeTimeline (takeDir, juce::var (obj.get()));

    // 세션 스냅샷 갱신(이번 녹음 시점 구성)
    takeDir.getChildFile ("session.json").replaceWithText (juce::JSON::toString (sessionSnapshot));
}

//==============================================================================
// 녹음 undo/redo — 커밋 직전 상태(*.prev)와 현재 상태를 렌임으로 통째 스왑.

static juce::StringArray undoBaseNames (const juce::File& takeDir)
{
    // 스왑 대상 = 스템 + 메타데이터. 현재/보존 어느 쪽에만 있는 파일도 포함
    // (첫 녹음 undo: 스템이 .prev 쪽에만 생기는 경우 등).
    juce::StringArray names;
    juce::Array<juce::File> files;
    takeDir.findChildFiles (files, juce::File::findFiles, false, "ch*_dry.wav*");
    for (auto& f : files)
    {
        auto n = f.getFileName();
        if (n.endsWith (".prev"))
            n = n.dropLastCharacters (5);
        if (n.endsWith ("_dry.wav"))
            names.addIfNotAlreadyThere (n);
    }
    names.add ("timeline.json");
    names.add ("session.json");
    return names;
}

bool TakeManager::hasUndoState (const juce::File& takeDir) const
{
    return takeDir.getChildFile ("timeline.json.prev").existsAsFile();
}

bool TakeManager::swapUndoState (const juce::File& takeDir) const
{
    if (! hasUndoState (takeDir))
        return false;

    bool ok = true;
    for (const auto& name : undoBaseNames (takeDir))
    {
        const auto cur  = takeDir.getChildFile (name);
        const auto prev = takeDir.getChildFile (name + ".prev");
        const auto tmp  = takeDir.getChildFile (name + ".swp");

        const bool hasCur = cur.existsAsFile(), hasPrev = prev.existsAsFile();
        if (hasCur && hasPrev)
        {
            tmp.deleteFile();
            ok = cur.moveFileTo (tmp) && prev.moveFileTo (cur) && tmp.moveFileTo (prev) && ok;
        }
        else if (hasCur)   ok = cur.moveFileTo (prev) && ok;   // 현재에만 → 보존 쪽으로
        else if (hasPrev)  ok = prev.moveFileTo (cur) && ok;   // 보존에만 → 현재 쪽으로
    }
    return ok;
}

bool TakeManager::isCurrentNewer (const juce::File& takeDir) const
{
    const auto curAt  = readTimeline (takeDir).getProperty ("updatedAt", "").toString();
    const auto prevAt = juce::JSON::parse (takeDir.getChildFile ("timeline.json.prev"))
                            .getProperty ("updatedAt", "").toString();
    return curAt >= prevAt;   // ISO8601 문자열 = 사전순 비교 가능
}
