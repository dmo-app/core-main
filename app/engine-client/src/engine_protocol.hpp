// One line the engine wrote on stdout, read without trusting it (audit 2, 2026-11-16, bug 1): a reply with an
// "error" or "ok" of the wrong type, or an id that is not a whole number, used to throw inside the reader thread,
// which then ended the app (an exception that leaves a thread calls std::terminate). Here nothing throws: every
// line becomes one of these kinds, and the client decides what to do. No Windows and no GUI: tested on Linux.
#pragma once
#include <cstdint>
#include <string>
#include "json.hpp"

namespace core::client {

struct EngineLine {
  enum class Kind {
    NotJson,  // not a JSON object: kept in the diagnostics
    Ready,    // the hello
    Event,    // {event: ...}
    Reply,    // an answer to one of our commands (id)
    NoId      // looks like an answer, but has no whole-number id: a broken protocol (our client always sends whole ids,
              // so no command can be told its answer): the session ends at once, see endsSession()
  };
  Kind kind = Kind::NotJson;
  nlohmann::json message;  // the object as read (Ready, Event)
  int64_t id = 0;
  bool ok = false;         // only "ok": true is a success
  nlohmann::json result;   // null when there is none
  std::string error;       // Reply with ok false, NoId: what to show
};

inline std::string engineLineText(const nlohmann::json& value, size_t limit = 200) {
  std::string s = value.is_string() ? value.get_ref<const std::string&>()
                                    : value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  if (s.size() > limit) s.resize(limit);
  return s;
}

// Never throws (except std::bad_alloc).
inline EngineLine readEngineLine(const std::string& line) {
  EngineLine out;
  nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
  if (message.is_discarded() || !message.is_object()) return out;
  // An event has a text "event" and no "id". A line with an "id" is a reply even if it also carries an "event" (deep
  // hunt: {"id":5,"ok":true,"event":{}} used to be taken as an event, and the call waited for its whole timeout).
  if (message.contains("event") && !message.contains("id")) {
    const nlohmann::json& event = message["event"];
    out.kind = event.is_string() && event.get_ref<const std::string&>() == "ready" ? EngineLine::Kind::Ready : EngineLine::Kind::Event;
    out.message = std::move(message);
    return out;
  }
  const auto error = message.find("error");
  if (!message.contains("id") || !message["id"].is_number_integer()) {
    out.kind = EngineLine::Kind::NoId;
    out.error = error != message.end() ? engineLineText(*error) : line.substr(0, 200);
    return out;
  }
  out.kind = EngineLine::Kind::Reply;
  out.id = message["id"].get<int64_t>();
  const auto ok = message.find("ok");
  out.ok = ok != message.end() && ok->is_boolean() && ok->get<bool>();
  if (const auto result = message.find("result"); result != message.end()) out.result = *result;
  if (!out.ok) out.error = error != message.end() && !error->is_null() ? engineLineText(*error, 4000) : "Engine error.";
  return out;
}

// Audit 2 (deep version), point 4: a reply without a whole-number id used to be only logged, and the command waiting
// for it hung until its timeout (10 s), hiding the cause. Such a line means the engine broke the protocol: the
// session ends at once (every pending command fails with this reason) and the engine restarts.
inline bool endsSession(const EngineLine& line) { return line.kind == EngineLine::Kind::NoId; }
inline std::string endReason(const EngineLine& line) {
  return "The engine broke the protocol (a reply without a valid id): " + line.error;
}

}  // namespace core::client
