// What the audio thread plays: an immutable copy of the project, built on the UI thread after each
// edit and handed over by pointer (design §6, "Threads and edits"). Nothing in it changes after
// it's built, so the audio thread reads it without locks.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "model/Multisample.h"
#include "model/Project.h"
#if TWILL_PLUGINS
#include "platform/Plugins.h"
#endif

namespace twill {

struct SampleData;
// Finds a loaded sample by path (on the UI thread, while building a snapshot). The samples must
// stay alive while any snapshot or voice may use them (the app's sample cache keeps them).
using SampleResolver = std::function<const SampleData*(const std::string& path)>;
#if TWILL_PLUGINS
// The loaded plugin a Plugin track plays (on the UI thread, while building a snapshot); null if none.
// The app keeps it alive while any snapshot may use it.
using PluginResolver = std::function<PluginInstance*(const Track& t)>;
#endif
// The same for a multisample's preset, read with its zones (and their samples loaded, so `samples`
// finds them); null when it can't be read.
using MultisampleResolver = std::function<const Multisample*(const std::string& path)>;
// The same for a track's DX7 bank file (Track::fm_bank): its kFmBankBytes of voices, or null when it
// can't be read (the engine's built-in bank plays). The app keeps the bytes alive while any snapshot
// or voice may use them.
using FmBankResolver = std::function<const uint8_t*(const std::string& path)>;

// A multisample zone as the audio thread plays it: its sample found, and its frames at 48 kHz.
struct SnapZone {
  const SampleData* sample = nullptr;
  int index = 0;  // in the Multisample's zones (missing ones aren't here, so this can differ)
  int lo_key = 0, hi_key = 127, lo_vel = 0, hi_vel = 127, root = 60;
  float tune = 0, gain = 1, pan = 0;
  int begin = 0, end = 0;        // frames
  bool loop = false, one_shot = false;
  int loop_a = 0, loop_b = 0;    // frames
  int xfade = 0;                 // the loop's crossfade, frames (fits the loop and what's before it)
  bool xfade_linear = false;
  float keytrack = 1, veltrack = 1, lo_rand = 0, hi_rand = 1;
  bool tune_after_keytrack = false;
  SeqMode seq = SeqMode::Always;
  int seq_group = 0, seq_length = 0, seq_position = 1;
};

// SamplerSettings without its strings, so the audio thread can copy it freely.
struct SnapSampler {
  SamplerMode mode = SamplerMode::Single;
  int root = 60;
  float fine = 0, start = 0, end = 1;
  LoopMode loop = LoopMode::Off;
  float loop_start = 0, loop_end = 1;
  bool reverse = false;
  int slices = 16;
  bool slice_transients = false;
  struct Slot {
    float tune = 0, level = 1, pan = 0;
    int choke = 0;
    float decay = 1;
  } kit[kKitSlots];
  void set(ParamId id, float value);  // for parameter locks
};

// A lane's value at a moment, as the engine applies it.
struct LaneValue {
  ParamId id = kParamNone;
  float value = 0;
};

struct SnapClip {
  Tick length = 0;
  std::vector<Note> notes;  // sorted by start
  std::vector<Lane> lanes;
};

struct SnapPlacement {
  Tick start = 0, length = 0, offset = 0;
  const SnapClip* clip = nullptr;
};

struct SnapTrack {
  InstrumentType instrument = InstrumentType::Macro;
  int voices = 1;
  bool legato = false;  // one voice; touching or overlapping notes slur instead of retriggering
  MacroSettings macro;
  const uint8_t* fm_bank = nullptr;  // the six-op FM engines' bank, if the track has its own
  SnapSampler sampler;
  ToneSettings tone;
  const SampleData* sample = nullptr;              // the sampler's sample (single and slice modes)
  const SampleData* kit[kKitSlots] = {};           // kit mode
  std::vector<int> slice_points;  // slicing at transients: each slice's first frame, then the last's end
  std::vector<SnapZone> zones;    // multi mode: the preset's zones whose samples are loaded
  float level = 0.8f, pan = 0;
  float send_chorus = 0, send_delay = 0, send_reverb = 0;
  Modulator mods[kModulators];
  int midi_channel = 0;
  int midi_cc[2] = {1, 74};
#if TWILL_PLUGINS
  PluginInstance* plugin = nullptr;  // Plugin tracks: what plays them
#endif
  bool audible = true;  // mute and solo, already resolved
  std::vector<SnapPlacement> placements;  // sorted by start
};

struct Snapshot {
  uint64_t revision = 0;  // the EditHistory revision it was built from
  float tempo = 120;
  bool midi_clock_out = false;  // 24 clocks a beat, with start and stop, while playing
  bool loop = false;
  Tick loop_start = 0, loop_end = 0;
  MixSettings mix;
  std::vector<SnapClip> clips;    // placements point into this; never resized after building
  std::vector<SnapTrack> tracks;
};

// Builds a snapshot of the first `max_tracks` tracks (the rest stay in the project, silent on this
// device; design §6, "Track count").
std::unique_ptr<Snapshot> build_snapshot(const Project& p, int max_tracks, uint64_t revision,
                                         const SampleResolver& samples = nullptr,
                                         const MultisampleResolver& multisamples = nullptr
#if TWILL_PLUGINS
                                         , const PluginResolver& plugins = nullptr
#endif
                                         , const FmBankResolver& fm_banks = nullptr
);

}  // namespace twill
