// The wire between Core and its AI (Fase 4, 2026-11-16). The AI is a separate, optional process (core-ai); Core
// starts it with pipes on its stdin/stdout and talks only through them: no sockets, no ports, no network on Core's
// side. Without the process Core works at 100% (the panel says "offline").
//
// A frame = 4 bytes of length (little endian) + that many bytes of UTF-8 JSON, one object with a "type".
// Limits: what the AI sends Core is at most kMaxFromAi bytes per frame (a proposal is small); what Core sends the AI
// (the song) at most kMaxToAi. JSON nested deeper than kMaxDepth is refused before it is parsed. A frame that breaks
// any rule is a protocol violation: Core logs it and stops the AI process (it never guesses what was meant).
//
// The messages (docs/contracts/ai-ipc.md has every field):
//   Core -> AI  hello {protocol, catalog, scope}, ask {id, text, song, zone, settings}, cancel {id},
//               applied {id}, feedback {recipeId, liked, note}, memory {op, file}, chats {op, file}, bye
//   AI -> Core  hello {protocol, models, notices}, status {id, text}, partial {id, text},
//               answer {id, reply, error, log, proposal, cost}, recipe {id, recipeId}, result {op, ok, text, data},
//               error {id?, text}
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "json.hpp"

namespace core::ai::ipc {

using json = nlohmann::json;

constexpr int kProtocol = 1;
constexpr size_t kMaxFromAi = size_t(1) << 20;   // 1 MiB
constexpr size_t kMaxToAi = size_t(32) << 20;    // 32 MiB (a whole song)
constexpr int kMaxDepth = 64;

// One frame for a message; "" when it is bigger than maxBytes (nothing is sent then).
std::string frame(const json& message, size_t maxBytes);

// Reads frames from a byte stream. After the first error it stays failed (the stream cannot be trusted any more).
class Decoder {
public:
  explicit Decoder(size_t maxFrame) : max_(maxFrame) {}
  // The messages completed by these bytes. A broken frame: failed() and why in error().
  std::vector<json> feed(const char* data, size_t size);
  bool failed() const { return !error_.empty(); }
  const std::string& error() const { return error_; }
  size_t pending() const { return buf_.size(); }  // bytes of a frame not complete yet

private:
  size_t max_;
  std::string buf_;
  std::string error_;
};

// How deep the JSON text nests ({ and [ outside strings); -1 when the brackets do not match.
int depthOf(std::string_view text);
// A frame's payload as a message: valid UTF-8 JSON, an object with a string "type", nested at most kMaxDepth.
// Null and why when not.
json parseMessage(std::string_view payload, std::string* why);

// Each message the AI sends Core, checked field by field (types and sizes). "" when it is fine, else why.
// Extra fields are ignored; an unknown type is refused.
std::string checkFromAi(const json& message);
// The same for what Core sends the AI (the AI process checks it).
std::string checkToAi(const json& message);

}  // namespace core::ai::ipc
