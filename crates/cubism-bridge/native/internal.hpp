#pragma once
#include "bridge.h"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

extern thread_local char error_text[1024];

template<typename Function> int32_t checked(Function operation) noexcept {
    try {
        operation();
        return 0;
    } catch (const std::exception& error) {
        std::snprintf(error_text, sizeof error_text, "%s", error.what());
    } catch (...) {
        std::snprintf(error_text, sizeof error_text, "unknown Cubism error");
    }
    return 1;
}

inline std::string input_string(const uint8_t* bytes, size_t length) {
    if (!bytes || !length) throw std::invalid_argument("empty input string");
    std::string result(reinterpret_cast<const char*>(bytes), length);
    if (result.find('\0') != std::string::npos) throw std::invalid_argument("NUL in input string");
    return result;
}

int32_t viewport_width(const DwRuntime& runtime);
int32_t viewport_height(const DwRuntime& runtime);
void begin_frame(DwRuntime& runtime);
void require_window(const DwRuntime& runtime);
void stage_window(DwRuntime& runtime);
void read_frame(const DwRuntime& runtime, uint8_t* pixels, size_t length);
