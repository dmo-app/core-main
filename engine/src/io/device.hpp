#pragma once
#include <memory>
#include "core/server.hpp"

namespace core {
// Audio output through miniaudio (WASAPI shared/exclusive on Windows; null backend for headless tests).
std::unique_ptr<AudioOutput> makeMiniaudioOutput();
} // namespace core
