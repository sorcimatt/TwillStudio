#include "app/SoundPicker.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "app/App.h"
#include "engine/PlaitsVoice.h"
#include "model/FmBank.h"
#include "plaits/resources.h"  // the six-op FM engines' built-in banks

namespace twill {
namespace {

constexpr const char* kCategoriesFolder = "banks/Categories/";

// "Synth & Lead 01" -> "Synth & Lead": the category a bank file belongs to.
std::string category_of(const std::string& title) {
  size_t end = title.size();
  while (end > 0 && std::isdigit(static_cast<unsigned char>(title[end - 1]))) --end;
  while (end > 0 && title[end - 1] == ' ') --end;
  return end > 0 ? title.substr(0, end) : title;
}

void add_bank(std::vector<PickerItem>& items, const uint8_t* bank, const std::string& path, int engine,
              const std::string& where) {
  for (int i = 0; i < kFmBankVoices; ++i) {
    const std::string name = fm_voice_name(bank, i);
    if (name.empty() || name == "(empty)") continue;
    PickerItem it;
    it.kind = PickerItem::Kind::Dx7;
    it.label = name;
    it.where = where + " #" + std::to_string(i + 1);
    it.engine = engine;
    it.bank = path;
    it.patch = i;
    items.push_back(it);
  }
}

}  // namespace

std::vector<PickerGroup> sound_picker_groups(App& app) {
  std::vector<PickerGroup> groups;
  Platform& p = app.platform();

  // DX7 categories, from the category banks.
  std::vector<Platform::DirEntry> files;
  std::map<std::string, std::vector<std::string>> by_category;  // category -> bank titles, sorted
  if (p.list_dir((p.data_dir() + kCategoriesFolder).c_str(), files))
    for (const Platform::DirEntry& f : files)
      if (!f.is_dir && is_fm_bank_file(f.name)) by_category[category_of(fm_bank_title(f.name))].push_back(f.name);
  static const char* const kOrder[] = {"Bass",        "Piano & Keys",  "Organ",          "Plucked & Guitar",
                                       "Bells & Mallets", "Pads & Strings", "Brass & Winds", "Synth & Lead",
                                       "Drums & Percussion", "Effects", "Fading tones"};
  std::vector<std::string> order(std::begin(kOrder), std::end(kOrder));
  for (const auto& c : by_category)
    if (std::find(order.begin(), order.end(), c.first) == order.end()) order.push_back(c.first);
  for (const std::string& cat : order) {
    auto found = by_category.find(cat);
    if (found == by_category.end()) continue;
    std::vector<std::string> names = found->second;
    std::sort(names.begin(), names.end());
    PickerGroup g;
    g.label = "DX7: " + cat;
    for (const std::string& file : names) {
      const std::string path = std::string(kCategoriesFolder) + file;
      if (const uint8_t* bank = app.load_fm_bank(path)) add_bank(g.items, bank, path, 2, fm_bank_title(file));
    }
    if (!g.items.empty()) groups.push_back(std::move(g));
  }

  // Twill's own DX7 sounds, the three built-in banks.
  PickerGroup own;
  own.label = "DX7: Twill's own";
  for (int b = 0; b < 3; ++b) add_bank(own.items, plaits::fm_patches_table[b], "", 2 + b, "bank " + std::to_string(b + 1));
  groups.push_back(std::move(own));

  // The other synth engines.
  PickerGroup engines;
  engines.label = "Synth engines";
  for (int e = 0; e < PlaitsVoice::kNumEngines; ++e) {
    if (e >= 2 && e <= 4) continue;
    PickerItem it;
    it.kind = PickerItem::Kind::Engine;
    it.label = PlaitsVoice::engine_name(e);
    it.engine = e;
    engines.items.push_back(it);
  }
  groups.push_back(std::move(engines));

  PickerGroup sampler;
  sampler.label = "Sampler: drums, samples";
  sampler.action.kind = PickerItem::Kind::Sampler;
  groups.push_back(std::move(sampler));
  PickerGroup midi;
  midi.label = "MIDI out: an external synth";
  midi.action.kind = PickerItem::Kind::MidiOut;
  groups.push_back(std::move(midi));
  PickerGroup files_group;
  files_group.label = "DX7: a bank file...";
  files_group.action.kind = PickerItem::Kind::BankFiles;
  groups.push_back(std::move(files_group));
  return groups;
}

void apply_picker_item(Track& t, const PickerItem& item) {
  switch (item.kind) {
    case PickerItem::Kind::Dx7:
      t.instrument = InstrumentType::Macro;
      t.macro.engine = item.engine;
      t.fm_bank = item.bank;
      t.macro.harmonics = (item.patch + 0.5f) / 32.0f / 1.02f;  // the middle of the patch's step
      break;
    case PickerItem::Kind::Engine:
      t.instrument = InstrumentType::Macro;
      t.macro.engine = item.engine;
      break;
    case PickerItem::Kind::Sampler: t.instrument = InstrumentType::Sampler; break;
    case PickerItem::Kind::MidiOut: t.instrument = InstrumentType::MidiOut; break;
    case PickerItem::Kind::BankFiles: break;
  }
}

void find_in_picker(const std::vector<PickerGroup>& groups, const Track& t, int& group, int& item) {
  group = item = -1;
  for (int g = 0; g < int(groups.size()); ++g) {
    const PickerGroup& pg = groups[size_t(g)];
    if (pg.items.empty()) {
      if ((pg.action.kind == PickerItem::Kind::Sampler && t.instrument == InstrumentType::Sampler) ||
          (pg.action.kind == PickerItem::Kind::MidiOut && t.instrument == InstrumentType::MidiOut))
        group = g;
      continue;
    }
    for (int i = 0; i < int(pg.items.size()); ++i) {
      const PickerItem& it = pg.items[size_t(i)];
      if (t.instrument != InstrumentType::Macro) break;
      const bool match = it.kind == PickerItem::Kind::Dx7
                             ? t.macro.engine >= 2 && t.macro.engine <= 4 && fm_patch_index(t) == it.patch &&
                                   t.fm_bank == it.bank && (!it.bank.empty() || t.macro.engine == it.engine)
                             : t.macro.engine == it.engine;
      if (match) {
        group = g;
        item = i;
        return;
      }
    }
  }
}

}  // namespace twill
