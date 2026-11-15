// SHA-256 (FIPS 180-4) for asset identity: a file's content, never its name, says which asset it is.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace core::doc {

std::string sha256Hex(const void* data, size_t size);
std::string sha256File(const std::filesystem::path& path);  // throws std::runtime_error if unreadable

} // namespace core::doc
