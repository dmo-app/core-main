#include "ai_protocol.hpp"
#include <algorithm>
#include <set>

namespace core::ai::ipc {

std::string frame(const json& message, size_t maxBytes) {
  std::string payload;
  try {
    payload = message.dump(-1, ' ', false, json::error_handler_t::replace);
  } catch (...) {
    return "";
  }
  if (payload.empty() || payload.size() > maxBytes || payload.size() > 0xFFFFFFFFu) return "";
  const uint32_t n = (uint32_t)payload.size();
  std::string out;
  out.reserve(4 + payload.size());
  out.push_back((char)(n & 0xFF));
  out.push_back((char)((n >> 8) & 0xFF));
  out.push_back((char)((n >> 16) & 0xFF));
  out.push_back((char)((n >> 24) & 0xFF));
  out += payload;
  return out;
}

int depthOf(std::string_view text) {
  int depth = 0, deepest = 0;
  bool inString = false, escaped = false;
  for (char c : text) {
    if (inString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') inString = true;
    else if (c == '{' || c == '[') deepest = std::max(deepest, ++depth);
    else if (c == '}' || c == ']') {
      if (--depth < 0) return -1;
    }
  }
  return depth == 0 && !inString ? deepest : -1;
}

json parseMessage(std::string_view payload, std::string* why) {
  auto fail = [&](const std::string& w) -> json {
    if (why) *why = w;
    return nullptr;
  };
  const int depth = depthOf(payload);
  if (depth < 0) return fail("the JSON brackets do not match");
  if (depth > kMaxDepth) return fail("the JSON nests deeper than " + std::to_string(kMaxDepth));
  json j = json::parse(payload.begin(), payload.end(), nullptr, false);  // invalid UTF-8 or JSON: discarded
  if (j.is_discarded()) return fail("not valid JSON (UTF-8)");
  if (!j.is_object()) return fail("a message is a JSON object");
  if (!j.contains("type") || !j["type"].is_string()) return fail("a message has a string \"type\"");
  return j;
}

std::vector<json> Decoder::feed(const char* data, size_t size) {
  std::vector<json> out;
  if (failed()) return out;
  buf_.append(data, size);
  for (;;) {
    if (buf_.size() < 4) break;
    const auto b = [&](int i) { return (uint32_t)(unsigned char)buf_[(size_t)i]; };
    const size_t n = b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24);
    if (n == 0) {
      error_ = "an empty frame";
      break;
    }
    if (n > max_) {
      error_ = "a frame of " + std::to_string(n) + " bytes (the limit is " + std::to_string(max_) + ")";
      break;
    }
    if (buf_.size() < 4 + n) break;
    std::string why;
    json m = parseMessage(std::string_view(buf_).substr(4, n), &why);
    if (m.is_null()) {
      error_ = why;
      break;
    }
    out.push_back(std::move(m));
    buf_.erase(0, 4 + n);
  }
  if (failed()) buf_.clear();
  return out;
}

namespace {

// The checks of one message: each returns "" or why.
struct Check {
  const json& m;
  std::string why;
  bool ok() const { return why.empty(); }
  Check& str(const char* key, size_t max, bool required = true) {
    if (!ok()) return *this;
    if (!m.contains(key)) {
      if (required) why = std::string("\"") + key + "\" is missing";
      return *this;
    }
    const json& v = m[key];
    if (!v.is_string()) why = std::string("\"") + key + "\" is not text";
    else if (v.get_ref<const std::string&>().size() > max) why = std::string("\"") + key + "\" is longer than " + std::to_string(max);
    return *this;
  }
  Check& strings(const char* key, size_t maxItems, size_t maxEach, bool required = false) {
    if (!ok()) return *this;
    if (!m.contains(key)) {
      if (required) why = std::string("\"") + key + "\" is missing";
      return *this;
    }
    const json& v = m[key];
    if (!v.is_array()) { why = std::string("\"") + key + "\" is not a list"; return *this; }
    if (v.size() > maxItems) { why = std::string("\"") + key + "\" has more than " + std::to_string(maxItems) + " items"; return *this; }
    for (const auto& s : v)
      if (!s.is_string() || s.get_ref<const std::string&>().size() > maxEach) { why = std::string("\"") + key + "\" has an item that is not short text"; return *this; }
    return *this;
  }
  Check& boolean(const char* key, bool required = true) {
    if (!ok()) return *this;
    if (!m.contains(key)) { if (required) why = std::string("\"") + key + "\" is missing"; return *this; }
    if (!m[key].is_boolean()) why = std::string("\"") + key + "\" is not true/false";
    return *this;
  }
  Check& object(const char* key, bool nullable, bool required = true) {
    if (!ok()) return *this;
    if (!m.contains(key)) { if (required) why = std::string("\"") + key + "\" is missing"; return *this; }
    const json& v = m[key];
    if (!(v.is_object() || (nullable && v.is_null()))) why = std::string("\"") + key + "\" is not an object";
    return *this;
  }
  Check& number(const char* key, double lo, double hi, bool required = true) {
    if (!ok()) return *this;
    if (!m.contains(key)) { if (required) why = std::string("\"") + key + "\" is missing"; return *this; }
    const json& v = m[key];
    if (!v.is_number() || !(v.get<double>() >= lo && v.get<double>() <= hi)) why = std::string("\"") + key + "\" is not a number in range";
    return *this;
  }
};

constexpr size_t kId = 64, kShort = 4096, kLong = 64 * 1024;

}  // namespace

std::string checkFromAi(const json& m) {
  if (!m.is_object() || !m.contains("type") || !m["type"].is_string()) return "a message has a string \"type\"";
  const std::string& type = m["type"].get_ref<const std::string&>();
  Check c{m, ""};
  if (type == "hello") {
    c.number("protocol", kProtocol, kProtocol).strings("models", 32, 100).strings("notices", 16, 1000).str("name", 100, false);
  } else if (type == "status" || type == "partial") {
    c.str("id", kId).str("text", kShort);
  } else if (type == "answer") {
    c.str("id", kId).str("reply", kLong, false).str("error", kShort, false).strings("log", 128, 1000).object("proposal", true, false)
        .object("cost", false, false);
    if (c.ok() && m.contains("cost")) {
      Check k{m["cost"], ""};
      k.number("message", 0, 1e6, false).number("chat", 0, 1e6, false).number("requests", 0, 1e6, false);
      if (!k.ok()) c.why = "cost: " + k.why;
    }
  } else if (type == "recipe") {
    c.str("id", kId).str("recipeId", kId);
  } else if (type == "result") {
    c.str("op", 32).boolean("ok").str("text", kLong, false);
  } else if (type == "error") {
    c.str("id", kId, false).str("text", kShort);
  } else {
    return "unknown message type \"" + type.substr(0, 32) + "\"";
  }
  return c.ok() ? "" : type + ": " + c.why;
}

std::string checkToAi(const json& m) {
  if (!m.is_object() || !m.contains("type") || !m["type"].is_string()) return "a message has a string \"type\"";
  const std::string& type = m["type"].get_ref<const std::string&>();
  Check c{m, ""};
  if (type == "hello") {
    c.number("protocol", kProtocol, kProtocol);
    if (c.ok() && (!m.contains("catalog") || !m["catalog"].is_array())) c.why = "\"catalog\" is not a list";
    if (c.ok()) c.strings("scope", 64, 64, true);
  } else if (type == "ask") {
    c.str("id", kId).str("text", kLong).object("song", false).object("zone", true, false).object("settings", false, false);
  } else if (type == "cancel" || type == "applied") {
    c.str("id", kId);
  } else if (type == "feedback") {
    c.str("recipeId", kId).boolean("liked").str("note", kShort, false);
  } else if (type == "memory" || type == "chats") {
    c.str("op", 32).str("file", 4096, false);
  } else if (type == "bye") {
  } else {
    return "unknown message type \"" + type.substr(0, 32) + "\"";
  }
  return c.ok() ? "" : type + ": " + c.why;
}

}  // namespace core::ai::ipc
