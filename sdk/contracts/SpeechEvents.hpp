#pragma once

// Shared by speech providers and orchestration plugins, never interpreted by the host.
namespace SpeechEvents {
constexpr auto Speak = "speech.speak";
constexpr auto Completed = "speech.completed";
}
