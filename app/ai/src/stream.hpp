// Streaming (A8 of the plan, 2026-11-16): Claude's answer arrives as server-sent events, so the panel can show the
// text and the calls as they come instead of waiting 7 to 13 s for the whole message. The assembler turns the events
// back into the same message a request without streaming gets ({content, stop_reason, usage}), so Chat::onResponse,
// the cost and the memory see no difference. Anthropic's API only; the OpenAI-compatible providers stay unstreamed.
// No network here: the app feeds it the bytes it reads (any chunk size). Testable.
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include "json.hpp"

namespace core::ai {

class StreamAssembler {
public:
  std::function<void(const std::string&)> onText;  // each piece of the answer's text, in order
  std::function<void(const std::string&)> onTool;  // the name of each tool call as it starts

  void feed(std::string_view bytes);  // any chunk of the HTTP body
  void finish();                      // the body ended: a last event without its blank line is read too

  bool sawEvents() const { return events_ > 0; }  // false: the body was not a stream (an HTTP error's JSON)
  bool complete() const { return stopped_; }      // message_stop arrived
  // The answer as a message without streaming would give it; an {"type":"error"} object when the stream said so.
  nlohmann::json message() const;

private:
  void line(std::string_view l);
  void event(const std::string& data);

  std::string pending_, data_;
  int events_ = 0;
  bool stopped_ = false;
  nlohmann::json message_ = nlohmann::json::object();
  nlohmann::json error_;
  std::vector<std::string> partialJson_;  // per block: the tool input's JSON as it arrives
};

}  // namespace core::ai
