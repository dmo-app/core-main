#include "provider.hpp"

namespace core::ai {

namespace {

std::string trimSlash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::string textOf(const json& content) {
  if (content.is_string()) return content.get<std::string>();
  std::string out;
  if (content.is_array())
    for (const auto& b : content)
      if (b.value("type", std::string()) == "text") out += (out.empty() ? "" : "\n") + b.value("text", std::string());
  return out;
}

json toOpenAi(const json& r) {
  json messages = json::array();
  const std::string system = textOf(r.value("system", json::array()));
  if (!system.empty()) messages.push_back({{"role", "system"}, {"content", system}});
  for (const auto& m : r.value("messages", json::array())) {
    const std::string role = m.value("role", std::string());
    const json& content = m.contains("content") ? m["content"] : json("");
    if (role == "assistant") {
      json out = {{"role", "assistant"}};
      const std::string text = textOf(content);
      out["content"] = text.empty() ? json(nullptr) : json(text);
      json calls = json::array();
      if (content.is_array())
        for (const auto& b : content)
          if (b.value("type", std::string()) == "tool_use")
            calls.push_back({{"id", b.value("id", std::string())}, {"type", "function"},
                             {"function", {{"name", b.value("name", std::string())}, {"arguments", b.value("input", json::object()).dump()}}}});
      if (!calls.empty()) out["tool_calls"] = calls;
      messages.push_back(out);
      continue;
    }
    // user: plain text, or tool results (each its own "tool" message) and text.
    if (content.is_array()) {
      std::string text;
      for (const auto& b : content) {
        const std::string type = b.value("type", std::string());
        if (type == "tool_result") {
          const json& c = b.contains("content") ? b["content"] : json("");
          messages.push_back({{"role", "tool"}, {"tool_call_id", b.value("tool_use_id", std::string())}, {"content", c.is_string() ? c.get<std::string>() : textOf(c)}});
        } else if (type == "text") {
          text += (text.empty() ? "" : "\n") + b.value("text", std::string());
        }
      }
      if (!text.empty()) messages.push_back({{"role", "user"}, {"content", text}});
    } else {
      messages.push_back({{"role", "user"}, {"content", textOf(content)}});
    }
  }
  json tools = json::array();
  for (const auto& t : r.value("tools", json::array()))
    tools.push_back({{"type", "function"},
                     {"function", {{"name", t.value("name", std::string())}, {"description", t.value("description", std::string())},
                                   {"parameters", t.value("input_schema", json::object())}}}});
  json body = {{"model", r.value("model", std::string())}, {"messages", messages}, {"max_tokens", r.value("max_tokens", 4096)}};
  if (!tools.empty()) body["tools"] = tools;
  return body;
}

Turn fromOpenAi(const json& response) {
  Turn t;
  if (response.contains("error")) {
    const json& e = response["error"];
    t.error = e.is_object() ? e.value("type", std::string("error")) + ": " + e.value("message", std::string()) : e.dump();
    return t;
  }
  const json choices = response.value("choices", json::array());
  if (choices.empty() || !choices[0].is_object()) {
    t.error = "the answer has no choices";
    return t;
  }
  const json& msg = choices[0].value("message", json::object());
  if (msg.contains("content") && msg["content"].is_string() && !msg["content"].get<std::string>().empty())
    t.content.push_back({{"type", "text"}, {"text", msg["content"].get<std::string>()}});
  for (const auto& c : msg.value("tool_calls", json::array())) {
    const json fn = c.value("function", json::object());
    json input = json::object();
    const json args = fn.contains("arguments") ? fn["arguments"] : json("{}");
    if (args.is_string()) {
      input = json::parse(args.get<std::string>(), nullptr, false);
      if (input.is_discarded() || !input.is_object()) input = json::object();
    } else if (args.is_object()) {
      input = args;
    }
    t.content.push_back({{"type", "tool_use"}, {"id", c.value("id", std::string())}, {"name", fn.value("name", std::string())}, {"input", input}});
  }
  const std::string finish = choices[0].value("finish_reason", std::string());
  const bool calls = std::any_of(t.content.begin(), t.content.end(), [](const json& b) { return b["type"] == "tool_use"; });
  t.stop = calls ? "tool_use" : finish == "length" ? "max_tokens" : finish == "content_filter" ? "refusal" : "end_turn";
  const json u = response.value("usage", json::object());
  int64_t cached = 0;
  if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object())
    cached = u["prompt_tokens_details"].value("cached_tokens", (int64_t)0);
  const int64_t prompt = u.value("prompt_tokens", (int64_t)0);
  t.usage = {{"input_tokens", prompt - cached}, {"output_tokens", u.value("completion_tokens", (int64_t)0)}, {"cache_read_input_tokens", cached}};
  return t;
}

}  // namespace

std::string Provider::url() const {
  if (kind == "openai") {
    const std::string base = baseUrl.empty() ? std::string("https://api.openai.com/v1") : trimSlash(baseUrl);
    return base + "/chat/completions";
  }
  return (baseUrl.empty() ? std::string("https://api.anthropic.com") : trimSlash(baseUrl)) + "/v1/messages";
}

HttpRequest makeRequest(const Provider& p, const json& request) {
  HttpRequest h;
  h.url = p.url();
  h.headers.push_back({"content-type", "application/json"});
  if (p.kind == "openai") {
    if (!p.apiKey.empty()) h.headers.push_back({"Authorization", "Bearer " + p.apiKey});
    h.body = toOpenAi(request);
    return h;
  }
  h.headers.push_back({"x-api-key", p.apiKey});
  h.headers.push_back({"anthropic-version", "2023-06-01"});
  h.body = request;
  return h;
}

Turn readResponse(const Provider& p, const json& response) {
  if (!response.is_object()) {
    Turn t;
    t.error = "the answer is not readable";
    return t;
  }
  if (p.kind == "openai") return fromOpenAi(response);
  Turn t;
  if (response.value("type", std::string()) == "error" || response.contains("error")) {
    const json e = response.contains("error") ? response["error"] : json::object();
    t.error = e.is_object() ? e.value("type", std::string("error")) + ": " + e.value("message", std::string()) : std::string("error");
    return t;
  }
  t.content = response.contains("content") && response["content"].is_array() ? response["content"] : json::array();
  t.stop = response.value("stop_reason", std::string());
  t.usage = response.value("usage", json::object());
  return t;
}

}  // namespace core::ai
