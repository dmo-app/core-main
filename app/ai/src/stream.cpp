#include "stream.hpp"

namespace core::ai {

void StreamAssembler::feed(std::string_view bytes) {
  pending_.append(bytes);
  size_t start = 0;
  for (size_t nl; (nl = pending_.find('\n', start)) != std::string::npos; start = nl + 1) {
    std::string_view l(pending_.data() + start, nl - start);
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    line(l);
  }
  pending_.erase(0, start);
}

void StreamAssembler::finish() {
  if (!pending_.empty()) {
    std::string rest;
    rest.swap(pending_);
    if (!rest.empty() && rest.back() == '\r') rest.pop_back();
    line(rest);
  }
  line("");  // the last event, if its blank line never came
}

void StreamAssembler::line(std::string_view l) {
  if (l.empty()) {  // the end of an event
    if (!data_.empty()) {
      try {
        event(data_);
      } catch (const std::exception&) {  // a malformed event is skipped; a missing end makes message() an error
      }
    }
    data_.clear();
    return;
  }
  if (l.substr(0, 5) == "data:") {
    l.remove_prefix(5);
    if (!l.empty() && l.front() == ' ') l.remove_prefix(1);
    if (!data_.empty()) data_ += '\n';
    data_.append(l);
  }
  // "event:", ":" comments and anything else: the data's own "type" says what it is.
}

void StreamAssembler::event(const std::string& data) {
  using json = nlohmann::json;
  const json e = json::parse(data, nullptr, false);
  if (!e.is_object()) return;
  ++events_;
  const std::string type = e.value("type", std::string());
  if (type == "message_start" && e.contains("message") && e["message"].is_object()) {
    message_ = e["message"];
    if (!message_.contains("content") || !message_["content"].is_array()) message_["content"] = json::array();
    return;
  }
  if (type == "error") {
    error_ = e;
    return;
  }
  if (!message_.contains("content")) message_["content"] = json::array();
  json& content = message_["content"];
  const size_t index = e.value("index", (size_t)0);
  if (type == "content_block_start" && e.contains("content_block")) {
    while (content.size() <= index) content.push_back(json::object());
    content[index] = e["content_block"];
    if (partialJson_.size() <= index) partialJson_.resize(index + 1);
    partialJson_[index].clear();
    if (content[index].value("type", std::string()) == "tool_use" && onTool) onTool(content[index].value("name", std::string()));
    return;
  }
  if (type == "content_block_delta" && e.contains("delta") && index < content.size()) {
    const json& d = e["delta"];
    json& block = content[index];
    const std::string dt = d.value("type", std::string());
    if (dt == "text_delta") {
      const std::string t = d.value("text", std::string());
      block["text"] = block.value("text", std::string()) + t;
      if (onText) onText(t);
    } else if (dt == "input_json_delta") {
      if (partialJson_.size() <= index) partialJson_.resize(index + 1);
      partialJson_[index] += d.value("partial_json", std::string());
    } else if (dt == "thinking_delta") {
      block["thinking"] = block.value("thinking", std::string()) + d.value("thinking", std::string());
    } else if (dt == "signature_delta") {
      block["signature"] = block.value("signature", std::string()) + d.value("signature", std::string());
    }
    return;
  }
  if (type == "content_block_stop" && index < content.size()) {
    json& block = content[index];
    if (block.value("type", std::string()) == "tool_use") {
      const std::string& raw = index < partialJson_.size() ? partialJson_[index] : std::string();
      if (!raw.empty()) {
        const json input = json::parse(raw, nullptr, false);
        block["input"] = input.is_discarded() ? json::object() : input;  // half written: Chat answers it, not runs it
      } else if (!block.contains("input")) {
        block["input"] = json::object();
      }
    }
    return;
  }
  if (type == "message_delta") {
    if (e.contains("delta") && e["delta"].is_object())
      for (const auto& [k, v] : e["delta"].items()) message_[k] = v;
    if (e.contains("usage") && e["usage"].is_object()) {
      if (!message_.contains("usage") || !message_["usage"].is_object()) message_["usage"] = json::object();
      for (const auto& [k, v] : e["usage"].items())
        if (!v.is_null()) message_["usage"][k] = v;
    }
    return;
  }
  if (type == "message_stop") stopped_ = true;
}

nlohmann::json StreamAssembler::message() const {
  if (!error_.is_null()) return error_;
  nlohmann::json m = message_;
  // Blocks never started (an index skipped) would be empty objects: leave them out.
  if (m.contains("content") && m["content"].is_array()) {
    nlohmann::json kept = nlohmann::json::array();
    for (const auto& b : m["content"])
      if (b.is_object() && b.contains("type")) kept.push_back(b);
    m["content"] = kept;
  }
  const bool hasStop = m.contains("stop_reason") && m["stop_reason"].is_string() && !m["stop_reason"].get<std::string>().empty();
  if (!stopped_ && !hasStop) {
    // The stream broke before the end: an error, so the message leaves the conversation (Chat::abandon).
    return {{"type", "error"}, {"error", {{"type", "stream_cut"}, {"message", "the answer stopped before its end"}}}};
  }
  return m;
}

}  // namespace core::ai
