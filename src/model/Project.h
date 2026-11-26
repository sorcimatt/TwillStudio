// The project: tracks, clips of notes, the arrangement, and instrument settings (design §8).
// Plain data; the UI edits it through Edit commands (model/Edit.h) and the engine plays immutable
// snapshots built from it (engine/Snapshot.h).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "model/Scale.h"

namespace twill {

using Tick = int32_t;
constexpr Tick kTicksPerQuarter = 960;
constexpr int kMaxTracks = 16;         // the file format's limit; a device may play fewer
constexpr int kMaxLocksPerNote = 4;
constexpr int kMaxVoicesPerTrack = 8;

// Parameters, by stable id: saved projects keep working as parameters are added. Ids are never
// reused or renumbered.
enum ParamId : uint16_t {
  kParamNone = 0,
  // Macro-oscillator.
  kParamEngine = 1,
  kParamHarmonics = 2,
  kParamTimbre = 3,
  kParamMorph = 4,
  kParamDecay = 5,
  kParamLpgColour = 6,
  kParamFmAmount = 7,
  kParamTimbreAmount = 8,
  kParamMorphAmount = 9,
  kParamPitch = 10,  // modulation destination only: semitones
  // Sampler.
  kParamSampleStart = 20,
  kParamSampleEnd = 21,
  kParamLoopStart = 22,
  kParamLoopEnd = 23,
  kParamFineTune = 24,
  kParamRootNote = 25,
  kParamSlices = 26,
  kParamLoopMode = 27,
  kParamReverse = 28,
  kParamSamplerMode = 29,
  kParamSliceMode = 30,  // 0: equal slices, 1: at transients
  // Every instrument: filter, drive, amp envelope.
  kParamFilterType = 40,
  kParamCutoff = 41,
  kParamResonance = 42,
  kParamDrive = 43,
  kParamAttack = 44,
  kParamDecayTime = 45,
  kParamSustain = 46,
  kParamRelease = 47,
  kParamAmpMode = 48,  // macro oscillator: 0 = its own low-pass gate, 1 = the ADSR
  // MIDI Out: the track's two assignable controllers (Track::midi_cc), 0-127.
  kParamMidiCcA = 60,
  kParamMidiCcB = 61,
  // Track mixer.
  kParamLevel = 100,
  kParamPan = 101,
  kParamSendChorus = 102,
  kParamSendDelay = 103,
  kParamSendReverb = 104,
  // Project: master and effects (Project::mix).
  kParamMasterLevel = 300,
  kParamEqLow = 301,
  kParamEqMid = 302,
  kParamEqHigh = 303,
  kParamReverbSize = 304,
  kParamReverbTone = 305,
  kParamDelayTime = 306,
  kParamDelayFeedback = 307,
  kParamChorusRate = 308,
  kParamChorusDepth = 309,
  kParamCompThreshold = 310,  // master compressor: 0 is off
  kParamCompRatio = 311,
  kParamCompRelease = 312,
  kParamLimiter = 313,  // master limiter: 1 on, 0 off (a soft clip instead)
};

enum class InstrumentType : uint8_t;  // below

struct ParamInfo {
  ParamId id;
  const char* name;
  float min, max, default_value;
  bool integer;
};
const ParamInfo* param_info(ParamId id);  // null for unknown ids
const std::vector<ParamInfo>& macro_params();
const std::vector<ParamInfo>& sampler_params();
const std::vector<ParamInfo>& tone_params();   // filter, drive, envelope
const std::vector<ParamInfo>& mixer_params();
const std::vector<ParamInfo>& master_params();   // master level and EQ
const std::vector<ParamInfo>& effect_params();   // reverb, delay, chorus
// A parameter's name as the app shows it for a track: "CC 74" for a MIDI Out controller.
struct Track;
std::string param_name(const Track& t, ParamId id);
// A MIDI controller's number, with its usual use if it has one ("74 (Cutoff)").
std::string cc_label(int cc);
// Display text for a parameter's value ("1.2 kHz", "40 ms", "Forward").
std::string format_param(ParamId id, float value);
// The same for a track's setting, where its engine reads one in steps: the chiptune arpeggio's chord
// and pattern, and the chord engine's chord, are named ("Minor", "Up-down, 2 octaves"). The sound
// view, parameter locks and automation lanes all show values this way.
std::string format_track_param(const Track& t, ParamId id, float value);
// The patch a six-op FM track's Harmonics picks in its bank (0-31), or -1 on any other engine.
int fm_patch_index(const Track& t);
// Whether a track plays a DX7 bank file: a macro track on a six-op FM engine with fm_bank set. The
// file then replaces all three six-op engines' banks, so the engine list shows them as one entry.
bool plays_fm_bank_file(const Track& t);
// One press of a control from `value`: to the middle of the next step where the engine reads the
// setting in steps, else by `fine` of the range (ten times that when coarse), or by 1 (4, or 12 for
// the root note, when coarse) for a whole-number setting. Kept in range.
float step_track_param(const Track& t, ParamId id, float value, int step, bool coarse, float fine);
// "Legato" for 0, else the number of voices.
std::string voices_label(int voices);
// Conversions shared by the engine and the UI.
float cutoff_hz(float v);      // 0-1 to 20 Hz - 20 kHz
float env_seconds(float v);
float env_value(float seconds);  // env_seconds' inverse, kept in range
// A kit slot's decay: the time a hit takes to fade by 60 dB, 20 ms to about 3.6 s; 0 at 1, which
// means no fade (the drum plays to the end of its sample).
float kit_decay_seconds(float v);
// The master compressor's settings in real units.
float comp_threshold_db(float v);  // 0 .. -40
float comp_ratio(float v);         // 1.5 .. 10
float comp_release_seconds(float v);    // 0-1 to 1 ms - 10 s

struct ParamLock {
  ParamId id = kParamNone;
  float value = 0;
  bool operator==(const ParamLock& o) const { return id == o.id && value == o.value; }
};

struct Note {
  uint32_t id = 0;  // unique within its clip, so edits can name a note however notes move
  Tick start = 0, length = kTicksPerQuarter / 4;
  uint8_t pitch = 60;
  uint8_t velocity = 100;
  uint8_t probability = 100;  // percent
  bool glide = false;         // mono tracks: slide from the previous note without retriggering
  uint8_t num_locks = 0;
  ParamLock locks[kMaxLocksPerNote];

  bool operator==(const Note& o) const;
  const ParamLock* lock(ParamId id) const;
  bool set_lock(ParamId id, float value);  // false if there's no room for another lock
  void clear_lock(ParamId id);
};

// Automation (design §7, "Lanes"): a parameter's value over the clip's time, as points with
// straight lines between them; before the first point and after the last, the value holds.
struct LanePoint {
  Tick tick = 0;
  float value = 0;
  bool operator==(const LanePoint& o) const { return tick == o.tick && value == o.value; }
};
struct Lane {
  ParamId param = kParamNone;
  std::vector<LanePoint> points;  // sorted by tick, at most one per tick
  bool operator==(const Lane& o) const { return param == o.param && points == o.points; }
  float value_at(Tick t) const;
  void set_point(Tick t, float value);  // adds or replaces the point at t
  void remove_point(Tick t);
};
constexpr int kMaxLanesPerClip = 8;
// Parameters a lane can automate on an instrument (and the mixer's), in menu order. Every track's
// lanes can also automate the effects and master (mix_automatable_params()); a lane for one of
// those on any playing clip sets it for the whole mix.
std::vector<ParamId> automatable_params(InstrumentType t);
const std::vector<ParamId>& mix_automatable_params();

struct Clip {
  uint32_t id = 0;
  int track = 0;
  std::string name;
  Tick length = 4 * kTicksPerQuarter;
  std::vector<Note> notes;  // kept sorted by start, then pitch
  std::vector<Lane> lanes;
  uint32_t next_note_id = 1;
  Lane* lane(ParamId p);
  const Lane* lane(ParamId p) const;

  bool operator==(const Clip& o) const;
  Note* find(uint32_t note_id);
  const Note* find(uint32_t note_id) const;
  void sort();
};

// A clip placed on a track's timeline. Plays `length` ticks starting `offset` ticks into the clip,
// looping the clip if `length` is longer than it.
struct Placement {
  Tick start = 0;
  uint32_t clip_id = 0;
  Tick offset = 0;
  Tick length = 4 * kTicksPerQuarter;
  bool operator==(const Placement& o) const {
    return start == o.start && clip_id == o.clip_id && offset == o.offset && length == o.length;
  }
};

#if TWILL_PLUGINS
enum class InstrumentType : uint8_t { Macro = 0, Sampler = 1, MidiOut = 2, Plugin = 3 };
#else
enum class InstrumentType : uint8_t { Macro = 0, Sampler = 1, MidiOut = 2 };
#endif
enum class MacroOutput : uint8_t { Main = 0, Aux = 1, Mix = 2 };
// How fast the chiptune engine's arpeggio steps while a note is held: eighth, sixteenth,
// thirty-second or sixty-fourth notes at the song's tempo.
enum class ArpRate : uint8_t { Eighth = 0, Sixteenth = 1, ThirtySecond = 2, SixtyFourth = 3 };
constexpr int kNumArpRates = 4;
inline int arp_steps_per_beat(ArpRate r) { return 2 << int(r); }
inline const char* arp_rate_name(ArpRate r) {
  static const char* const names[kNumArpRates] = {"1/8", "1/16", "1/32", "1/64"};
  return names[int(r)];
}

struct MacroSettings {
  int engine = 8;
  float harmonics = 0.5f, timbre = 0.5f, morph = 0.5f;
  float decay = 0.5f, lpg_colour = 0.5f;
  float fm_amount = 0, timbre_amount = 0, morph_amount = 0;
  MacroOutput output = MacroOutput::Main;
  ArpRate arp_rate = ArpRate::Sixteenth;  // the chiptune engine's arpeggio (track chunk version 9)
  bool operator==(const MacroSettings& o) const;
  float get(ParamId id) const;
  void set(ParamId id, float value);
};

// Multi: a multisample, its zones read from a sample library's preset (model/Multisample.h).
enum class SamplerMode : uint8_t { Single = 0, Slice = 1, Kit = 2, Multi = 3 };
enum class LoopMode : uint8_t { Off = 0, Forward = 1, PingPong = 2 };
enum class FilterType : uint8_t { Off = 0, LowPass = 1, BandPass = 2, HighPass = 3 };

constexpr int kKitSlots = 16;  // kit slot i plays on note kKitFirstNote + i
constexpr int kKitFirstNote = 36;

struct KitSlot {
  std::string sample;  // path, relative to the data folder unless absolute
  float tune = 0;      // semitones
  float level = 1;
  float pan = 0;
  int choke = 0;       // 0: none; slots sharing a group cut each other off
  float decay = 1;     // 0-1, see kit_decay_seconds(); 1: the whole sample (track chunk version 8)
  bool operator==(const KitSlot& o) const {
    return sample == o.sample && tune == o.tune && level == o.level && pan == o.pan && choke == o.choke &&
           decay == o.decay;
  }
};

struct SamplerSettings {
  SamplerMode mode = SamplerMode::Single;
  std::string sample;
  int root = 60;
  float fine = 0;               // semitones, -1..1
  float start = 0, end = 1;     // fractions of the sample
  LoopMode loop = LoopMode::Off;
  float loop_start = 0, loop_end = 1;
  bool reverse = false;
  int slices = 16;              // equal: how many; at transients: the most there can be
  bool slice_transients = false;  // track chunk version 6
  KitSlot kit[kKitSlots];
  std::string preset;  // Multi: the preset file, as `sample` is named (track chunk version 10)
  bool operator==(const SamplerSettings& o) const;
  float get(ParamId id) const;
  void set(ParamId id, float value);
};

// Modulators (design §6): four per instrument, each driving one parameter.
enum class ModType : uint8_t { Off = 0, Envelope = 1, Lfo = 2, Velocity = 3, KeyTrack = 4 };
enum class LfoShape : uint8_t { Sine = 0, Triangle = 1, Saw = 2, Square = 3, Random = 4 };
constexpr int kModulators = 4;
struct Modulator {
  ModType type = ModType::Off;
  ParamId dest = kParamNone;
  float depth = 0.5f;            // -1..1, as a share of the destination's range (pitch: of 24 semitones)
  LfoShape shape = LfoShape::Sine;
  float rate = 0.5f;             // LFO: 0-1 (free: 0.05-20 Hz; synced: 4 bars to 1/16)
  bool sync = false;
  float attack = 0, decay = 0.4f;  // envelope: 0-1, see env_seconds()
  // LFO: restart with each note, or run on per track (free: from when playback starts; synced:
  // locked to the song position, so a 1-bar LFO lines up with every bar).
  bool retrigger = true;
  bool operator==(const Modulator& o) const {
    return type == o.type && dest == o.dest && depth == o.depth && shape == o.shape && rate == o.rate &&
           sync == o.sync && attack == o.attack && decay == o.decay && retrigger == o.retrigger;
  }
};
const char* mod_type_name(ModType t);
const char* lfo_shape_name(LfoShape s);
float lfo_hz(float rate);                  // free-running rate
float lfo_sync_beats(float rate);          // synced rate: length of a cycle in beats
std::string lfo_rate_text(const Modulator& m);
// Parameters a modulator can drive on an instrument, in menu order.
std::vector<ParamId> mod_destinations(InstrumentType t);

// The part every instrument shares after its sound source (design §6).
struct ToneSettings {
  FilterType filter = FilterType::Off;
  float cutoff = 1, resonance = 0, drive = 0;
  float attack = 0, decay = 0.5f, sustain = 1, release = 0.3f;  // 0-1, see env_seconds()
  int amp_mode = 0;  // macro oscillator: 0 = its low-pass gate, 1 = the ADSR
  bool operator==(const ToneSettings& o) const;
  float get(ParamId id) const;
  void set(ParamId id, float value);
};

#if TWILL_PLUGINS
// A plugin the platform hosts (platform/Plugins.h), as a Plugin track keeps it: which, and its
// state for the file (a PLUG chunk, ProjectFile.cpp).
struct PluginSettings {
  std::string id;      // the plugin's own id: found by it wherever it is installed
  std::string path;    // where it was loaded from: a hint
  std::string name;    // as shown, even where it is not installed
  uint32_t slot = 0;   // the loaded instance it plays (the app's), unique in the project; 0: none
  std::vector<uint8_t> state;  // its own state, as last saved (not compared: the plugin's, not an edit)
  bool operator==(const PluginSettings& o) const {
    return id == o.id && path == o.path && name == o.name && slot == o.slot;
  }
};
#endif

struct Track {
  std::string name;
  uint32_t colour = 0;  // index into the theme's track colours, kept when the track moves
  InstrumentType instrument = InstrumentType::Macro;
  int voices = 1;  // 0: legato (one voice; touching or overlapping notes slur, see voices_label())
  MacroSettings macro;
  SamplerSettings sampler;
  ToneSettings tone;
  float level = 0.8f;  // 0-1
  float pan = 0;       // -1..1
  float send_chorus = 0, send_delay = 0, send_reverb = 0;  // 0-1
  Modulator mods[kModulators];
  int midi_channel = 0;  // MIDI Out tracks: 0-15 (track chunk version 4)
  int midi_cc[2] = {1, 74};  // MIDI Out tracks: the controllers kParamMidiCcA/B send (version 5)
  // Macro tracks on a six-op FM engine: a DX7 32-voice bank file (model/FmBank.h) played in place of
  // the engine's built-in bank, named as samples are; empty for the built-in one. Saved in a chunk
  // of its own (FMBK), so other builds skip it and play the built-in bank.
  std::string fm_bank;
#if TWILL_PLUGINS
  PluginSettings plugin;     // Plugin tracks
#endif
  bool mute = false, solo = false;
  std::vector<Placement> arrangement;  // sorted by start

  bool operator==(const Track& o) const;
};

// The master section and the send effects' settings.
struct MixSettings {
  float master = 1;                            // 0-1.5
  float eq_low = 0, eq_mid = 0, eq_high = 0;   // -1..1 (x 12 dB)
  float reverb_size = 0.6f, reverb_tone = 0.5f;
  float delay_time = 0.5f;                     // 0-1: 1/16, 1/8T, 1/8, dotted 1/8, 1/4, dotted 1/4, 1/2
  float delay_feedback = 0.4f;
  float chorus_rate = 0.3f, chorus_depth = 0.5f;
  // Master compressor, after the EQ: threshold 0-1 (0 off, 1 = -40 dB), ratio 0-1 (1.5:1 to 10:1),
  // release 0-1 (30 ms to 1 s). See comp_threshold_db() and friends.
  float comp_threshold = 0, comp_ratio = 0.4f, comp_release = 0.4f;
  // Last on the master: a look-ahead peak limiter (ceiling -0.3 dBFS), or when off, a tanh soft clip.
  bool limiter = true;  // PROJ version 6
  bool operator==(const MixSettings& o) const;
  float get(ParamId id) const;
  void set(ParamId id, float value);
};

// MIDI clock (24 a beat, with start, stop and continue): sent, followed, or neither.
enum class MidiClock : uint8_t { Off = 0, Send = 1, Follow = 2 };

struct Project {
  std::string name = "Untitled";
  float tempo = 120;
  int time_sig_num = 4, time_sig_den = 4;
  bool loop = true;
  Tick loop_start = 0, loop_end = 4 * 4 * kTicksPerQuarter;
  int key = 0;                            // 0 = C ... 11 = B
  ScaleType scale = ScaleType::Chromatic;  // for the piano roll's scale mode (file version 2)
  MixSettings mix;                         // file version 3
  MidiClock midi_clock = MidiClock::Off;   // file version 4
  std::vector<Track> tracks;
  std::vector<Clip> clips;
  uint32_t next_clip_id = 1;

  bool operator==(const Project& o) const;
  Clip* find_clip(uint32_t id);
  const Clip* find_clip(uint32_t id) const;
  Clip& add_clip(int track, Tick length);
  Tick song_end() const;  // end of the last placement on any track
  float track_param(int track, ParamId id) const;
  void set_track_param(int track, ParamId id, float value);
};

}  // namespace twill
