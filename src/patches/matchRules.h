#pragma once
#include <cstdint>

// Host-chosen match rules: non-vanilla gameplay switched on for one match when
// every player supports it. The host's choice is posted as a chat line at match
// start; lockstep executes that line on the same tick on every client, which is
// where each patched client switches the rules on. See
// docs/features/match-rules.md.
//
// Install before the handshake publishes (it sets the advertised rule bits) and
// before the lobby panel (it reads the rule list).
void installMatchRules();

// What the lobby panel shows for one rule.
struct MatchRuleView {
    const char *label;
    bool fix; // a bug fix: on whenever everyone supports it, never switchable
    bool everyoneSupports; // every player in the lobby can run it
    bool proposed; // the host has it on for the next match
};

int matchRulesView(MatchRuleView *out, int max);

// The host switches rule `index` (as listed by matchRulesView) for the next match
// and saves it to the ini. False, and nothing changes, when this client is not
// the lobby owner, the rule is a fix, or it is not available to everyone.
bool matchRuleToggle(int index);
