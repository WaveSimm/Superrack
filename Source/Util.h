#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// JUCE 의 String(const char*) 는 8비트 ASCII 전용이라 한글 리터럴이 깨진다.
// 비ASCII 문자열은 반드시 이 헬퍼(또는 CharPointer_UTF8)로 UTF-8 디코딩한다.
namespace sr
{
    inline juce::String u8 (const char* utf8Literal)
    {
        return juce::String (juce::CharPointer_UTF8 (utf8Literal));
    }
}
