// Core's check of what its AI proposes (Fase 4, 2026-11-16). The AI runs in another process and Core trusts nothing
// it sends: a proposal is admitted whole or refused whole, never in part, and nothing changes the song until Diogo
// presses Apply (or set "trusted" in Core's own panel settings). The check, in order:
//   1. its shape: an object, "id" and "summary" short text, "calls" a list of 1 to kMaxCalls objects with a "fn";
//   2. the scope (ai_scope.hpp): every call is a music-creation action; one outside it refuses the proposal;
//   3. the permissions (catalog.hpp, ai-settings.json, read by Core): read only refuses any call, a denied action too;
//   4. the zone: the one Core sent with the request replaces whatever the proposal says;
//   5. Core's own check against the song (control::validateProposal): one call left out refuses the proposal.
// The verification and the Undo stay in Core: they are the DAW's correctness, not AI.
#pragma once
#include <string>
#include <vector>
#include "calls.hpp"
#include "catalog.hpp"

namespace core::ai {

constexpr size_t kMaxCalls = 64;

struct Admission {
  bool ok = false;
  std::string why;            // refused: why (for the panel and the log)
  control::Proposal proposal; // admitted: the checked proposal, ready for applyProposal
  nlohmann::json raw;         // admitted: the proposal as checked (with Core's zone)
};

// `functions` must outlive the admitted proposal (each of its calls points at its function).
Admission admitProposal(const nlohmann::json& raw, const std::vector<control::CoreFunction>& functions, const doc::Document& song,
                        const doc::SampleIndex* samples, const Permissions& permissions, const control::Zone& zone);

// Apply checks a proposal again against the song as it is then (it may have changed since the AI made it). Every call
// must still fit: "" when they do, else why nothing should be applied (never only a part of what was shown).
std::string changedSince(const nlohmann::json& proposal, const control::Proposal& now);

}  // namespace core::ai
