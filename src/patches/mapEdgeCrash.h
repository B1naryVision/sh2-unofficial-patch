#pragma once

// Guards an unbounded map-flag read in GlowWorm::KnotFormation that crashes when
// troops/formations are pushed off the edge of the map. See
// docs/bugs/map-edge-formation-crash.md.
void installMapEdgeCrashFix();
