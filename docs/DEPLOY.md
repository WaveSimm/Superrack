# 배포 가이드 — Superrack

## 정식 빌드

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target Superrack
```
산출물: `build/Superrack_artefacts/Release/Superrack.exe` (단일 파일, JUCE 정적 링크).

### 다른 컴퓨터에서 실행
- 필요: Windows 10/11 x64 · VC++ 재배포 패키지(2015-2022, 대개 기설치) · ASIO 드라이버 · 사용할 VST3 플러그인.
- exe 복사만으로 동작. 세션(`.superrack`)·테이크 폴더는 선택 복사(테이크는 자기완결, SR 달라도 리샘플 재생).
- 첫 실행: ⚙ 설정에서 ASIO 장치 선택 + 저장 위치를 로컬 드라이브로 지정. 플러그인은 uid 폴백으로 경로 달라도 자동 재매칭.
- 포터블: `SUPERRACK_APPDATA=<폴더>` 로 설정 폴더를 이동식 매체에 둘 수 있음.

## 베타 빌드 (15일 만료)

```bash
cmake --build build --config Release --target SuperrackBeta
```
산출물: `build/SuperrackBeta_artefacts/Release/Superrack Beta.exe`.

- 별도 타깃(`EXCLUDE_FROM_ALL`) — 정식 빌드와 독립. 컴파일 정의 `SUPERRACK_BETA=1`, `SUPERRACK_BETA_DAYS=15`.
- **동작**: 각 머신에서 **최초 실행일 기준 15일** 사용 가능. 창 제목에 "남은 기간 N일" 표시. 만료 후에는 안내 후 자동 종료(메인 창 안 열림).
- **기록**: `%APPDATA%/Superrack/.beta-state` + HKCU 레지스트리 **이중 저장**(한쪽 삭제로 리셋 불가, 이른 first 채택). 체크섬 포함 난독 인코딩 + 시계 되돌림 감지.
- **한계**: 배포 관리용 게이트이지 DRM 아님 — %APPDATA%+레지스트리 동시 삭제, 시계 조작, 인코딩 리버스로 우회 가능. 신뢰 테스터 대상 기간 제한 용도.
- **기간 변경**: CMake `SUPERRACK_BETA_DAYS` 값 수정 후 재빌드.
- **QA 훅**: `SUPERRACK_BETA_TODAY=<epoch일>` 환경변수로 시간 경과 시뮬레이션(만료 테스트용). 판정 로직은 `SuperrackTests` 의 `testBetaGate` 가 회귀 검증.

## 테스트

```bash
cmake --build build --config Release --target SuperrackTests
./build/SuperrackTests_artefacts/Release/SuperrackTests.exe   # 84 어서션, 실패 수=종료 코드
```

## 참고 — ASIO SDK 라이선스
클로즈드소스 배포 시 Steinberg ASIO SDK 무료 독점 계약 서명 필요(기술검토 §6). 본인 머신 간 사용은 무관.
