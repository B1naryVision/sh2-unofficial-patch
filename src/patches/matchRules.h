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

// What a replay needs to run a match under the rules it was played with.
struct MatchRulesState {
    uint32_t active; // rules on at the checkpoint
    uint32_t hostPeer; // the host's peer id, which a rules line must come from
    bool committed; // a rules line had already executed
};

// Game thread, on the frame the replay checkpoint is saved.
void matchRulesCapture(MatchRulesState *out);

// A replay load: the recorded state replaces the live one until ReplayEnd, and a
// rules line in the replay commits exactly as it did live. `state` null is an
// older sidecar that did not record it: single player then gets every fix, as
// live, and multiplayer starts vanilla and takes the first rules line from
// anyone.
void matchRulesReplayBegin(const MatchRulesState *state, bool networked);
void matchRulesReplayEnd();
