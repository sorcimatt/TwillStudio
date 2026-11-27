// The sound picker's lists: a track's sound chosen by kind rather than by engine number. DX7
// sounds by category (from the banks in banks/Categories, one group per category, "Bass 1.syx",
// "Bass 2.syx"... together), Twill's own DX7 banks, its other synth engines, the sampler and MIDI out.
#pragma once
#include <string>
#include <vector>

#include "model/Project.h"

namespace twill {

class App;

struct PickerItem {
  enum class Kind { Dx7, Engine, Sampler, MidiOut, BankFiles };
  Kind kind = Kind::Engine;
  std::string label;  // as listed: "SOLID BASS", "Wavetable"
  std::string where;  // shown beside it, dimmer: "Bass 1 #1"
  int engine = 0;     // Dx7 (2-4) and Engine
  std::string bank;   // Dx7: the bank file (relative to the data folder), empty for a built-in one
  int patch = 0;      // Dx7: 0-31
};

struct PickerGroup {
  std::string label;              // "DX7: Bass", "Synth engines"
  std::vector<PickerItem> items;  // empty: the group is one choice, `action`
  PickerItem action;              // Sampler, MIDI out, or BankFiles (opens the bank browser)
};

// Every group, in order: DX7 categories (a known order first: Bass, Piano & Keys, Organ...; then
// any others by name), Twill's built-in DX7 banks, the other synth engines, the sampler, MIDI out,
// and "DX7: a bank file" (the bank browser). Silent "(empty)" voices are left out.
std::vector<PickerGroup> sound_picker_groups(App& app);

// Sets a track's sound to `item` (Dx7, Engine, Sampler or MidiOut), leaving the rest as it was.
void apply_picker_item(Track& t, const PickerItem& item);

// Where a track's current sound is in `groups`: group and item (-1 if not in a list).
void find_in_picker(const std::vector<PickerGroup>& groups, const Track& t, int& group, int& item);

}  // namespace twill
