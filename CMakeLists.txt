cmake_minimum_required(VERSION 3.22)
project(Superrack VERSION 0.1.0 LANGUAGES C CXX)

# ── JUCE 8.0.14 자동 다운로드 (최초 1회, 이후 캐시. VST3 SDK 3.8.0 포함) ───────
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.14
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ── ASIO SDK (옵션) ───────────────────────────────────────────────────────────
# JUCE_ASIO=1 을 쓰려면 Steinberg ASIO SDK 헤더(common/asio.h)가 인클루드 경로에
# 있어야 한다. SDK 경로를 주면 자동으로 추가한다.
#   cmake -B build -DASIO_SDK_DIR="C:/SDKs/asiosdk/common"
set(ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK 의 common 폴더 (asio.h 위치)")

# ── GUI 앱 타깃 ───────────────────────────────────────────────────────────────
# macOS: MICROPHONE_PERMISSION_* 가 Info.plist 의 NSMicrophoneUsageDescription 을
# 만든다. 이게 없으면 Tahoe(macOS 26) 에서 CoreAudio 입력 장치를 열 때 TCC 가
# 앱을 종료시키거나 무음을 준다 — 맥 녹음 실패의 근본 원인. BUNDLE_ID 는 TCC 승인이
# 재빌드마다 초기화되지 않도록 고정한다.
juce_add_gui_app(Superrack
    PRODUCT_NAME "Superrack"
    COMPANY_NAME "Superrack"
    BUNDLE_ID "com.superrack.Superrack"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Superrack 는 오디오 인터페이스 입력 채널을 처리·녹음하기 위해 오디오 입력 접근이 필요합니다.")

target_sources(Superrack PRIVATE
    Source/Main.cpp
    Source/AppSettings.cpp
    Source/AudioEngine.cpp
    Source/ChannelStrip.cpp
    Source/PluginCatalog.cpp
    Source/PluginBrowser.cpp
    Source/MultitrackRecorder.cpp
    Source/TimelinePlayer.cpp
    Source/TakeManager.cpp
    Source/CpuProfiler.cpp
    Source/AudioWorkerPool.cpp
    Source/ChannelRow.cpp
    Source/TimelineView.cpp
    Source/MainComponent.cpp)

target_compile_features(Superrack PRIVATE cxx_std_17)

# farbot (MIT, 벤더링) — RT wait-free 체인 스왑 (RealtimeObject 만 사용, fifo 금지)
target_include_directories(Superrack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/farbot/include")

# UTF-8 소스 (한국어 주석) — MSVC 코드페이지 경고(C4819) 방지
if(MSVC)
    target_compile_options(Superrack PRIVATE /utf-8)
endif()

target_compile_definitions(Superrack PRIVATE
    $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>   # ASIO 백엔드 (Windows 전용 — 맥은 CoreAudio)
    JUCE_PLUGINHOST_VST3=1       # Phase 1~ 채널 VST3 체인 호스팅
    JUCE_MODAL_LOOPS_PERMITTED=1 # 동기 플러그인 스캔 중 진행창 갱신 (PluginBrowser)
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

if(ASIO_SDK_DIR)
    target_include_directories(Superrack PRIVATE "${ASIO_SDK_DIR}")
    message(STATUS "Superrack: ASIO SDK 경로 사용 -> ${ASIO_SDK_DIR}")
else()
    message(STATUS "Superrack: ASIO_SDK_DIR 미지정 - JUCE 번들 ASIO 헤더로 빌드 시도")
endif()

target_link_libraries(Superrack PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ── 베타 배포 타깃 — 머신별 최초 실행 + 15일 후 만료 (BetaGate) ────────────────
# 기본 빌드에서 제외: cmake --build build --config Release --target SuperrackBeta
juce_add_gui_app(SuperrackBeta
    PRODUCT_NAME "Superrack Beta"
    COMPANY_NAME "Superrack"
    BUNDLE_ID "com.superrack.SuperrackBeta"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Superrack 는 오디오 인터페이스 입력 채널을 처리·녹음하기 위해 오디오 입력 접근이 필요합니다."
    EXCLUDE_FROM_ALL TRUE)

target_sources(SuperrackBeta PRIVATE
    Source/Main.cpp
    Source/AppSettings.cpp
    Source/AudioEngine.cpp
    Source/BetaGate.cpp
    Source/ChannelStrip.cpp
    Source/PluginCatalog.cpp
    Source/PluginBrowser.cpp
    Source/MultitrackRecorder.cpp
    Source/TimelinePlayer.cpp
    Source/TakeManager.cpp
    Source/CpuProfiler.cpp
    Source/AudioWorkerPool.cpp
    Source/ChannelRow.cpp
    Source/TimelineView.cpp
    Source/MainComponent.cpp)

target_compile_features(SuperrackBeta PRIVATE cxx_std_17)
target_include_directories(SuperrackBeta PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/farbot/include")

if(MSVC)
    target_compile_options(SuperrackBeta PRIVATE /utf-8)
endif()

target_compile_definitions(SuperrackBeta PRIVATE
    $<$<PLATFORM_ID:Windows>:JUCE_ASIO=1>
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    SUPERRACK_BETA=1
    SUPERRACK_BETA_DAYS=15)

if(ASIO_SDK_DIR)
    target_include_directories(SuperrackBeta PRIVATE "${ASIO_SDK_DIR}")
endif()

target_link_libraries(SuperrackBeta PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_utils
    juce::juce_gui_basics
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ── L1 테스트 타깃 (QA Phase, BACKLOG G) — 헤드리스 콘솔, ASIO/플러그인 불필요 ──
juce_add_console_app(SuperrackTests PRODUCT_NAME "SuperrackTests")

target_sources(SuperrackTests PRIVATE
    Tests/TestMain.cpp
    Source/AppSettings.cpp
    Source/BetaGate.cpp
    Source/ChannelStrip.cpp
    Source/PluginCatalog.cpp
    Source/MultitrackRecorder.cpp
    Source/TimelinePlayer.cpp
    Source/TakeManager.cpp
    Source/AudioWorkerPool.cpp)

target_compile_features(SuperrackTests PRIVATE cxx_std_17)
target_include_directories(SuperrackTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/farbot/include")

if(MSVC)
    target_compile_options(SuperrackTests PRIVATE /utf-8)
endif()

target_compile_definitions(SuperrackTests PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(SuperrackTests PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_events
    PUBLIC
    juce::juce_recommended_config_flags)
