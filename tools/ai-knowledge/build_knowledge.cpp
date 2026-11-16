// core-ai-knowledge <core-manual.jsonl> <core-ai-knowledge.sqlite>: builds the AI's local knowledge base (app/ai).
#include <iostream>
#include "knowledge.hpp"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: core-ai-knowledge <in.jsonl> <out.sqlite>\n";
    return 2;
  }
  std::string why;
  const int n = core::ai::buildKnowledge(argv[1], argv[2], &why);
  if (n < 0) {
    std::cerr << "core-ai-knowledge: " << why << "\n";
    return 1;
  }
  std::cout << n << " chunks -> " << argv[2] << "\n";
  return 0;
}
