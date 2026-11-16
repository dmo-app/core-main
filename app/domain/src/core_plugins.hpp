// Core's tools as cards (Diogo 2026-11-16, brief "painel de tools só com plugins"): the cards of the Tools panel, the
// Plugins menu's Core section and the "Tools" submenu of a track's right-click are this one list, and the AI opens
// and calls the same tools by the same ids (role_tools.hpp: songToolCatalog). No Core sound: each tool writes notes,
// sends or automation, or puts the plugin of a role (Settings > Roles) on the track. S-Core is not a card (Diogo:
// "não vamos usar"); Core Glitch and the Kick Designer wait for a decision.
#pragma once
#include <string>
#include <vector>

namespace core::doc {

struct CorePluginInfo {
  std::string id;                    // "pcore", "fcore", "rcore"... the AI's id too
  std::string shortName;             // the card's title: "P-Core"
  std::string word;                  // the small word on the card's right: "Patterns"
  std::string about;                 // what it does, in plain words
  std::vector<std::string> roles;    // the roles whose plugins it plays (empty: it only writes notes)
  std::string opens;                 // "window" (its own window) | "tool" (a menu: where, then it writes)
  std::vector<std::string> aliases;  // other names the AI may use ("p-core", "patterns", "pum"...)
};
const std::vector<CorePluginInfo>& corePlugins();  // card order
// By id or any alias, case ignored; nullptr when none.
const CorePluginInfo* findCorePlugin(const std::string& idOrName);
// Problems with the list itself (an empty field, an id or alias used twice, an unknown role): empty when it is right.
std::vector<std::string> corePluginListProblems();

}  // namespace core::doc
