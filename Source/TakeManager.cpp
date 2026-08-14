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
        {
            t.historyCount = h->size();
            for (auto& e : *h)
            {
                HistoryEntry he;
                he.op          = e.getProperty ("op", "").toString();
                he.startSample = (juce::int64) (double) e.getProperty ("startSample", 0.0);
                he.endSample   = (juce::int64) (double) e.getProperty ("endSample", 0.0);
                he.at          = e.getProperty ("at", "").toString();
                if (auto* chs = e.getProperty ("channels", {}).getArray())
                    for (auto& c : *chs)
                        he.channels.add ((int) c);
                t.history.add (he);
            }
        }
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

    // 마지막 작업(updatedAt) 최신순 — 옛 테이크에 재녹음하면 위로 올라온다.
    // ISO8601 문자열 = 사전순 비교. 동률(구형 데이터)은 id(생성 시각) 역순.
    std::sort (takes.begin(), takes.end(),
               [] (const TakeInfo& a, const TakeInfo& b)
               { return a.updatedAt != b.updatedAt ? a.updatedAt > b.updatedAt : a.id > b.id; });
    return takes;
}

//==============================================================================
void TakeManager::commitRecording (const juce::File& takeDir, const juce::File& recTmp,
                                   juce::int64 punchSamples, const TakeEnv& env,
                                   const juce::var& sessionSnapshot,
                                   juce::int64 trimSamples, juce::uint32 armedMask,
                                   juce::int64 srcOffsetSamples)
{
    ensureFormats();
    const double sampleRate = env.sampleRate;

    juce::Array<juce::File> newStems;
    recTmp.findChildFiles (newStems, juce::File::findFiles, false, "ch*_dry.wav");
    newStems.sort();
    if (newStems.isEmpty())
        return;

    // armed 채널이 하나도 없으면 커밋 없음 — 아래의 .prev 정리 전에 확인해야
    // 기존 undo 상태를 실수로 지우지 않는다.
    {
        bool anyArmed = false;
        for (auto& ns : newStems)
        {
            const int c = ns.getFileName().fromFirstOccurrenceOf ("ch", false, false)
                              .upToFirstOccurrenceOf ("_", false, false).getIntValue() - 1;
            if (c >= 0 && c < 32 && (armedMask & (1u << (juce::uint32) c)) != 0)
                { anyArmed = true; break; }
        }
        if (! anyArmed)
            return;
    }

    // 이전 커밋의 stale .prev 정리 — 부분 채널 커밋에서 만지지 않은 채널의 옛
    // .prev 가 남으면 undo 가 회차가 섞인 상태를 복원한다 (DESIGN §5.12).
    {
        juce::Array<juce::File> stale;
        takeDir.findChildFiles (stale, juce::File::findFiles, false, "*.prev");
        for (auto& f : stale)
            f.deleteFile();
    }

    // 길이 불변식: 절대 줄지 않는다 — 시작값이 기존 길이 (§5.12)
    const juce::int64 prevLen = (juce::int64) (double) readTimeline (takeDir).getProperty ("lengthSamples", 0.0);

    juce::AudioBuffer<float> buf (1, 32768);
    juce::int64 finalLen = prevLen;
    juce::int64 newLen   = 0;
    juce::Array<int> armedChannels;   // 이번 커밋이 실제로 만진 채널 (history "channels")

    for (auto& ns : newStems)
    {
        const auto name    = ns.getFileName();

        // 미선택 채널 — 파일 불가침 (꼬리 포함 전부 유지, .prev 도 만들지 않음)
        const int chIndex = name.fromFirstOccurrenceOf ("ch", false, false)
                                .upToFirstOccurrenceOf ("_", false, false).getIntValue() - 1;
        if (chIndex < 0 || chIndex >= 32 || (armedMask & (1u << (juce::uint32) chIndex)) == 0)
            continue;

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

        armedChannels.add (chIndex);

        // 기존 파일 리더 — head 와 tail 양쪽에서 쓴다
        std::unique_ptr<juce::AudioFormatReader> old (
            dstFile.existsAsFile() ? formatMgr.createReaderFor (dstFile) : nullptr);
        const juce::int64 oldLen = old != nullptr ? (juce::int64) old->lengthInSamples : 0;

        auto copyFromReader = [&] (juce::AudioFormatReader& r, juce::int64 srcStart, juce::int64 count)
        {
            juce::int64 done = 0;
            while (done < count)
            {
                const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), count - done);
                r.read (&buf, 0, n, srcStart + done, true, false);
                writer->writeFromAudioSampleBuffer (buf, 0, n);
                done += n;
            }
            return done;
        };

        // 1) head: 기존 테이크 파일 [0..punchSamples), 부족분 무음 패딩
        juce::int64 written = 0;
        if (punchSamples > 0)
        {
            if (old != nullptr)
                written += copyFromReader (*old, 0, juce::jmin (punchSamples, oldLen));
            while (written < punchSamples)
            {
                const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), punchSamples - written);
                buf.clear();
                writer->writeFromAudioSampleBuffer (buf, 0, n);
                written += n;
            }
        }

        // 2) 새 녹음 — srcOffset(프리롤) 건너뛰고, 펀치아웃이면 trim 으로 절단
        //    (정지 지터와 무관하게 창 [in,out) 이 샘플 정확해진다)
        juce::int64 thisNew = 0;
        if (auto* nr = formatMgr.createReaderFor (ns))
        {
            std::unique_ptr<juce::AudioFormatReader> nw (nr);
            juce::int64 len = (juce::int64) nw->lengthInSamples - juce::jmax ((juce::int64) 0, srcOffsetSamples);
            len = juce::jmax ((juce::int64) 0, len);
            if (trimSamples > 0)
                len = juce::jmin (len, trimSamples);
            thisNew = copyFromReader (*nw, juce::jmax ((juce::int64) 0, srcOffsetSamples), len);
        }

        // 3) tail: 기존 파일 [punch+N .. oldEnd) 이어붙임 — 꼬리 보존.
        //    테이크 길이가 절대 줄지 않는 것은 이 복사가 보장한다 (§5.12).
        juce::int64 tailN = 0;
        if (old != nullptr && punchSamples + thisNew < oldLen)
            tailN = copyFromReader (*old, punchSamples + thisNew, oldLen - (punchSamples + thisNew));

        old.reset();      // 리더 해제 (아래 렌임 전 필수)
        writer.reset();   // 파일 마감

        newLen   = juce::jmax (newLen, thisNew);
        finalLen = juce::jmax (finalLen, written + thisNew + tailN);

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

                // 지운 채널도 "만진 채널" — 없으면 undo 후 redo 가 이 파일을 되지우지 못한다.
                const int c = f.getFileName().fromFirstOccurrenceOf ("ch", false, false)
                                  .upToFirstOccurrenceOf ("_", false, false).getIntValue() - 1;
                if (c >= 0)
                    armedChannels.addIfNotAlreadyThere (c);
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
    entry->setProperty ("endSample",   punchSamples + newLen);   // 대체된 구간 [start,end)
    entry->setProperty ("at",          juce::Time::getCurrentTime().toISO8601 (true));
    {
        juce::Array<juce::var> chArr;
        for (int c : armedChannels)
            chArr.add (c);
        entry->setProperty ("channels", chArr);   // 만진 채널 — UI 표기 + 부분 undo 판별
    }
    history.add (juce::var (entry));

    // 최종 길이 = max(기존, punch + 새 녹음) — 꼬리 보존으로 절대 줄지 않는다 (§5.12).
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

    // 부분 채널 커밋 대응(§5.12): .prev 없는 현재 스템은 두 경우로 갈린다 —
    // ① 스왑 대상 커밋이 만들거나 지운 파일(첫 녹음·채널 감소 등) → 옮겨야 함
    // ② 부분 커밋이 만지지 않은 채널 → 불가침.
    // 판별: 두 상태 중 **최신 쪽** timeline 의 마지막 이력 channels(만진 채널)로
    // 가른다 — undo 때는 현재, redo 때는 .prev 가 최신이다.
    const bool undoDirection = isCurrentNewer (takeDir);
    const auto newerTimeline = undoDirection
                                   ? readTimeline (takeDir)
                                   : juce::JSON::parse (takeDir.getChildFile ("timeline.json.prev"));
    juce::Array<int> touched;
    bool hasTouchedInfo = false;
    if (auto* h = newerTimeline.getProperty ("history", {}).getArray(); h != nullptr && ! h->isEmpty())
        if (auto* chs = h->getLast().getProperty ("channels", {}).getArray())
        {
            hasTouchedInfo = true;   // 구버전 이력엔 없음 → 전 채널 취급(레거시 동작 유지)
            for (auto& c : *chs)
                touched.add ((int) c);
        }

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
        else if (hasCur)
        {
            if (hasTouchedInfo && name.startsWith ("ch"))
            {
                const int idx = name.fromFirstOccurrenceOf ("ch", false, false)
                                    .upToFirstOccurrenceOf ("_", false, false).getIntValue() - 1;
                if (! touched.contains (idx))
                    continue;   // 부분 커밋이 만지지 않은 채널 — 불가침
            }
            ok = cur.moveFileTo (prev) && ok;   // 커밋이 만들거나 지운 파일 → 반대 상태로
        }
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
