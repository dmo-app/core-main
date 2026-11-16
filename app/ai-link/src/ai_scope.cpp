#include "ai_scope.hpp"
#include <algorithm>

namespace core::ai {

const std::vector<std::string>& creationScope() {
  static const std::vector<std::string> s{"pcore", "shape", "arrange", "plays", "track", "roll", "stutter", "voicestutter"};
  return s;
}

bool inCreationScope(const std::string& functionId) {
  const auto& s = creationScope();
  return std::find(s.begin(), s.end(), functionId) != s.end();
}

std::vector<control::CoreFunction> exposedFunctions(const std::vector<control::CoreFunction>& all) {
  std::vector<control::CoreFunction> out;
  for (const auto& f : all)
    if (inCreationScope(f.id)) out.push_back(f);
  return out;
}

}  // namespace core::ai
