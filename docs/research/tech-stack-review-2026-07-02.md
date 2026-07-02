# Superrack 기술 스택 대안 검토 (2026-07-02, 웹 리서치 기반)

> 방법: 6개 병렬 웹 리서치 + 8개 핵심 주장 적대적 검증(1차 출처: GitHub 소스/릴리스, 공식 문서).
> 관련: [`../BACKLOG.md`](../BACKLOG.md) · [`../02-design/DESIGN.md`](../02-design/DESIGN.md)

## 1. 프레임워크: JUCE 8 — **유지** (8.0.14 로 포인트 업그레이드 권장)

- iPlug2 / DPF / CPLUG 는 전부 플러그인 **제작** 프레임워크 — 호스팅 기능 없음. 후보 탈락.
- Steinberg VST3 SDK 는 2025-11 **MIT 전환**으로 라이선스 장벽이 사라졌지만, 직접 호스팅은 스캐닝/장치/GUI/그래프 전부 자작 = 수개월급. Tracktion Engine 은 JUCE 위 레이어(탈JUCE 아님, GPLv3/상용).
- JUCE 자체는 활발(8.0.14, 2026-06-22; 8.0.13에서 VST3 SDK 3.8.0 반영). 기존 호스트 앱의 탈JUCE 이전 사례 없음.

## 2. 실시간 안전 패턴: **farbot RealtimeObject 도입 권장**

- 현재의 "atomic 플래그 + CallbackFence 대기"는 개선됐지만 여전히 수제 패턴.
- **farbot `RealtimeObject<Chain, nonRealtimeMutatable>`**: 메시지 스레드에서 새 체인 구성 → 오디오 스레드 wait-free 획득 → 구버전 비RT 해제. Tracktion/Waveform 프로덕션 검증, MIT, 헤더 벤더링으로 도입 비용 낮음. 단 **fifo 모듈은 미해결 레이스 리포트(#24/#26) — 쓰지 말 것**, RealtimeObject 만.
- crill 의 RCU 는 main 미병합(WIP). `std::atomic<shared_ptr>` 는 MSVC 락 기반 + 오디오 스레드 해제 위험 — 부적합.
- 전문가 합의(Doumler, Renn-Giles/Rowland ADC 2019·2022): "완성된 교체 객체 포인터 스왑 + 비RT 지연 삭제"가 정답 패턴.

## 3. 채널 병렬 처리: **커스텀 소형 워커 풀 도입 권장** (~200-400 LOC)

- 측정된 병목(무거운 체인 4ch@96k/1ms 예산)을 직접 해결. 채널 완전 독립 = embarrassingly parallel — 업계 표준 분해 단위도 "독립 체인당 워커 1개"(Ableton 공개 문서).
- 설계 레퍼런스: **tracktion_graph `LockFreeMultiThreadedNodePlayer`** (rigtorp MPMCQueue + atomic 의존성 카운터, 오디오 스레드도 처리 참여 fork-join). GPLv3 라 코드 복사 불가, 설계 참고만.
- 대기 전략: **짧은 spin(backoff) → lightweight semaphore** (Rowland 프로덕션 결론). 순수 CV 는 1ms 예산에 웨이크업 과다, 순수 busy-wait 는 우선순위 역전 위험.
- Windows 세부: JUCE `AudioWorkgroup` 은 Apple 전용, `ThreadPool` RT 부적합, `startRealtimeThread` 는 프로세스 전체 REALTIME_PRIORITY_CLASS 부작용. 워커는 직접 `SetThreadPriority(TIME_CRITICAL)` + MMCSS "Pro Audio" + 하이브리드 CPU P-코어 고정, 풀 크기 < 물리 P-코어 수.
- taskflow / oneTBB 등 범용 라이브러리는 RT 보장 없음 — 배제.
- 대안: +1버퍼 파이프라이닝(ASIO-Guard/REAPER 방식) — 모니터링 지연 1버퍼 추가 비용.

## 4. CLAP 호스팅: **보류**

- CLAP 1.2.9 스펙은 안정적이지만 타깃 시장(상용 믹싱 플러그인) 커버리지 이득 ≈ 0: 메이저 믹싱 벤더 중 FabFilter 만 CLAP 출시(전부 VST3 병행). Waves/UAD/iZotope/NI 미지원.
- JUCE 공식 CLAP **호스팅** 지원 없음(JUCE 9 로드맵의 CLAP 은 플러그인 제작 쪽). 커뮤니티 `juce_clap_hosting` 은 super-alpha.
- 재검토 트리거: 메이저 벤더 CLAP 출시 또는 JUCE 공식 호스팅 지원.

## 5. 스템 스트리밍/녹음: **재생만 교체, 녹음 유지**

- **BufferingAudioSource 는 알려진 것보다 나쁨**: 소스 검증 결과 오디오 스레드가 락 2개, 백그라운드 스레드가 **같은 callbackLock 을 쥔 채 디스크 read** — 교과서적 우선순위 역전. JUCE 8 수정 없음.
  - 대체 패턴: 채널당 SPSC 링버퍼 + 리더(prefetch) 스레드, 언더런 시 무음. 레퍼런스: Mixxx `CachingReader`(GPL, 읽기 좋음), Ardour butler.
- **ThreadedWriter 는 유지 적합**(write 경로 AbstractFifo 락프리, 소스 검증). 32ch×96k×32f ≈ 12.3MB/s — NVMe 에 사소. 개선: 채널당 수 초 FIFO(총 ~25-60MB), `setFlushInterval()` 주기 flush(크래시 시 헤더 복구성).
- **RF64**: JUCE 가 4GB 초과 시 자동 전환(소스 확인) — 별도 작업 불필요.

## 6. ASIO: **유지**

- 라이선스 문제 2025-10 대부분 소멸: ASIO SDK 독점+GPLv3 이중 라이선스화, **JUCE 8.0.11부터 헤더 번들**(현재 버전이 이미 해당). 클로즈드소스 배포 시에만 Steinberg 무료 독점 계약 서명.
- WASAPI 는 다채널 구조적 불가(프로 인터페이스가 스테레오 페어 다수로 노출 — 단일 장치 32ch 아키텍처와 비호환). 비교 호스트 전원(Waves SuperRack Performer, Gig Performer, Cantabile) ASIO 전용/우선.
- MS 인박스 UAC2+ASIO 드라이버(ACX, MIT)는 ASIO 를 강화하는 방향, 2026-07 현재 프리뷰 미출시.
- 선택 과제: 스테레오 전용 무설정 폴백으로 WASAPI sharedLowLatency 노출(JUCE 기본 지원).

## 도입 가치 Top 3

1. **채널 병렬 워커 풀** (§3) — 유일하게 측정된 실병목(4ch@96k) 직접 해결. → BACKLOG A2
2. **farbot RealtimeObject 체인 스왑** (§2) — 최저 비용으로 수제 패턴을 검증된 패턴으로. 병렬화의 전제 조건. → BACKLOG D2
3. **BufferingAudioSource 교체** (§5) — 락 안 디스크 I/O(우선순위 역전) 소스 확인. → BACKLOG B

**유지**: JUCE 8(→8.0.14), ASIO, ThreadedWriter(튜닝만), 자동 RF64. **보류**: CLAP 호스팅, 탈JUCE.
