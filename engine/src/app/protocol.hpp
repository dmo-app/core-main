#pragma once
#include "core/project.hpp"
#include <istream>

namespace core {
// OS/plugin diagnostics can contain bytes in a local codepage. A bad diagnostic must not
// terminate serve's ticker or command thread. Keep the JSON line valid UTF-8 on every path.
inline std::string protocolLine(const json& message) {
  return message.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Drain an oversized line without growing it beyond the protocol limit. Reading its newline
// still matters: the next command must start on the next line, not in the rejected payload.
inline bool readProtocolLine(std::istream& input, std::string& line, size_t limit, bool& tooLarge) {
  line.clear();
  tooLarge = false;
  bool any = false;
  auto* buffer = input.rdbuf();
  for (;;) {
    const auto c = buffer->sbumpc();
    if (c == std::char_traits<char>::eof()) return any;
    any = true;
    if (c == '\n') return true;
    if (line.size() < limit) line.push_back(static_cast<char>(c));
    else tooLarge = true;
  }
}
}  // namespace core
