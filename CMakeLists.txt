cmake_minimum_required(VERSION 3.22)
project(Superrack VERSION 0.1.0 LANGUAGES CXX)

# ── JUCE 8.0.11 자동 다운로드 (최초 1회, 이후 캐시) ────────────────────────────
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.11
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ── ASIO SDK (옵션) ───────────────────────────────────────────────────────────
# JUCE_ASIO=1 을 쓰려면 Steinberg ASIO SDK 헤더(common/asio.h)가 인클루드 경로에
# 있어야 한다. SDK 경로를 주면 자동으로 추가한다.
#   cmake -B build -DASIO_SDK_DIR="C:/SDKs/asiosdk/common"
set(ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK 의 common 폴더 (asio.h 위치)")

# ── GUI 앱 타깃 ───────────────────────────────────────────────────────────────
juce_add_gui_app(Superrack
    PRODUCT_NAME "Superrack"
    COMPANY_NAME "Superrack")

target_sources(Superrack PRIVATE
    Source/Main.cpp
    Source/AudioEngine.cpp
    Source/ChannelStrip.cpp
    Source/MultitrackRecorder.cpp
    Source/TimelinePlayer.cpp
    Source/TakeManager.cpp
    Source/CpuProfiler.cpp
    Source/ChannelRow.cpp
    Source/MainComponent.cpp)

target_compile_features(Superrack PRIVATE cxx_std_17)

# UTF-8 소스 (한국어 주석) — MSVC 코드페이지 경고(C4819) 방지
if(MSVC)
    target_compile_options(Superrack PRIVATE /utf-8)
endif()

target_compile_definitions(Superrack PRIVATE
    JUCE_ASIO=1                  # ASIO 백엔드 활성화
    JUCE_PLUGINHOST_VST3=1       # Phase 1~ 채널 VST3 체인 호스팅
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
