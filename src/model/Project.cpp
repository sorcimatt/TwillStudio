#include "model/Project.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace twill {

namespace {
const std::vector<ParamInfo> kMacroParams = {
    {kParamEngine, "Engine", 0, 23, 8, true},
    {kParamHarmonics, "Harmonics", 0, 1, 0.5f, false},
    {kParamTimbre, "Timbre", 0, 1, 0.5f, false},
    {kParamMorph, "Morph", 0, 1, 0.5f, false},
    {kParamDecay, "Decay", 0, 1, 0.5f, false},
    {kParamLpgColour, "LPG colour", 0, 1, 0.5f, false},
    {kParamFmAmount, "FM amount", -1, 1, 0, false},
    {kParamTimbreAmount, "Timbre amount", -1, 1, 0, false},
    {kParamMorphAmount, "Morph amount", -1, 1, 0, false},
    {kParamPitch, "Pitch", -24, 24, 0, false},
};
const std::vector<ParamInfo> kSamplerParams = {
    {kParamSamplerMode, "Mode", 0, 3, 0, true},
    {kParamRootNote, "Root note", 0, 127, 60, true},
    {kParamFineTune, "Fine tune", -1, 1, 0, false},
    {kParamSampleStart, "Start", 0, 1, 0, false},
    {kParamSampleEnd, "End", 0, 1, 1, false},
    {kParamLoopMode, "Loop", 0, 2, 0, true},
    {kParamLoopStart, "Loop start", 0, 1, 0, false},
    {kParamLoopEnd, "Loop end", 0, 1, 1, false},
    {kParamReverse, "Reverse", 0, 1, 0, true},
    {kParamSliceMode, "Slice", 0, 1, 0, true},
    {kParamSlices, "Slices", 2, 64, 16, true},
};
const std::vector<ParamInfo> kToneParams = {
    {kParamFilterType, "Filter", 0, 3, 0, true},
    {kParamCutoff, "Cutoff", 0, 1, 1, false},
    {kParamResonance, "Resonance", 0, 1, 0, false},
    {kParamDrive, "Drive", 0, 1, 0, false},
    {kParamAmpMode, "Amp", 0, 1, 0, true},
    {kParamAttack, "Attack", 0, 1, 0, false},
    {kParamDecayTime, "Decay", 0, 1, 0.5f, false},
    {kParamSustain, "Sustain", 0, 1, 1, false},
    {kParamRelease, "Release", 0, 1, 0.3f, false},
};
const std::vector<ParamInfo> kMixerParams = {
    {kParamLevel, "Level", 0, 1, 0.8f, false},
    {kParamPan, "Pan", -1, 1, 0, false},
    {kParamSendChorus, "Chorus send", 0, 1, 0, false},
    {kParamSendDelay, "Delay send", 0, 1, 0, false},
    {kParamSendReverb, "Reverb send", 0, 1, 0, false},
};
const std::vector<ParamInfo> kMasterParams = {
    {kParamMasterLevel, "Master level", 0, 1.5f, 1, false},
    {kParamEqLow, "EQ low", -1, 1, 0, false},
    {kParamEqMid, "EQ mid", -1, 1, 0, false},
    {kParamEqHigh, "EQ high", -1, 1, 0, false},
};
const std::vector<ParamInfo> kEffectParams = {
    {kParamReverbSize, "Reverb size", 0, 1, 0.6f, false},
    {kParamReverbTone, "Reverb tone", 0, 1, 0.5f, false},
    {kParamDelayTime, "Delay time", 0, 1, 0.5f, false},
    {kParamDelayFeedback, "Delay feedback", 0, 1, 0.4f, false},
    {kParamChorusRate, "Chorus rate", 0, 1, 0.3f, false},
    {kParamChorusDepth, "Chorus depth", 0, 1, 0.5f, false},
    {kParamCompThreshold, "Comp threshold", 0, 1, 0, false},
    {kParamCompRatio, "Comp ratio", 0, 1, 0.4f, false},
    {kParamCompRelease, "Comp release", 0, 1, 0.4f, false},
    {kParamLimiter, "Limiter", 0, 1, 1, true},
};
const std::vector<ParamInfo> kMidiParams = {
    {kParamMidiCcA, "CC A", 0, 127, 64, true},
    {kParamMidiCcB, "CC B", 0, 127, 64, true},
};
const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
}  // namespace

const char* mod_type_name(ModType t) {
  switch (t) {
    case ModType::Off: return "Off";
    case ModType::Envelope: return "Envelope";
    case ModType::Lfo: return "LFO";
    case ModType::Velocity: return "Velocity";
    case ModType::KeyTrack: return "Key tracking";
  }
  return "?";
}

const char* lfo_shape_name(LfoShape s) {
  switch (s) {
    case LfoShape::Sine: return "Sine";
    case LfoShape::Triangle: return "Triangle";
    case LfoShape::Saw: return "Saw";
    case LfoShape::Square: return "Square";
    case LfoShape::Random: return "Random";
  }
  return "?";
}

float lfo_hz(float rate) { return 0.05f * std::pow(400.0f, std::min(1.0f, std::max(0.0f, rate))); }

namespace {
const float kSyncBeats[] = {16, 8, 4, 2, 1, 0.5f, 0.25f};
const char* const kSyncNames[] = {"4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16"};
int sync_index(float rate) { return std::max(0, std::min(6, int(rate * 6 + 0.5f))); }
}  // namespace

float lfo_sync_beats(float rate) { return kSyncBeats[sync_index(rate)]; }

std::string lfo_rate_text(const Modulator& m) {
  if (m.sync) return kSyncNames[sync_index(m.rate)];
  char b[24];
  const float hz = lfo_hz(m.rate);
  std::snprintf(b, sizeof b, hz < 1 ? "%.2f Hz" : "%.1f Hz", hz);
  return b;
}

std::vector<ParamId> mod_destinations(InstrumentType t) {
#if TWILL_PLUGINS
  if (t == InstrumentType::Plugin) return {};  // a plugin's sound is its own
#endif
  std::vector<ParamId> d = {kParamPitch};
  if (t == InstrumentType::Macro)
    for (ParamId id : {kParamHarmonics, kParamTimbre, kParamMorph, kParamDecay, kParamLpgColour}) d.push_back(id);
  else
    for (ParamId id : {kParamSampleStart, kParamFineTune}) d.push_back(id);
  for (ParamId id : {kParamCutoff, kParamResonance, kParamDrive}) d.push_back(id);
  return d;
}

const std::vector<ParamInfo>& macro_params() { return kMacroParams; }
const std::vector<ParamInfo>& sampler_params() { return kSamplerParams; }
const std::vector<ParamInfo>& tone_params() { return kToneParams; }
const std::vector<ParamInfo>& mixer_params() { return kMixerParams; }
const std::vector<ParamInfo>& master_params() { return kMasterParams; }
const std::vector<ParamInfo>& effect_params() { return kEffectParams; }

bool MixSettings::operator==(const MixSettings& o) const {
  return master == o.master && eq_low == o.eq_low && eq_mid == o.eq_mid && eq_high == o.eq_high &&
         reverb_size == o.reverb_size && reverb_tone == o.reverb_tone && delay_time == o.delay_time &&
         delay_feedback == o.delay_feedback && chorus_rate == o.chorus_rate && chorus_depth == o.chorus_depth &&
         comp_threshold == o.comp_threshold && comp_ratio == o.comp_ratio && comp_release == o.comp_release &&
         limiter == o.limiter;
}

float MixSettings::get(ParamId id) const {
  switch (id) {
    case kParamMasterLevel: return master;
    case kParamEqLow: return eq_low;
    case kParamEqMid: return eq_mid;
    case kParamEqHigh: return eq_high;
    case kParamReverbSize: return reverb_size;
    case kParamReverbTone: return reverb_tone;
    case kParamDelayTime: return delay_time;
    case kParamDelayFeedback: return delay_feedback;
    case kParamChorusRate: return chorus_rate;
    case kParamChorusDepth: return chorus_depth;
    case kParamCompThreshold: return comp_threshold;
    case kParamCompRatio: return comp_ratio;
    case kParamCompRelease: return comp_release;
    case kParamLimiter: return limiter ? 1.0f : 0.0f;
    default: return 0;
  }
}

void MixSettings::set(ParamId id, float v) {
  if (const ParamInfo* info = param_info(id)) v = std::min(info->max, std::max(info->min, v));
  switch (id) {
    case kParamMasterLevel: master = v; break;
    case kParamEqLow: eq_low = v; break;
    case kParamEqMid: eq_mid = v; break;
    case kParamEqHigh: eq_high = v; break;
    case kParamReverbSize: reverb_size = v; break;
    case kParamReverbTone: reverb_tone = v; break;
    case kParamDelayTime: delay_time = v; break;
    case kParamDelayFeedback: delay_feedback = v; break;
    case kParamChorusRate: chorus_rate = v; break;
    case kParamChorusDepth: chorus_depth = v; break;
    case kParamCompThreshold: comp_threshold = v; break;
    case kParamCompRatio: comp_ratio = v; break;
    case kParamCompRelease: comp_release = v; break;
    case kParamLimiter: limiter = v > 0.5f; break;
    default: break;
  }
}

float cutoff_hz(float v) { return 20.0f * std::pow(1000.0f, std::min(1.0f, std::max(0.0f, v))); }
std::string voices_label(int voices) { return voices == 0 ? "Legato" : std::to_string(voices); }

float comp_threshold_db(float v) { return -40.0f * std::min(1.0f, std::max(0.0f, v)); }
float comp_ratio(float v) { return 1.5f + 8.5f * std::min(1.0f, std::max(0.0f, v)); }
float comp_release_seconds(float v) { return 0.03f * std::pow(1.0f / 0.03f, std::min(1.0f, std::max(0.0f, v))); }

float kit_decay_seconds(float v) {
  if (v >= 1) return 0;
  return 0.02f * std::pow(200.0f, std::max(0.0f, v) / 0.98f);
}

float env_seconds(float v) { return 0.001f * std::pow(10000.0f, std::min(1.0f, std::max(0.0f, v))); }
float env_value(float seconds) {
  return seconds <= 0.001f ? 0.0f : std::min(1.0f, std::log10(seconds / 0.001f) / 4.0f);
}

std::string format_param(ParamId id, float v) {
  char b[32];
  auto time = [&](float seconds) {
    if (seconds < 1) std::snprintf(b, sizeof b, "%.0f ms", seconds * 1000);
    else std::snprintf(b, sizeof b, "%.2f s", seconds);
  };
  switch (id) {
    case kParamCompThreshold:
      if (v <= 0) return "Off";
      std::snprintf(b, sizeof b, "%.0f dB", comp_threshold_db(v));
      return b;
    case kParamCompRatio:
      std::snprintf(b, sizeof b, "%.1f:1", comp_ratio(v));
      return b;
    case kParamCompRelease:
      time(comp_release_seconds(v));
      return b;
    case kParamSamplerMode: return v < 0.5f ? "Single" : v < 1.5f ? "Slices" : v < 2.5f ? "Kit" : "Multi";
    case kParamSliceMode: return v > 0.5f ? "At transients" : "Equal";
    case kParamLoopMode: return v < 0.5f ? "Off" : v < 1.5f ? "Forward" : "Ping-pong";
    case kParamReverse: case kParamLimiter: return v > 0.5f ? "On" : "Off";
    case kParamFilterType: return v < 0.5f ? "Off" : v < 1.5f ? "Low-pass" : v < 2.5f ? "Band-pass" : "High-pass";
    case kParamAmpMode: return v < 0.5f ? "Low-pass gate" : "ADSR";
    case kParamRootNote: {
      int n = int(v + 0.5f);
      return std::string(kNoteNames[n % 12]) + std::to_string(n / 12 - 1);
    }
    case kParamSlices: case kParamEngine: case kParamMidiCcA: case kParamMidiCcB:
      return std::to_string(int(v + 0.5f));
    case kParamFineTune: std::snprintf(b, sizeof b, "%+.0f cents", v * 100); return b;
    case kParamCutoff: {
      float hz = cutoff_hz(v);
      if (hz < 1000) std::snprintf(b, sizeof b, "%.0f Hz", hz);
      else std::snprintf(b, sizeof b, "%.1f kHz", hz / 1000);
      return b;
    }
    case kParamAttack: case kParamDecayTime: case kParamRelease: time(env_seconds(v)); return b;
    case kParamPan: std::snprintf(b, sizeof b, "%+.0f", v * 100); return b;
    case kParamEqLow: case kParamEqMid: case kParamEqHigh:
      std::snprintf(b, sizeof b, "%+.1f dB", v * 12); return b;
    case kParamDelayTime: {
      static const char* const names[] = {"1/16", "1/8 T", "1/8", "1/8 dotted", "1/4", "1/4 dotted", "1/2"};
      return names[std::max(0, std::min(6, int(v * 6 + 0.5f)))];
    }
    case kParamFmAmount: case kParamTimbreAmount: case kParamMorphAmount:
      std::snprintf(b, sizeof b, "%+.0f%%", v * 100); return b;
    default: std::snprintf(b, sizeof b, "%.0f%%", v * 100); return b;
  }
}

bool SamplerSettings::operator==(const SamplerSettings& o) const {
  if (mode != o.mode || sample != o.sample || root != o.root || fine != o.fine || start != o.start ||
      end != o.end || loop != o.loop || loop_start != o.loop_start || loop_end != o.loop_end ||
      reverse != o.reverse || slices != o.slices || slice_transients != o.slice_transients || preset != o.preset)
    return false;
  for (int i = 0; i < kKitSlots; ++i)
    if (!(kit[i] == o.kit[i])) return false;
  return true;
}

float SamplerSettings::get(ParamId id) const {
  switch (id) {
    case kParamSamplerMode: return float(mode);
    case kParamRootNote: return float(root);
    case kParamFineTune: return fine;
    case kParamSampleStart: return start;
    case kParamSampleEnd: return end;
    case kParamLoopMode: return float(loop);
    case kParamLoopStart: return loop_start;
    case kParamLoopEnd: return loop_end;
    case kParamReverse: return reverse ? 1.0f : 0.0f;
    case kParamSlices: return float(slices);
    case kParamSliceMode: return slice_transients ? 1.0f : 0.0f;
    default: return 0;
  }
}

void SamplerSettings::set(ParamId id, float v) {
  if (const ParamInfo* info = param_info(id)) v = std::min(info->max, std::max(info->min, v));
  switch (id) {
    case kParamSamplerMode: mode = SamplerMode(int(v + 0.5f)); break;
    case kParamRootNote: root = int(v + 0.5f); break;
    case kParamFineTune: fine = v; break;
    case kParamSampleStart: start = std::min(v, end); break;
    case kParamSampleEnd: end = std::max(v, start); break;
    case kParamLoopMode: loop = LoopMode(int(v + 0.5f)); break;
    case kParamLoopStart: loop_start = std::min(v, loop_end); break;
    case kParamLoopEnd: loop_end = std::max(v, loop_start); break;
    case kParamReverse: reverse = v > 0.5f; break;
    case kParamSlices: slices = int(v + 0.5f); break;
    case kParamSliceMode: slice_transients = v > 0.5f; break;
    default: break;
  }
}

bool ToneSettings::operator==(const ToneSettings& o) const {
  return filter == o.filter && cutoff == o.cutoff && resonance == o.resonance && drive == o.drive &&
         attack == o.attack && decay == o.decay && sustain == o.sustain && release == o.release &&
         amp_mode == o.amp_mode;
}

float ToneSettings::get(ParamId id) const {
  switch (id) {
    case kParamFilterType: return float(filter);
    case kParamCutoff: return cutoff;
    case kParamResonance: return resonance;
    case kParamDrive: return drive;
    case kParamAttack: return attack;
    case kParamDecayTime: return decay;
    case kParamSustain: return sustain;
    case kParamRelease: return release;
    case kParamAmpMode: return float(amp_mode);
    default: return 0;
  }
}

void ToneSettings::set(ParamId id, float v) {
  if (const ParamInfo* info = param_info(id)) v = std::min(info->max, std::max(info->min, v));
  switch (id) {
    case kParamFilterType: filter = FilterType(int(v + 0.5f)); break;
    case kParamCutoff: cutoff = v; break;
    case kParamResonance: resonance = v; break;
    case kParamDrive: drive = v; break;
    case kParamAttack: attack = v; break;
    case kParamDecayTime: decay = v; break;
    case kParamSustain: sustain = v; break;
    case kParamRelease: release = v; break;
    case kParamAmpMode: amp_mode = int(v + 0.5f); break;
    default: break;
  }
}

const ParamInfo* param_info(ParamId id) {
  for (const auto* list : {&kMacroParams, &kSamplerParams, &kToneParams, &kMidiParams, &kMixerParams, &kMasterParams,
                           &kEffectParams})
    for (const ParamInfo& p : *list)
      if (p.id == id) return &p;
  return nullptr;
}

bool Note::operator==(const Note& o) const {
  if (id != o.id || start != o.start || length != o.length || pitch != o.pitch ||
      velocity != o.velocity || probability != o.probability || glide != o.glide ||
      num_locks != o.num_locks)
    return false;
  for (int i = 0; i < num_locks; ++i)
    if (!(locks[i] == o.locks[i])) return false;
  return true;
}

const ParamLock* Note::lock(ParamId pid) const {
  for (int i = 0; i < num_locks; ++i)
    if (locks[i].id == pid) return &locks[i];
  return nullptr;
}

bool Note::set_lock(ParamId pid, float value) {
  for (int i = 0; i < num_locks; ++i)
    if (locks[i].id == pid) {
      locks[i].value = value;
      return true;
    }
  if (num_locks == kMaxLocksPerNote) return false;
  locks[num_locks++] = ParamLock{pid, value};
  return true;
}

void Note::clear_lock(ParamId pid) {
  for (int i = 0; i < num_locks; ++i)
    if (locks[i].id == pid) {
      locks[i] = locks[--num_locks];
      locks[num_locks] = ParamLock{};
      return;
    }
}

bool Clip::operator==(const Clip& o) const {
  return id == o.id && track == o.track && name == o.name && length == o.length &&
         notes == o.notes && lanes == o.lanes && next_note_id == o.next_note_id;
}

Lane* Clip::lane(ParamId p) {
  for (Lane& l : lanes)
    if (l.param == p) return &l;
  return nullptr;
}

const Lane* Clip::lane(ParamId p) const { return const_cast<Clip*>(this)->lane(p); }

float Lane::value_at(Tick t) const {
  if (points.empty()) return 0;
  if (t <= points.front().tick) return points.front().value;
  if (t >= points.back().tick) return points.back().value;
  auto it = std::upper_bound(points.begin(), points.end(), t,
                             [](Tick x, const LanePoint& p) { return x < p.tick; });
  const LanePoint& b = *it;
  const LanePoint& a = *(it - 1);
  return a.value + (b.value - a.value) * float(t - a.tick) / float(b.tick - a.tick);
}

void Lane::set_point(Tick t, float value) {
  auto it = std::lower_bound(points.begin(), points.end(), t,
                             [](const LanePoint& p, Tick x) { return p.tick < x; });
  if (it != points.end() && it->tick == t) it->value = value;
  else points.insert(it, LanePoint{t, value});
}

void Lane::remove_point(Tick t) {
  points.erase(std::remove_if(points.begin(), points.end(), [t](const LanePoint& p) { return p.tick == t; }),
               points.end());
}

const std::vector<ParamId>& mix_automatable_params() {
  static const std::vector<ParamId> v = {kParamReverbSize, kParamReverbTone, kParamDelayTime, kParamDelayFeedback,
                                         kParamChorusRate, kParamChorusDepth, kParamEqLow, kParamEqMid,
                                         kParamEqHigh, kParamMasterLevel};
  return v;
}

std::string cc_label(int cc) {
  const char* use = nullptr;
  switch (cc) {
    case 1: use = "Mod wheel"; break;
    case 2: use = "Breath"; break;
    case 7: use = "Volume"; break;
    case 10: use = "Pan"; break;
    case 11: use = "Expression"; break;
    case 64: use = "Sustain"; break;
    case 71: use = "Resonance"; break;
    case 72: use = "Release"; break;
    case 73: use = "Attack"; break;
    case 74: use = "Cutoff"; break;
    default: break;
  }
  return use ? std::to_string(cc) + " (" + use + ")" : std::to_string(cc);
}

namespace {

// The macro oscillator's chords (its chord bank, in order), which the chord engine (14) and the
// chiptune arpeggio (7) read from Harmonics at 1.02 times the setting, and the arpeggio's patterns,
// read from Timbre: four directions, each over one, two or four octaves.
constexpr int kNumChords = 11, kNumArpPatterns = 12;
const char* const kChordNames[kNumChords] = {"Octave", "Fifth", "Sus4", "Minor", "Minor 7th", "Minor 9th",
                                             "Minor 11th", "6/9", "Major 9th", "Major 7th", "Major"};
const char* const kArpPatternNames[kNumArpPatterns] = {
    "Up, 1 octave",       "Up, 2 octaves",     "Up, 4 octaves",     "Down, 1 octave",
    "Down, 2 octaves",    "Down, 4 octaves",   "Up-down, 1 octave", "Up-down, 2 octaves",
    "Up-down, 4 octaves", "Random, 1 octave",  "Random, 2 octaves", "Random, 4 octaves"};

// The steps a track's engine reads a setting in: their names, how many, and the scale it applies.
// Without names (the six-op FM engines' patches, whose names are in the bank), they're numbered.
struct Steps {
  const char* const* names = nullptr;
  int count = 0;
  float scale = 1;
  int index(float v) const { return std::max(0, std::min(count - 1, int(v * scale * count))); }
};
Steps steps_of(const Track& t, ParamId id) {
  if (t.instrument != InstrumentType::Macro) return {};
  if (id == kParamHarmonics && (t.macro.engine == 7 || t.macro.engine == 14)) return {kChordNames, kNumChords, 1.02f};
  // The six-op FM engines: Harmonics picks one of the bank's 32 patches, at 1.02 times the setting.
  if (id == kParamHarmonics && t.macro.engine >= 2 && t.macro.engine <= 4) return {nullptr, 32, 1.02f};
  if (id == kParamTimbre && t.macro.engine == 7) return {kArpPatternNames, kNumArpPatterns, 1};
  return {};
}

}  // namespace

std::string format_track_param(const Track& t, ParamId id, float v) {
  const Steps s = steps_of(t, id);
  if (s.count && !s.names) return "Patch " + std::to_string(s.index(v) + 1);
  return s.count ? std::string(s.names[s.index(v)]) : format_param(id, v);
}

int fm_patch_index(const Track& t) {
  const Steps s = steps_of(t, kParamHarmonics);
  return s.count && !s.names ? s.index(t.macro.harmonics) : -1;
}

float step_track_param(const Track& t, ParamId id, float v, int step, bool coarse, float fine) {
  const ParamInfo* info = param_info(id);
  if (!info) return v;
  const Steps s = steps_of(t, id);
  if (s.count) {
    const int i = std::max(0, std::min(s.count - 1, s.index(v) + step * (coarse ? 3 : 1)));
    v = (i + 0.5f) / s.count / s.scale;  // the middle of its step
  } else if (info->integer) {
    v += float(step * (coarse ? (id == kParamRootNote ? 12 : 4) : 1));
  } else {
    v += step * (info->max - info->min) * (coarse ? fine * 10 : fine);
  }
  return std::max(info->min, std::min(info->max, v));
}

std::string param_name(const Track& t, ParamId id) {
  if (id == kParamMidiCcA || id == kParamMidiCcB) return "CC " + std::to_string(t.midi_cc[id - kParamMidiCcA]);
  const ParamInfo* info = param_info(id);
  return info ? info->name : "?";
}

std::vector<ParamId> automatable_params(InstrumentType t) {
  std::vector<ParamId> v;
#if TWILL_PLUGINS
  if (t == InstrumentType::Plugin) {  // the track's mixer, and the mix
    v = {kParamLevel, kParamPan, kParamSendChorus, kParamSendDelay, kParamSendReverb};
    for (ParamId id : mix_automatable_params()) v.push_back(id);
    return v;
  }
#endif
  if (t == InstrumentType::MidiOut) {
    v = {kParamMidiCcA, kParamMidiCcB};
    for (ParamId id : mix_automatable_params()) v.push_back(id);
    return v;
  }
  if (t == InstrumentType::Macro)
    for (ParamId id : {kParamHarmonics, kParamTimbre, kParamMorph, kParamDecay, kParamLpgColour}) v.push_back(id);
  else
    for (ParamId id : {kParamFineTune, kParamSampleStart}) v.push_back(id);
  for (ParamId id : {kParamCutoff, kParamResonance, kParamDrive, kParamLevel, kParamPan, kParamSendChorus,
                     kParamSendDelay, kParamSendReverb})
    v.push_back(id);
  for (ParamId id : mix_automatable_params()) v.push_back(id);
  return v;
}

Note* Clip::find(uint32_t note_id) {
  for (Note& n : notes)
    if (n.id == note_id) return &n;
  return nullptr;
}

const Note* Clip::find(uint32_t note_id) const { return const_cast<Clip*>(this)->find(note_id); }

void Clip::sort() {
  std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
}

bool MacroSettings::operator==(const MacroSettings& o) const {
  return engine == o.engine && harmonics == o.harmonics && timbre == o.timbre && morph == o.morph &&
         decay == o.decay && lpg_colour == o.lpg_colour && fm_amount == o.fm_amount &&
         timbre_amount == o.timbre_amount && morph_amount == o.morph_amount && output == o.output &&
         arp_rate == o.arp_rate;
}

float MacroSettings::get(ParamId id) const {
  switch (id) {
    case kParamEngine: return float(engine);
    case kParamHarmonics: return harmonics;
    case kParamTimbre: return timbre;
    case kParamMorph: return morph;
    case kParamDecay: return decay;
    case kParamLpgColour: return lpg_colour;
    case kParamFmAmount: return fm_amount;
    case kParamTimbreAmount: return timbre_amount;
    case kParamMorphAmount: return morph_amount;
    default: return 0;
  }
}

void MacroSettings::set(ParamId id, float v) {
  if (const ParamInfo* info = param_info(id)) v = std::min(info->max, std::max(info->min, v));
  switch (id) {
    case kParamEngine: engine = int(v + 0.5f); break;
    case kParamHarmonics: harmonics = v; break;
    case kParamTimbre: timbre = v; break;
    case kParamMorph: morph = v; break;
    case kParamDecay: decay = v; break;
    case kParamLpgColour: lpg_colour = v; break;
    case kParamFmAmount: fm_amount = v; break;
    case kParamTimbreAmount: timbre_amount = v; break;
    case kParamMorphAmount: morph_amount = v; break;
    default: break;
  }
}

bool Track::operator==(const Track& o) const {
  return name == o.name && colour == o.colour && instrument == o.instrument && voices == o.voices &&
         macro == o.macro && sampler == o.sampler && tone == o.tone && level == o.level && pan == o.pan &&
         send_chorus == o.send_chorus && send_delay == o.send_delay && send_reverb == o.send_reverb &&
         std::equal(std::begin(mods), std::end(mods), std::begin(o.mods)) && midi_channel == o.midi_channel &&
         midi_cc[0] == o.midi_cc[0] && midi_cc[1] == o.midi_cc[1] && fm_bank == o.fm_bank &&
#if TWILL_PLUGINS
         plugin == o.plugin &&
#endif
         mute == o.mute && solo == o.solo && arrangement == o.arrangement;
}

bool Project::operator==(const Project& o) const {
  return name == o.name && tempo == o.tempo && time_sig_num == o.time_sig_num &&
         time_sig_den == o.time_sig_den && loop == o.loop && loop_start == o.loop_start &&
         loop_end == o.loop_end && key == o.key && scale == o.scale && mix == o.mix && midi_clock == o.midi_clock && tracks == o.tracks &&
         clips == o.clips && next_clip_id == o.next_clip_id;
}

Clip* Project::find_clip(uint32_t id) {
  for (Clip& c : clips)
    if (c.id == id) return &c;
  return nullptr;
}

const Clip* Project::find_clip(uint32_t id) const { return const_cast<Project*>(this)->find_clip(id); }

Clip& Project::add_clip(int track, Tick length) {
  Clip c;
  c.id = next_clip_id++;
  c.track = track;
  c.length = length;
  clips.push_back(c);
  return clips.back();
}

Tick Project::song_end() const {
  Tick end = 0;
  for (const Track& t : tracks)
    for (const Placement& pl : t.arrangement) end = std::max(end, pl.start + pl.length);
  return end;
}

float Project::track_param(int t, ParamId id) const {
  if (id >= 300) return mix.get(id);  // the master and effects: one setting for the whole project
  const Track& tr = tracks[size_t(t)];
  switch (id) {
    case kParamLevel: return tr.level;
    case kParamPan: return tr.pan;
    case kParamSendChorus: return tr.send_chorus;
    case kParamSendDelay: return tr.send_delay;
    case kParamSendReverb: return tr.send_reverb;
    case kParamMidiCcA: case kParamMidiCcB: return 64;  // a lane's starting point; tracks keep no value
    default: break;
  }
  if (id >= 40 && id < 60) return tr.tone.get(id);
  if (id >= 20 && id < 40) return tr.sampler.get(id);
  return tr.macro.get(id);
}

void Project::set_track_param(int t, ParamId id, float v) {
  Track& tr = tracks[size_t(t)];
  if (const ParamInfo* info = param_info(id)) v = std::min(info->max, std::max(info->min, v));
  switch (id) {
    case kParamLevel: tr.level = v; return;
    case kParamPan: tr.pan = v; return;
    case kParamSendChorus: tr.send_chorus = v; return;
    case kParamSendDelay: tr.send_delay = v; return;
    case kParamSendReverb: tr.send_reverb = v; return;
    case kParamMidiCcA: case kParamMidiCcB: return;
    default: break;
  }
  if (id >= 40 && id < 60) tr.tone.set(id, v);
  else if (id >= 20 && id < 40) tr.sampler.set(id, v);
  else tr.macro.set(id, v);
}

}  // namespace twill
