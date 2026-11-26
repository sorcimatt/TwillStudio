#include "engine/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace twill {

namespace {

constexpr float kGlideSeconds = 0.06f;
constexpr float kQuietLevel = 1e-4f;
constexpr int kQuietBlocksToFree = Engine::kSampleRate / 10 / Engine::kSubBlock;  // 100 ms of silence
constexpr int kMaxRingSamples = Engine::kSampleRate * 10;  // a released voice is freed after 10 s
constexpr float kChokeSeconds = 0.005f;
constexpr float kAudibleTail = 0.03f;  // -30 dB

// A track's settings with a note's parameter locks on top.
struct Settings {
  MacroSettings macro;
  SnapSampler sampler;
  ToneSettings tone;
};
Settings locked(const SnapTrack& t, const Note& n, const LaneValue* lanes = nullptr, int lane_count = 0) {
  Settings s{t.macro, t.sampler, t.tone};
  // Automation first, then the note's own locks.
  for (int i = 0; i < lane_count; ++i) {
    const ParamId id = lanes[i].id;
    if (id >= 40 && id < 60) s.tone.set(id, lanes[i].value);
    else if (id >= 20 && id < 40) s.sampler.set(id, lanes[i].value);
    else if (id < 20) s.macro.set(id, lanes[i].value);
  }
  for (int i = 0; i < n.num_locks; ++i) {
    const ParamId id = n.locks[i].id;
    const float v = n.locks[i].value;
    if (id >= 40 && id < 60) s.tone.set(id, v);
    else if (id >= 20 && id < 40) s.sampler.set(id, v);
    else if (id < 20) s.macro.set(id, v);
  }
  return s;
}

float get_param(const Settings& s, ParamId id) {
  if (id >= 40 && id < 60) return s.tone.get(id);
  if (id >= 20 && id < 40) {
    switch (id) {  // SnapSampler has no getter; these are the modulatable ones
      case kParamSampleStart: return s.sampler.start;
      case kParamFineTune: return s.sampler.fine;
      default: return 0;
    }
  }
  return s.macro.get(id);
}

void set_param(Settings& s, ParamId id, float v) {
  if (id >= 40 && id < 60) s.tone.set(id, v);
  else if (id >= 20 && id < 40) s.sampler.set(id, v);
  else s.macro.set(id, v);
}

MacroParams macro_params_for(const MacroSettings& s, const ToneSettings& tone, float note) {
  MacroParams p;
  p.engine = s.engine;
  p.harmonics = s.harmonics;
  p.timbre = s.timbre;
  p.morph = s.morph;
  p.decay = s.decay;
  p.lpg_colour = s.lpg_colour;
  p.fm_amount = s.fm_amount;
  p.timbre_amount = s.timbre_amount;
  p.morph_amount = s.morph_amount;
  p.use_lpg = tone.amp_mode == 0;
  p.note = note;
  return p;
}

}  // namespace

Engine::Engine(int max_voices, int max_synth_voices) : effects_(std::make_unique<Effects>()) {
  max_synth_voices_ = max_synth_voices > 0 && max_synth_voices < max_voices ? max_synth_voices : 0;
  reset_midi_cc();
  voices_.reserve(size_t(std::max(1, max_voices)));
  for (int i = 0; i < std::max(1, max_voices); ++i) {
    voices_.push_back(std::make_unique<Voice>());
    voices_.back()->tone.reset();
  }
  voice_zones_.reset(new std::atomic<int32_t>[voices_.size()]);
  for (size_t i = 0; i < voices_.size(); ++i) voice_zones_[i].store(-1);
  // The DSP's noise comes from one generator shared by every voice; start it the same way every
  // time, so a render is repeatable.
  PlaitsVoice::seed_random(0x21);
}

Engine::~Engine() {
  Snapshot* s;
  while (incoming_.pop(s)) delete s;
  collect_garbage();
  for (Snapshot* p : pending_free_) delete p;
  delete snap_;
}

void Engine::sounding_zones(int track, std::vector<int>& out) const {
  out.clear();
  for (size_t i = 0; i < voices_.size(); ++i) {
    const int32_t v = voice_zones_[i].load(std::memory_order_relaxed);
    if (v >= 0 && (v >> 20) == track) out.push_back(v & 0xFFFFF);
  }
}

void Engine::submit(std::unique_ptr<Snapshot> snapshot) {
  collect_garbage();
  Snapshot* s = snapshot.release();
  if (!incoming_.push(s)) delete s;  // the audio thread is far behind; drop this one
}

void Engine::collect_garbage() {
  Snapshot* s;
  while (outgoing_.pop(s)) delete s;
}

void Engine::play() { commands_.push(Command{Command::Play, 0, 0, 0, nullptr}); }
void Engine::stop() { commands_.push(Command{Command::Stop, 0, 0, 0, nullptr}); }
void Engine::set_tempo_override(double bpm) { commands_.push(Command{Command::Tempo, 0, 0, bpm, nullptr}); }
void Engine::locate(Tick t) { commands_.push(Command{Command::Locate, 0, 0, double(t), nullptr}); }
void Engine::audition(int track, uint8_t pitch, float seconds) {
  commands_.push(Command{Command::Audition, track, pitch, double(seconds), nullptr});
}
void Engine::live_note(int track, uint8_t pitch, uint8_t velocity) {
  commands_.push(Command{velocity ? Command::LiveOn : Command::LiveOff, track, pitch, double(velocity), nullptr});
}

void Engine::midi_note(const SnapTrack& t, uint8_t pitch, uint8_t velocity, int64_t gate) {
  const uint8_t ch = uint8_t(t.midi_channel & 15);
  if (gate <= 0) {  // live: out at once; the note off comes from the keyboard
    midi_out_.push(MidiEvent{MidiEvent::Now, clock_, 0, MidiMessage{uint8_t(0x90 | ch), pitch, velocity}});
    return;
  }
  midi_out(MidiMessage{uint8_t(0x90 | ch), pitch, velocity}, clock_);
  for (PendingOff& p : pending_offs_)
    if (p.samples <= 0) {
      p = PendingOff{uint8_t(0x80 | ch), pitch, gate};
      return;
    }
  midi_out(MidiMessage{uint8_t(0x80 | ch), pitch, 0}, clock_);  // no room to wait: end it now
}

void Engine::send_cc(int track, int which, int value) {
  const SnapTrack& t = snap_->tracks[size_t(track)];
  value = std::max(0, std::min(127, value));
  if (track < kMaxTracks) {
    if (cc_sent_[track][which] == value) return;
    cc_sent_[track][which] = int16_t(value);
  }
  midi_out(MidiMessage{uint8_t(0xB0 | (t.midi_channel & 15)), uint8_t(t.midi_cc[which] & 127), uint8_t(value)}, clock_);
}

void Engine::update_midi_cc() {
  for (int ti = 0; ti < int(snap_->tracks.size()) && ti < kMaxTracks; ++ti) {
    if (snap_->tracks[size_t(ti)].instrument != InstrumentType::MidiOut) continue;
    for (int li = 0; li < lane_counts_[ti]; ++li) {
      const LaneValue& lv = lane_values_[ti][li];
      if (lv.id != kParamMidiCcA && lv.id != kParamMidiCcB) continue;
      const int which = lv.id - kParamMidiCcA;
      if (clock_ < cc_held_until_[ti][which]) continue;
      send_cc(ti, which, int(std::lround(lv.value)));
    }
  }
}

void Engine::reset_midi_cc() {
  for (auto& t : cc_sent_) t[0] = t[1] = -1;
  for (auto& t : cc_held_until_) t[0] = t[1] = 0;
}

void Engine::flush_midi_notes() {
  for (PendingOff& p : pending_offs_)
    if (p.samples > 0) {
      midi_out(MidiMessage{p.status, p.pitch, 0}, clock_);
      p.samples = 0;
    }
}

void Engine::preview(const SampleData* sample) {
  commands_.push(Command{Command::Preview, 0, 0, 0, sample});
}

void Engine::apply_command(const Command& c) {
  switch (c.kind) {
    case Command::Play:
      reset_midi_cc();  // lanes send their values again from here
      if (!playing_.load() && snap_ && snap_->midi_clock_out) {
        midi_out(MidiMessage{0xFA, 0, 0}, clock_);  // start; the first clock goes with it
        next_midi_clock_ = double(clock_);
      }
      if (!playing_.load()) {
        elapsed_ = 0;
        // Free-running LFOs start from the top with playback, so a song plays the same each time.
        for (auto& t : track_lfo_phase_) std::fill(std::begin(t), std::end(t), 0.0f);
        for (auto& t : track_lfo_random_)
          for (float& r : t) {
            rng_ = rng_ * 1664525u + 1013904223u;
            r = (rng_ >> 8) / float(1 << 24) * 2 - 1;
          }
      }
      playing_.store(true);
      anchor_tick_ = tick_;
      anchor_clock_ = clock_;
      break;
    case Command::Stop:
      if (playing_.load() && snap_ && snap_->midi_clock_out) midi_out(MidiMessage{0xFC, 0, 0}, clock_);
      playing_.store(false);
      release_all();
      flush_midi_notes();
#if TWILL_PLUGINS
      plugins_all_off();
#endif
      break;
    case Command::Locate:
      reset_midi_cc();
      tick_ = anchor_tick_ = c.value;
      anchor_clock_ = clock_;
      position_.store(Tick(tick_));
  elapsed_ticks_.store(elapsed_, std::memory_order_relaxed);
      break;
    case Command::Audition:
      if (snap_ && c.track >= 0 && c.track < int(snap_->tracks.size())) {
        Note n;
        n.pitch = c.pitch;
        n.velocity = 102;  // 0.8 of full scale
        const SnapTrack& t = snap_->tracks[size_t(c.track)];
        const int64_t gate = std::max<int64_t>(kSubBlock, int64_t(c.value * kSampleRate));
#if TWILL_PLUGINS
        if (t.instrument == InstrumentType::Plugin) {
          plugin_note(c.track, c.pitch, uint8_t(n.velocity), gate);
          break;
        }
#endif
        if (t.instrument == InstrumentType::MidiOut) {  // heard on the MIDI device
          midi_note(t, c.pitch, uint8_t(n.velocity), gate);
          break;
        }
        if (start_multi(c.track, t, n, gate, false)) break;
        Voice* v = allocate(c.track, t, c.pitch);
        start_voice(*v, c.track, n, gate, false);
      }
      break;
    case Command::Tempo:
      tempo_override_ = c.value;
      anchor_tick_ = tick_;  // the new tempo takes effect from here
      anchor_clock_ = clock_;
      break;
    case Command::Preview:
      preview_ = c.sample;
      preview_pos_ = 0;
      break;
    case Command::LiveOn:
      if (snap_ && c.track >= 0 && c.track < int(snap_->tracks.size())) {
        const SnapTrack& t = snap_->tracks[size_t(c.track)];
#if TWILL_PLUGINS
        if (t.instrument == InstrumentType::Plugin) {
          plugin_note(c.track, c.pitch, uint8_t(c.value), 0);
          break;
        }
#endif
        if (t.instrument == InstrumentType::MidiOut) {
          midi_note(t, c.pitch, uint8_t(c.value), 0);
          break;
        }
        Note n;
        n.pitch = c.pitch;
        n.velocity = uint8_t(c.value);
        if (start_multi(c.track, t, n, INT64_MAX / 2, true)) break;
        Voice* v = allocate(c.track, t, c.pitch);
        start_voice(*v, c.track, n, INT64_MAX / 2, false);
        v->live = v->track == c.track;
      }
      break;
    case Command::LiveOff:
#if TWILL_PLUGINS
      if (snap_ && c.track >= 0 && c.track < int(snap_->tracks.size()) &&
          snap_->tracks[size_t(c.track)].instrument == InstrumentType::Plugin) {
        plugin_event(c.track, c.pitch, 0, clock_);
        break;
      }
#endif
      if (snap_ && c.track >= 0 && c.track < int(snap_->tracks.size()) &&
          snap_->tracks[size_t(c.track)].instrument == InstrumentType::MidiOut) {
        midi_out_.push(MidiEvent{MidiEvent::Now, clock_, 0,
                                 MidiMessage{uint8_t(0x80 | (snap_->tracks[size_t(c.track)].midi_channel & 15)), c.pitch, 0}});
        break;
      }
      for (auto& v : voices_)
        if (v->live && v->track == c.track && v->note.pitch == c.pitch && v->gate > 0) {
          v->live = false;
          if (!v->one_shot) release(*v);
        }
      break;
  }
}

Engine::Voice* Engine::allocate(int track, const SnapTrack& t, uint8_t pitch) {
  const bool kit = t.instrument == InstrumentType::Sampler && t.sampler.mode == SamplerMode::Kit;
  auto same = [&](const Voice& v) { return v.track == track && (!kit || v.note.pitch == pitch); };
  // Mono tracks keep one voice, like a mono synth (in a kit: one per drum).
  if (t.voices == 1)
    for (auto& v : voices_)
      if (same(*v)) return v.get();
  // At the track's polyphony: take its oldest held note.
  int held = 0;
  Voice* oldest_held = nullptr;
  for (auto& v : voices_)
    if (same(*v) && v->gate > 0) {
      ++held;
      if (!oldest_held || v->started < oldest_held->started) oldest_held = v.get();
    }
  if (held >= t.voices && oldest_held) return oldest_held;
  return any_voice(t.instrument == InstrumentType::Macro);
}

Engine::Voice* Engine::any_voice(bool synth) {
  // With the synth voices all in use, a macro note takes one of them, so the macro oscillators
  // never cost more than their limit however many samples play.
  int synths = 0;
  if (synth && max_synth_voices_)
    for (auto& v : voices_) synths += v->track >= 0 && v->instrument == InstrumentType::Macro;
  const bool synth_only = synth && max_synth_voices_ && synths >= max_synth_voices_;
  // A free voice, else the oldest released one anywhere, else the oldest of all.
  Voice* best = nullptr;
  for (auto& v : voices_) {
    if (synth_only && (v->track < 0 || v->instrument != InstrumentType::Macro)) continue;
    if (v->track < 0) return v.get();
    bool better = !best || (v->gate == 0 && best->gate > 0) ||
                  ((v->gate == 0) == (best->gate == 0) && v->started < best->started);
    if (better) best = v.get();
  }
  // Out of voices: count it if what's cut off is a held note, or a tail still loud enough to
  // notice going (above -30 dB), so the app can say so. Quieter tails vanish unnoticed in a mix.
  if (best && (best->gate > 0 || best->last_peak * best->gain > kAudibleTail))
    voices_stolen_.store(voices_stolen_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  return best;
}

bool Engine::start_sampler(Voice& v, const SnapTrack& t, const Note& n, int zone) {
  v.xfade = 0;  // set for a multisample zone below
  const Settings s = locked(t, n);
  const SnapSampler& sm = s.sampler;
  v.pan = 0;
  v.choke = 0;
  v.fade = 1;
  v.fade_per_block = 1;
  v.done = false;
  v.dir = 1;
  const SampleData* smp = nullptr;
  double semitones = 0;
  float a = sm.start, b = sm.end;
  LoopMode loop = sm.loop;
  float la = sm.loop_start, lb = sm.loop_end;
  bool reverse = sm.reverse;
  int frames_a = -1, frames_b = -1, frames_la = -1, frames_lb = -1;  // multi: in frames, not fractions

  switch (sm.mode) {
    case SamplerMode::Multi: {
      if (zone < 0 || zone >= int(t.zones.size())) return false;
      const SnapZone& z = t.zones[size_t(zone)];
      smp = z.sample;
      semitones = z.tune_after_keytrack ? (n.pitch - z.root) * z.keytrack + z.tune
                                        : (n.pitch - z.root + z.tune) * z.keytrack;
      semitones += sm.fine;
      v.gain = (1 - z.veltrack + z.veltrack * n.velocity / 127.0f) * z.gain;
      v.pan = z.pan;
      frames_a = z.begin, frames_b = z.end, frames_la = z.loop_a, frames_lb = z.loop_b;
      v.xfade = z.loop ? z.xfade : 0;
      v.xfade_linear = z.xfade_linear;
      loop = z.loop ? LoopMode::Forward : LoopMode::Off;
      reverse = false;
      break;
    }
    case SamplerMode::Single:
      smp = t.sample;
      semitones = n.pitch - sm.root + sm.fine;
      break;
    case SamplerMode::Slice: {
      smp = t.sample;
      const int i = n.pitch - sm.root;
      if (sm.slice_transients && t.sample) {
        const std::vector<int>& pts = t.slice_points;
        if (i < 0 || i + 1 >= int(pts.size())) return false;
        a = float(pts[size_t(i)]) / t.sample->frames;
        b = float(pts[size_t(i) + 1]) / t.sample->frames;
      } else {
        if (i < 0 || i >= sm.slices) return false;
        const float w = (b - a) / sm.slices;
        a = sm.start + w * i;
        b = a + w;
      }
      la = a, lb = b;  // a loop, if on, stays inside the slice
      semitones = sm.fine;
      break;
    }
    case SamplerMode::Kit: {
      const int i = n.pitch - kKitFirstNote;
      if (i < 0 || i >= kKitSlots) return false;
      smp = t.kit[i];
      semitones = sm.kit[i].tune + sm.fine;
      v.gain *= sm.kit[i].level;
      v.pan = sm.kit[i].pan;
      v.choke = sm.kit[i].choke;
      if (const float secs = kit_decay_seconds(sm.kit[i].decay); secs > 0)
        v.fade_per_block = std::pow(0.001f, float(kSubBlock) / (secs * float(kSampleRate)));  // -60 dB in secs
      a = 0, b = 1, la = 0, lb = 1;
      loop = LoopMode::Off;
      reverse = false;
      break;
    }
  }
  if (!smp || smp->frames < 2) return false;
  const int frames = smp->frames;
  v.sample = smp;
  v.begin = std::max(0, std::min(frames - 1, int(a * frames)));
  v.end = std::max(v.begin + 1, std::min(frames, int(b * frames)));
  v.loop_a = std::max(v.begin, std::min(v.end - 1, int(la * frames)));
  v.loop_b = std::max(v.loop_a + 1, std::min(v.end, int(lb * frames)));
  if (frames_a >= 0) {
    v.begin = std::max(0, std::min(frames - 1, frames_a));
    v.end = std::max(v.begin + 1, std::min(frames, frames_b));
    v.loop_a = std::max(v.begin, std::min(v.end - 1, frames_la));
    v.loop_b = std::max(v.loop_a + 1, std::min(v.end, frames_lb));
  }
  v.loop = loop;
  v.inc = v.base_inc = std::pow(2.0, semitones / 12.0);
  if (reverse) {
    v.pos = v.end - 1;
    v.dir = -1;
  } else {
    v.pos = v.begin;
  }

  // Choke: other notes on this track in the same group stop quickly.
  if (v.choke > 0)
    for (auto& o : voices_)
      if (o.get() != &v && o->track == v.track && o->choke == v.choke) o->env.kill(kChokeSeconds);
  return true;
}

void Engine::start_voice(Voice& v, int track, const Note& n, int64_t gate, bool legato, int zone) {
  const SnapTrack& t = snap_->tracks[size_t(track)];
  v.track = track;
  v.instrument = t.instrument;
  v.note = n;
  v.gain = n.velocity / 127.0f;
  v.gate = gate;
  v.quiet = 0;
  v.block_peak = 0;
  v.last_peak = 0;
  v.started = clock_;  // clock_ is the start of the current sub-block
  v.live = false;
  v.one_shot = false;
  v.zone = zone;
  if (!legato) {  // a legato note carries its modulators on, as it does the amp envelope
    v.mod_started = clock_;
    for (int m = 0; m < kModulators; ++m) {
      v.mod_phase[m] = 0;
      rng_ = rng_ * 1664525u + 1013904223u;
      v.mod_random[m] = (rng_ >> 8) / float(1 << 24) * 2 - 1;
      v.mod_out[m] = 0;
    }
  }
  if (!legato) update_modulators(v, t);  // a legato note's are already running
  const Settings s = locked(t, n);
  if (t.instrument == InstrumentType::Sampler) {
    v.tone.reset();
    v.env.reset();
    if (!start_sampler(v, t, n, zone)) {
      v.track = -1;  // nothing to play (no sample, or the note has no slice or kit slot)
      return;
    }
    // Kit drums are one-shots: the note's end doesn't release them. They stop at the end of the
    // sample, or early when choked, stolen (a drum hit again at one voice per drum) or stopped.
    if (t.sampler.mode == SamplerMode::Kit ||
        (t.sampler.mode == SamplerMode::Multi && zone >= 0 && zone < int(t.zones.size()) && t.zones[size_t(zone)].one_shot)) {
      v.one_shot = true;
      v.gate = INT64_MAX / 2;
    }
    v.env.gate_on();
    return;
  }
  // Macro oscillator.
  const float note = float(n.pitch);
  if (legato) {
    v.pitch_target = note;  // a gliding note slews from the current pitch; others go straight there
    if (!n.glide) v.glide_note = note;
    v.synth.legato_on();    // reopens the gate without a new edge if the last note just ended
    v.env.resume();
  } else {
    v.params = macro_params_for(s.macro, s.tone, note);
    v.glide_note = v.pitch_target = note;
    v.synth.note_on();
    v.env.retrigger();
  }
}

bool Engine::start_multi(int track, const SnapTrack& t, const Note& n, int64_t gate, bool live) {
  if (t.instrument != InstrumentType::Sampler || t.sampler.mode != SamplerMode::Multi) return false;
  // The zones this note plays: those holding its key and velocity, of each round robin only its
  // turn, and of each random group one.
  constexpr int kMaxLayers = 16;
  int play[kMaxLayers], count = 0;
  const int tr = std::min(track, kMaxTracks - 1);
  bool turned[kSeqGroups] = {};
  int candidates[kSeqGroups] = {}, pick[kSeqGroups];
  bool last_there[kSeqGroups] = {};
  // One random number for the note, for zones that play over a range of it.
  rng_ = rng_ * 1664525u + 1013904223u;
  const float chance = (rng_ >> 8) / float(1 << 24);
  auto matches = [&](const SnapZone& z) {
    return n.pitch >= z.lo_key && n.pitch <= z.hi_key && n.velocity >= z.lo_vel && n.velocity <= z.hi_vel &&
           chance >= z.lo_rand && (chance < z.hi_rand || z.hi_rand >= 1);
  };
  auto is_random = [](const SnapZone& z) { return z.seq == SeqMode::Random || z.seq == SeqMode::TrueRandom; };
  // Random groups: count the zones the note could play, then choose one (a Random group never the
  // one it played last, when there's another).
  for (int i = 0; i < int(t.zones.size()); ++i) {
    const SnapZone& z = t.zones[size_t(i)];
    if (!is_random(z) || !matches(z)) continue;
    const int g = z.seq_group % kSeqGroups;
    ++candidates[g];
    if (z.seq == SeqMode::Random && i == seq_last_[tr][g]) last_there[g] = true;
  }
  for (int g = 0; g < kSeqGroups; ++g) {
    pick[g] = -1;
    const bool skip_last = last_there[g] && candidates[g] > 1;
    if (candidates[g] == 0) continue;
    rng_ = rng_ * 1664525u + 1013904223u;
    pick[g] = int((rng_ >> 8) % uint32_t(candidates[g] - (skip_last ? 1 : 0)));  // the pick-th, counting on
  }
  for (int i = 0; i < int(t.zones.size()); ++i) {
    const SnapZone& z = t.zones[size_t(i)];
    if (!matches(z)) continue;
    const int g = z.seq_group % kSeqGroups;
    if (is_random(z)) {
      if (pick[g] < 0) continue;
      if (z.seq == SeqMode::Random && i == seq_last_[tr][g] && candidates[g] > 1) continue;
      if (pick[g]-- == 0) {
        if (count < kMaxLayers) play[count++] = i;
        seq_last_[tr][g] = i;
        pick[g] = -1;
      }
      continue;
    }
    if (z.seq == SeqMode::RoundRobin && z.seq_length > 0) {
      turned[g] = true;
      if (seq_turn_[tr][g] % z.seq_length + 1 != z.seq_position) continue;
    }
    if (count < kMaxLayers) play[count++] = i;
  }
  for (int g = 0; g < kSeqGroups; ++g)
    if (turned[g]) ++seq_turn_[tr][g];
  if (count == 0) return true;

  // Polyphony in notes: a mono track lets its last note go; at the limit, the oldest held note goes.
  int held = 0;
  uint64_t oldest = UINT64_MAX;
  int oldest_pitch = -1;
  for (size_t i = 0; i < voices_.size(); ++i) {
    const Voice* v = voices_[i].get();
    if (v->track == track && v->gate > 0) {
      // Count each note once, by its oldest layer (layers start together: the first of those).
      bool first = true;
      for (size_t j = 0; j < voices_.size() && first; ++j) {
        const Voice* o = voices_[j].get();
        if (j != i && o->track == track && o->gate > 0 && o->note.pitch == v->note.pitch &&
            (o->started < v->started || (o->started == v->started && j < i)))
          first = false;
      }
      if (!first) continue;
      ++held;
      if (v->started < oldest) oldest = v->started, oldest_pitch = v->note.pitch;
    }
  }
  if (held >= t.voices && oldest_pitch >= 0)
    for (auto& v : voices_)
      if (v->track == track && v->gate > 0 && (t.voices == 1 || v->note.pitch == oldest_pitch)) {
        v->env.kill(kChokeSeconds);
        v->gate = 0;
        v->live = false;
      }
  for (int k = 0; k < count; ++k) {
    Voice* v = any_voice(false);
    start_voice(*v, track, n, gate, false, play[k]);
    v->live = live && v->track == track;
  }
  return true;
}

void Engine::start_note(int track, const Note& n, double tps) {
  if (n.probability < 100) {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    if (int(rng_ % 100) >= n.probability) return;
  }
  const SnapTrack& t = snap_->tracks[size_t(track)];
#if TWILL_PLUGINS
  if (t.instrument == InstrumentType::Plugin) {
    plugin_note(track, n.pitch, n.velocity, std::max<int64_t>(kSubBlock, int64_t(n.length / tps)));
    if (observer_) observer_->note_on(track, n.pitch, clock_, MacroParams());
    return;
  }
#endif
  if (t.instrument == InstrumentType::MidiOut) {
    const int64_t gate = std::max<int64_t>(kSubBlock, int64_t(n.length / tps));
    // Locked controllers go out just before the note, and hold until it ends.
    for (int i = 0; i < n.num_locks; ++i) {
      const ParamId id = n.locks[i].id;
      if (id != kParamMidiCcA && id != kParamMidiCcB) continue;
      const int which = id - kParamMidiCcA;
      send_cc(track, which, int(std::lround(n.locks[i].value)));
      if (track < kMaxTracks) cc_held_until_[track][which] = clock_ + uint64_t(gate);
    }
    midi_note(t, n.pitch, n.velocity, gate);
    if (observer_) observer_->note_on(track, n.pitch, clock_, MacroParams());
    return;
  }
  if (start_multi(track, t, n, std::max<int64_t>(kSubBlock, int64_t(n.length / tps)), false)) {
    if (observer_) observer_->note_on(track, n.pitch, clock_, MacroParams());
    return;
  }
  Voice* v = allocate(track, t, n.pitch);
  // Legato (no retrigger) on a one-voice macro track, when this note overlaps the last or starts
  // where it ended (its gate closed within the last couple of grid blocks): for gliding notes, and
  // for every note on a legato track.
  const bool touching =
      v->track == track && !v->live && (v->gate > 0 || clock_ - v->released <= uint64_t(2 * kSubBlock));
  const bool legato =
      t.instrument == InstrumentType::Macro && t.voices == 1 && touching && (n.glide || t.legato);
  start_voice(*v, track, n, std::max<int64_t>(kSubBlock, int64_t(n.length / tps)), legato);
  if (observer_ && v->track == track) observer_->note_on(track, n.pitch, clock_, v->params);
}

void Engine::schedule(double t0, double t1) {
  const double tps = tempo() * kTicksPerQuarter / (60.0 * kSampleRate);
  for (int ti = 0; ti < int(snap_->tracks.size()); ++ti) {
    const SnapTrack& t = snap_->tracks[size_t(ti)];
    if (!t.audible) continue;
    for (const SnapPlacement& pl : t.placements) {
      if (pl.start >= t1) break;  // placements are sorted by start
      double a = std::max(t0, double(pl.start)), b = std::min(t1, double(pl.start + pl.length));
      if (a >= b) continue;
      // Into clip time, looping the clip.
      const double len = pl.clip->length;
      double la = std::fmod(a - pl.start + pl.offset, len), lb = la + (b - a);
      double segments[2][2] = {{la, std::min(lb, len)}, {0, lb - len}};
      for (auto& seg : segments) {
        if (seg[1] <= seg[0]) continue;
        const auto& notes = pl.clip->notes;
        auto it = std::lower_bound(notes.begin(), notes.end(), seg[0],
                                   [](const Note& n, double s) { return n.start < s; });
        for (; it != notes.end() && it->start < seg[1]; ++it) start_note(ti, *it, tps);
      }
    }
  }
}

void Engine::release(Voice& v) {
  v.gate = 0;
  v.released = clock_;
  if (v.instrument == InstrumentType::Macro) v.synth.note_off();
  v.env.gate_off();
}

void Engine::release_all() {
  for (auto& v : voices_)
    if (v->gate > 0) release(*v);
}

// A stretch of frames with every read inside the sample and no loop, end or crossfade in reach.
void Engine::render_sampler_run(Voice& v, float* l, float* r, int n) {
  const SampleData& s = *v.sample;
  const int16_t* pcm = s.pcm.data();
  constexpr float kScale = 1.0f / 32767.0f;
  double pos = v.pos;
  const double inc = v.inc;
  if (s.channels == 1) {
    for (int i = 0; i < n; ++i) {
      const int i0 = int(pos);
      const float t = float(pos - i0);
      const int16_t* p = pcm + (i0 - 1);
      l[i] = r[i] = hermite4(p[0] * kScale, p[1] * kScale, p[2] * kScale, p[3] * kScale, t);
      pos += inc;
    }
  } else {
    for (int i = 0; i < n; ++i) {
      const int i0 = int(pos);
      const float t = float(pos - i0);
      const int16_t* p = pcm + 2 * (i0 - 1);
      l[i] = hermite4(p[0] * kScale, p[2] * kScale, p[4] * kScale, p[6] * kScale, t);
      r[i] = hermite4(p[1] * kScale, p[3] * kScale, p[5] * kScale, p[7] * kScale, t);
      pos += inc;
    }
  }
  v.pos = pos;
}

void Engine::render_sampler(Voice& v, float* l, float* r, int n) {
  const SampleData& s = *v.sample;
  const int last = s.frames - 1;
  auto at = [&](int i, int ch) { return s.at(std::max(0, std::min(last, i)), ch); };
  const bool mono = s.channels == 1;
  for (int i = 0; i < n; ++i) {
    if (v.done) {
      l[i] = r[i] = 0;
      continue;
    }
    // Most of a sample is far from its ends, its loop points and its crossfade: there, a run of
    // frames needs none of the checks below. (Forwards only; the rest takes the careful way.)
    if (v.dir > 0 && v.pos >= 1 && v.pos >= v.begin) {
      double limit = v.loop == LoopMode::Off ? double(v.end) : double(v.loop_b);
      if (v.xfade > 0 && v.loop == LoopMode::Forward) limit = std::min(limit, double(v.loop_b - v.xfade));
      limit = std::min(limit, double(last - 2));
      // Frames whose read and whose step both stay short of the limit.
      const double room = (limit - v.pos) / v.inc - 1;
      const int k = room > 0 ? int(std::min(room, double(n - i))) : 0;
      if (k > 0) {
        render_sampler_run(v, l + i, r + i, k);
        i += k - 1;
        continue;
      }
    }
    const int i0 = int(std::floor(v.pos));
    const float t = float(v.pos - i0);
    l[i] = hermite4(at(i0 - 1, 0), at(i0, 0), at(i0 + 1, 0), at(i0 + 2, 0), t);
    r[i] = mono ? l[i] : hermite4(at(i0 - 1, 1), at(i0, 1), at(i0 + 1, 1), at(i0 + 2, 1), t);
    // A crossfaded loop: over its last `xfade` frames, the audio the same distance before the
    // loop's start fades in as the end fades out; at the end the loop's start carries on from it.
    if (v.xfade > 0 && v.loop == LoopMode::Forward && v.dir > 0 && v.pos >= v.loop_b - v.xfade && v.pos < v.loop_b) {
      const float x = float((v.pos - (v.loop_b - v.xfade)) / v.xfade);  // 0..1 across the fade
      float g_out, g_in;
      if (v.xfade_linear) g_out = 1 - x, g_in = x;
      else g_out = std::cos(x * 1.5707963f), g_in = std::sin(x * 1.5707963f);
      const int j0 = i0 - (v.loop_b - v.loop_a);
      const float pl = hermite4(at(j0 - 1, 0), at(j0, 0), at(j0 + 1, 0), at(j0 + 2, 0), t);
      const float pr = mono ? pl : hermite4(at(j0 - 1, 1), at(j0, 1), at(j0 + 1, 1), at(j0 + 2, 1), t);
      l[i] = l[i] * g_out + pl * g_in;
      r[i] = r[i] * g_out + pr * g_in;
    }
    v.pos += v.inc * v.dir;
    switch (v.loop) {
      case LoopMode::Off:
        if (v.pos >= v.end || v.pos < v.begin) v.done = true;
        break;
      case LoopMode::Forward: {
        const double len = v.loop_b - v.loop_a;
        if (v.dir > 0 && v.pos >= v.loop_b) v.pos -= len;
        if (v.dir < 0 && v.pos < v.loop_a) v.pos += len;
        break;
      }
      case LoopMode::PingPong:
        if (v.dir > 0 && v.pos >= v.loop_b) {
          v.pos = 2.0 * v.loop_b - v.pos;
          v.dir = -1;
        } else if (v.dir < 0 && v.pos < v.loop_a) {
          v.pos = 2.0 * v.loop_a - v.pos;
          v.dir = 1;
        }
        break;
    }
  }
}

void Engine::update_lanes(double tick) {
  for (int ti = 0; ti < int(snap_->tracks.size()) && ti < kMaxTracks; ++ti) {
    int& count = lane_counts_[ti];
    count = 0;
    for (const SnapPlacement& pl : snap_->tracks[size_t(ti)].placements) {
      if (pl.start > tick) break;
      if (tick >= pl.start + pl.length || pl.clip->lanes.empty()) continue;
      const Tick ct = Tick(std::fmod(tick - pl.start + pl.offset, double(pl.clip->length)));
      for (const Lane& l : pl.clip->lanes)
        if (count < kMaxLanesPerClip) lane_values_[ti][count++] = LaneValue{l.param, l.value_at(ct)};
      break;
    }
  }
}

namespace {
float lfo_value(LfoShape shape, float ph, float random) {
  switch (shape) {
    case LfoShape::Sine: return std::sin(6.2831853f * ph);
    case LfoShape::Triangle: return 1 - 4 * std::fabs(ph - 0.5f);
    case LfoShape::Saw: return 2 * ph - 1;
    case LfoShape::Square: return ph < 0.5f ? 1 : -1;
    case LfoShape::Random: return random;
  }
  return 0;
}
}  // namespace

float Engine::lfo_hz_now(const Modulator& m) const {
  return m.sync ? float(tempo() / 60.0 / lfo_sync_beats(m.rate)) : lfo_hz(m.rate);
}

void Engine::update_track_lfos(bool playing) {
  for (int ti = 0; ti < int(snap_->tracks.size()) && ti < kMaxTracks; ++ti) {
    const SnapTrack& t = snap_->tracks[size_t(ti)];
    for (int i = 0; i < kModulators; ++i) {
      const Modulator& m = t.mods[i];
      if (m.type != ModType::Lfo || m.retrigger) continue;
      float& ph = track_lfo_phase_[ti][i];
      const float before = ph;
      if (m.sync && playing) {  // where the song is in the cycle: lines up with the bars
        const double cycle = lfo_sync_beats(m.rate) * kTicksPerQuarter;
        ph = float(tick_ / cycle - std::floor(tick_ / cycle));
      } else {
        ph += lfo_hz_now(m) * kSubBlock / float(kSampleRate);
        ph -= std::floor(ph);
      }
      if (ph < before) {  // a new cycle: a new random value
        rng_ = rng_ * 1664525u + 1013904223u;
        track_lfo_random_[ti][i] = (rng_ >> 8) / float(1 << 24) * 2 - 1;
      }
    }
  }
}

void Engine::update_modulators(Voice& v, const SnapTrack& t) {
  const float sr = float(kSampleRate);
  const float elapsed = float(clock_ - v.mod_started) / sr;
  for (int i = 0; i < kModulators; ++i) {
    const Modulator& m = t.mods[i];
    float out = 0;
    switch (m.type) {
      case ModType::Off:
        break;
      case ModType::Envelope: {
        const float a = env_seconds(m.attack), d = env_seconds(m.decay);
        out = elapsed < a ? elapsed / a : std::exp(-4.6f * (elapsed - a) / d);
        break;
      }
      case ModType::Lfo: {
        if (!m.retrigger && v.track >= 0 && v.track < kMaxTracks) {  // the track's, kept by update_track_lfos
          out = lfo_value(m.shape, track_lfo_phase_[v.track][i], track_lfo_random_[v.track][i]);
          break;
        }
        float& ph = v.mod_phase[i];
        ph += lfo_hz_now(m) * kSubBlock / sr;
        if (ph >= 1) {
          ph -= std::floor(ph);
          rng_ = rng_ * 1664525u + 1013904223u;
          v.mod_random[i] = (rng_ >> 8) / float(1 << 24) * 2 - 1;
        }
        out = lfo_value(m.shape, ph, v.mod_random[i]);
        break;
      }
      case ModType::Velocity:
        out = v.note.velocity / 127.0f;
        break;
      case ModType::KeyTrack:
        out = std::max(-1.0f, std::min(1.0f, (v.note.pitch - 60) / 48.0f));
        break;
    }
    v.mod_out[i] = out;
  }
}

void Engine::render(float* out, int frames) {
  // Timed as a whole, so the load counts everything the callback does.
  const auto t0 = std::chrono::steady_clock::now();
  render_block(out, frames);
  if (frames <= 0) return;
  const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const float load = float(took * kSampleRate / frames);
  const float a = 1.0f - std::exp(-float(frames) / kSampleRate);  // about a second
  load_avg_.store(load_avg_.load(std::memory_order_relaxed) + (load - load_avg_.load(std::memory_order_relaxed)) * a,
                  std::memory_order_relaxed);
  if (load > load_peak_.load(std::memory_order_relaxed)) load_peak_.store(load, std::memory_order_relaxed);
  if (load > load_max_.load(std::memory_order_relaxed)) load_max_.store(load, std::memory_order_relaxed);
  if (load > 1) late_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::render_block(float* out, int frames) {
  Command cmd;
  Snapshot* s;
  while (incoming_.pop(s)) {
    if (snap_) {
      // Hand the old snapshot back for the UI thread to free; hold it if the queue is full.
      bool held = outgoing_.push(snap_);
      for (Snapshot*& slot : pending_free_)
        if (!held && !slot) {
          slot = snap_;
          held = true;
        }
      if (!held) {
        // Only if the UI thread stopped collecting garbage: skip this snapshot rather than free
        // the playing one here (freeing memory on the audio thread can stall it).
        delete s;
        continue;
      }
    }
    // A tempo change takes effect from here: re-anchor the tick position.
    if (!snap_ || (tempo_override_ <= 0 && snap_->tempo != s->tempo)) {
      anchor_tick_ = tick_;
      anchor_clock_ = clock_;
    }
    snap_ = s;
    // Sampler voices on a sample the new snapshot doesn't use stop here, so it can be freed.
    for (auto& v : voices_) {
      if (v->track < 0 || v->instrument != InstrumentType::Sampler || !v->sample) continue;
      bool used = false;
      if (v->track < int(snap_->tracks.size())) {
        const SnapTrack& t = snap_->tracks[size_t(v->track)];
        used = t.sample == v->sample;
        for (const SampleData* k : t.kit) used = used || k == v->sample;
        for (const SnapZone& z : t.zones) used = used || z.sample == v->sample;
      }
      if (!used) v->track = -1;
    }
#if TWILL_PLUGINS
    sync_plugins();
#endif
    revision_.store(snap_->revision);
  }
  for (Snapshot*& slot : pending_free_)
    if (slot && outgoing_.push(slot)) slot = nullptr;
  // Commands after the snapshot, so an audition plays the newest settings.
  while (commands_.pop(cmd)) apply_command(cmd);
  callbacks_.fetch_add(1);

  std::fill(out, out + 2 * frames, 0.0f);
  // Where this callback sits in time, for whoever sends the MIDI out.
  midi_out_.push(MidiEvent{MidiEvent::Anchor, clock_,
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count(),
                           MidiMessage{}});
  if (!snap_) return;

  const bool playing = playing_.load();
  const double tps = tempo() * kTicksPerQuarter / (60.0 * kSampleRate);
  const float glide_step = 1.0f - std::exp(-float(kSubBlock) / (kGlideSeconds * kSampleRate));
  float main[kSubBlock], aux[kSubBlock], vl[kSubBlock], vr[kSubBlock];
  float bus_chorus[2 * kSubBlock], bus_delay[2 * kSubBlock], bus_reverb[2 * kSubBlock];
  float peaks[kMaxTracks] = {};
  float master_peak = 0;
  int active = 0;

  for (int pos = 0; pos < frames;) {
    // A callback can end partway through a grid block; the rest of it comes next callback. Work
    // that happens once per block runs at its start (grid_start) or end (grid_end) only, so a
    // split block behaves exactly like a whole one.
    const int n = std::min(int(kSubBlock - clock_ % kSubBlock), frames - pos);
    const bool grid_start = clock_ % kSubBlock == 0;
    const bool grid_end = (clock_ + uint64_t(n)) % kSubBlock == 0;
#if TWILL_PLUGINS
    // A plugin block ends here: each plugin plays it, and it sounds over the one starting.
    if (clock_ % kPluginBlock == 0) run_plugins();
#endif
    if (playing && grid_start) {
      // Notes for the whole grid block start at its first sample. Ticks come from the anchor
      // rather than being summed per block, so long playback doesn't drift.
      const uint64_t block_end = clock_ + kSubBlock;
      double t0 = tick_, t1 = anchor_tick_ + double(block_end - anchor_clock_) * tps;
      if (snap_->loop && t0 < snap_->loop_end && t1 >= snap_->loop_end) {
        schedule(t0, snap_->loop_end);
        t1 = snap_->loop_start + (t1 - snap_->loop_end);
        anchor_tick_ = t1;
        anchor_clock_ = block_end;
        schedule(snap_->loop_start, t1);
      } else {
        schedule(t0, t1);
      }
      tick_ = t1;
      elapsed_ += kSubBlock * tps;
    }
    // MIDI Out notes due to end.
    for (PendingOff& p : pending_offs_)
      if (p.samples > 0 && p.samples <= n) {
        midi_out(MidiMessage{p.status, p.pitch, 0}, clock_ + uint64_t(p.samples));
        p.samples = 0;
      } else if (p.samples > 0) {
        p.samples -= n;
      }
#if TWILL_PLUGINS
    // Plugin notes due to end.
    for (PluginOff& p : plugin_offs_)
      if (p.samples > 0 && p.samples <= n) {
        plugin_event(p.track, p.pitch, 0, clock_ + uint64_t(p.samples));
        p.samples = 0;
      } else if (p.samples > 0) {
        p.samples -= n;
      }
#endif
    // MIDI clock: 24 a beat, on its own steady grid (not the song position, which jumps at loops).
    if (playing && snap_->midi_clock_out) {
      const double every = 60.0 * kSampleRate / (tempo() * 24);
      if (next_midi_clock_ < double(clock_)) next_midi_clock_ = double(clock_);  // just switched on
      while (next_midi_clock_ < double(clock_ + uint64_t(n))) {
        midi_out(MidiMessage{0xF8, 0, 0}, uint64_t(next_midi_clock_ + 0.5));
        next_midi_clock_ += every;
      }
    }
    if (grid_start) {
      if (playing) {
        update_lanes(tick_);
        update_midi_cc();
      } else {
        std::fill(std::begin(lane_counts_), std::end(lane_counts_), 0);
      }
      // The effects and master: the project's settings, then any lane for them on any track.
      update_track_lfos(playing);
      mix_ = snap_->mix;
      for (int ti = 0; ti < int(snap_->tracks.size()) && ti < kMaxTracks; ++ti)
        for (int li = 0; li < lane_counts_[ti]; ++li)
          if (lane_values_[ti][li].id >= 300) mix_.set(lane_values_[ti][li].id, lane_values_[ti][li].value);
    }

    std::fill(bus_chorus, bus_chorus + 2 * n, 0.0f);
    std::fill(bus_delay, bus_delay + 2 * n, 0.0f);
    std::fill(bus_reverb, bus_reverb + 2 * n, 0.0f);
    for (auto& vp : voices_) {
      Voice& v = *vp;
      if (v.track < 0) continue;
      if (v.track >= int(snap_->tracks.size())) {  // its track went away
        v.track = -1;
        continue;
      }
      const SnapTrack& t = snap_->tracks[size_t(v.track)];
      // The track changed instrument under this voice, or the voice was never one that sounds
      // here (MIDI Out tracks play on the MIDI device, not on voices).
      if (t.instrument != v.instrument || v.instrument == InstrumentType::MidiOut) {
        v.track = -1;
        continue;
      }
      // Track settings can change while a note sounds; the note keeps its pitch and locks.
      // Modulators go on top.
      if (grid_start) update_modulators(v, t);
      const int tl = v.track < kMaxTracks ? v.track : 0;
      Settings set = locked(t, v.note, lane_values_[tl], v.track < kMaxTracks ? lane_counts_[tl] : 0);
      // Mixer automation: level and pan.
      float track_level = t.level, track_pan = t.pan;
      float send_c = t.send_chorus, send_d = t.send_delay, send_r = t.send_reverb;
      for (int li = 0; v.track < kMaxTracks && li < lane_counts_[tl]; ++li) {
        const LaneValue& lv = lane_values_[tl][li];
        if (lv.id == kParamLevel) track_level = lv.value;
        if (lv.id == kParamPan) track_pan = lv.value;
        if (lv.id == kParamSendChorus) send_c = lv.value;
        if (lv.id == kParamSendDelay) send_d = lv.value;
        if (lv.id == kParamSendReverb) send_r = lv.value;
      }
      float pitch_offset = 0;
      for (int m = 0; m < kModulators; ++m) {
        const Modulator& mod = t.mods[m];
        if (mod.type == ModType::Off || mod.dest == kParamNone) continue;
        const float amount = mod.depth * v.mod_out[m];
        if (mod.dest == kParamPitch) {
          pitch_offset += amount * 24;
        } else if (const ParamInfo* info = param_info(mod.dest)) {
          set_param(set, mod.dest, get_param(set, mod.dest) + amount * (info->max - info->min));
        }
      }
      if (grid_start) v.tone.set(set.tone, float(kSampleRate));
      float env_from = 1, env_to = 1;
      const bool use_adsr = v.instrument == InstrumentType::Sampler || set.tone.amp_mode == 1;
      if (use_adsr && grid_start) {
        env_from = v.env.advance(set.tone, kSubBlock, float(kSampleRate));
        env_to = v.env.level();
      } else if (use_adsr) {
        env_from = env_to = v.env.level();
      }
      if (v.fade_per_block < 1) {  // a kit slot's decay, on top of the envelope
        env_from *= v.fade;
        if (grid_start) v.fade *= v.fade_per_block;
        env_to *= v.fade;
        if (v.fade < 0.001f) v.done = true;  // faded out: free it at the end of the grid block
      }

      if (v.instrument == InstrumentType::Macro) {
        if (grid_start) v.glide_note += (v.pitch_target - v.glide_note) * glide_step;
        MacroParams p = macro_params_for(set.macro, set.tone, v.glide_note + pitch_offset);
        p.fm_bank = t.fm_bank;
        // With the ADSR in charge the low-pass gate stays open and the envelope is applied here, as
        // a plain amplifier: the gate's own slow closing would otherwise smooth the envelope away
        // (a retriggered note's attack, for one, would be inaudible).
        p.level = 1;
        p.envelope = env_to;  // what the FM, timbre and morph amounts follow
        p.arp_step = float(60.0 * kSampleRate / (tempo() * arp_steps_per_beat(set.macro.arp_rate)));
        v.params = p;
        v.synth.render(p, main, aux, n);
        for (int i = 0; i < n; ++i) {
          const float x = set.macro.output == MacroOutput::Main ? main[i]
                          : set.macro.output == MacroOutput::Aux ? aux[i]
                                                                 : 0.5f * (main[i] + aux[i]);
          const float e = use_adsr ? env_from + (env_to - env_from) * float(i + 1) / float(n) : 1.0f;
          vl[i] = vr[i] = x * e;
        }
      } else {
        v.inc = v.base_inc * (pitch_offset != 0 ? std::exp2(pitch_offset / 12.0) : 1.0);
        render_sampler(v, vl, vr, n);
        // The envelope, ramped across the block.
        if (env_from == env_to) {
          for (int i = 0; i < n; ++i) vl[i] *= env_to, vr[i] *= env_to;
        } else {
          const float step = (env_to - env_from) / float(n);
          for (int i = 0; i < n; ++i) {
            const float e = env_from + step * float(i + 1);
            vl[i] *= e;
            vr[i] *= e;
          }
        }
      }
      v.tone.process(vl, vr, n);

      // Mix. The macro oscillator is mono: equal-power pan. Samples are stereo: balance, plus
      // the kit slot's own pan.
      float gl, gr;
      if (v.instrument == InstrumentType::Macro) {
        gl = std::cos((track_pan + 1) * 0.785398f);
        gr = std::sin((track_pan + 1) * 0.785398f);
      } else {
        const float pan = std::max(-1.0f, std::min(1.0f, track_pan + v.pan));
        gl = pan > 0 ? 1 - pan : 1;
        gr = pan < 0 ? 1 + pan : 1;
      }
      gl *= track_level * v.gain;
      gr *= track_level * v.gain;
      float& peak = v.block_peak;
      float& meter = peaks[tl];
      const bool sends = send_c != 0 || send_d != 0 || send_r != 0;
      float* o = out + 2 * pos;
      for (int i = 0; i < n; ++i) {
        const float l = vl[i] * gl, r = vr[i] * gr;
        o[2 * i] += l;
        o[2 * i + 1] += r;
        if (sends) {  // after the track's level and pan
          bus_chorus[2 * i] += l * send_c, bus_chorus[2 * i + 1] += r * send_c;
          bus_delay[2 * i] += l * send_d, bus_delay[2 * i + 1] += r * send_d;
          bus_reverb[2 * i] += l * send_r, bus_reverb[2 * i + 1] += r * send_r;
        }
        peak = std::max(peak, std::max(std::fabs(vl[i]), std::fabs(vr[i])));
        meter = std::max(meter, std::max(std::fabs(l), std::fabs(r)));
      }

      if (v.gate > 0) {
        v.gate -= n;
        if (v.gate <= 0) release(v);
        else if (v.one_shot && grid_end && v.env.idle()) v.track = -1;  // choked or hit again: free
      } else if (grid_end) {
        v.quiet = peak < kQuietLevel ? v.quiet + 1 : 0;
        bool finished;
        if (v.instrument == InstrumentType::Sampler) finished = v.done || v.env.idle();
        else if (use_adsr) finished = v.env.idle();
        else finished = v.quiet > kQuietBlocksToFree;
        if (finished || clock_ - v.started > uint64_t(kMaxRingSamples)) v.track = -1;  // free
      }
      if (v.instrument == InstrumentType::Sampler && v.done && grid_end) v.track = -1;
      if (grid_end) {
        v.last_peak = peak;
        peak = 0;
      }
      ++active;
    }

#if TWILL_PLUGINS
    // Plugin tracks: what each played for the last plugin block, through the track's level, pan
    // (balance: a plugin is stereo) and sends, with their automation, as a voice is.
    for (int ti = 0; ti < int(snap_->tracks.size()) && ti < kMaxTracks; ++ti) {
      PluginTrack& pt = plugin_[ti];
      if (!pt.inst) continue;
      const SnapTrack& t = snap_->tracks[size_t(ti)];
      float track_level = t.level, track_pan = t.pan;
      float send_c = t.send_chorus, send_d = t.send_delay, send_r = t.send_reverb;
      for (int li = 0; li < lane_counts_[ti]; ++li) {
        const LaneValue& lv = lane_values_[ti][li];
        if (lv.id == kParamLevel) track_level = lv.value;
        if (lv.id == kParamPan) track_pan = lv.value;
        if (lv.id == kParamSendChorus) send_c = lv.value;
        if (lv.id == kParamSendDelay) send_d = lv.value;
        if (lv.id == kParamSendReverb) send_r = lv.value;
      }
      const float pan = std::max(-1.0f, std::min(1.0f, track_pan));
      const float gl = (pan > 0 ? 1 - pan : 1) * track_level, gr = (pan < 0 ? 1 + pan : 1) * track_level;
      const int at = int(clock_ % kPluginBlock);
      float* o = out + 2 * pos;
      float& meter = peaks[ti];
      for (int i = 0; i < n; ++i) {
        const float l = pt.out[0][at + i] * gl, r = pt.out[1][at + i] * gr;
        o[2 * i] += l;
        o[2 * i + 1] += r;
        bus_chorus[2 * i] += l * send_c, bus_chorus[2 * i + 1] += r * send_c;
        bus_delay[2 * i] += l * send_d, bus_delay[2 * i + 1] += r * send_d;
        bus_reverb[2 * i] += l * send_r, bus_reverb[2 * i + 1] += r * send_r;
        meter = std::max(meter, std::max(std::fabs(l), std::fabs(r)));
      }
    }
#endif

    // Effects returns. They run every block (their tails keep ringing after a send goes to zero);
    // with nothing sent their output is exactly zero.
    effects_->process(mix_, float(tempo()), bus_chorus, bus_delay, bus_reverb, out + 2 * pos, n);

    // The sample browser's preview, straight to the output.
    if (preview_) {
      for (int i = 0; i < n && preview_; ++i) {
        out[2 * (pos + i)] += 0.8f * preview_->left(preview_pos_);
        out[2 * (pos + i) + 1] += 0.8f * preview_->right(preview_pos_);
        if (++preview_pos_ >= preview_->frames) preview_ = nullptr;
      }
    }
    // Master, a grid block at a time so automation lands the same at any callback size: EQ
    // (bypassed when flat), compressor, then level and the limiter.
    float* block = out + 2 * pos;
    if (!master_bypass_) {
      effects_->master_eq(mix_, block, n);
      effects_->master_compressor(mix_, block, n);
      effects_->master_out(mix_, block, n, output_boost_.load(std::memory_order_relaxed));
    }
    for (int i = 0; i < 2 * n; ++i) master_peak = std::max(master_peak, std::fabs(block[i]));
    clock_ += uint64_t(n);
    pos += n;
  }
  comp_gr_.store(effects_->gain_reduction_db(), std::memory_order_relaxed);
  for (int t = 0; t < kMaxTracks; ++t)
    if (peaks[t] > track_peak_[t].load(std::memory_order_relaxed)) track_peak_[t].store(peaks[t]);
  if (master_peak > master_peak_.load(std::memory_order_relaxed)) master_peak_.store(master_peak);

  position_.store(Tick(tick_));
  elapsed_ticks_.store(elapsed_, std::memory_order_relaxed);
  active_voices_.store(active * kSubBlock / std::max(1, frames));  // average over the callback
  for (size_t i = 0; i < voices_.size(); ++i) {
    const Voice& v = *voices_[i];
    int32_t z = -1;
    if (v.track >= 0 && v.track < int(snap_->tracks.size()) && v.zone >= 0 && v.instrument == InstrumentType::Sampler) {
      const SnapTrack& t = snap_->tracks[size_t(v.track)];
      if (t.sampler.mode == SamplerMode::Multi && v.zone < int(t.zones.size())) z = (v.track << 20) | t.zones[size_t(v.zone)].index;
    }
    voice_zones_[i].store(z, std::memory_order_relaxed);
  }
}

#if TWILL_PLUGINS
// A note on or off for a plugin track, at `sample` on the engine's clock: timed within the plugin
// block being gathered (an off due exactly at its end goes in as its last frame).
void Engine::plugin_event(int track, uint8_t pitch, uint8_t velocity, uint64_t sample) {
  if (track < 0 || track >= kMaxTracks || pitch > 127) return;
  PluginTrack& pt = plugin_[track];
  if (!pt.inst) return;
  if (velocity == 0) {
    if (pt.held[pitch] == 0) return;  // never struck, or already let go
    --pt.held[pitch];
  } else if (pt.held[pitch] < 255) {
    ++pt.held[pitch];
  }
  if (pt.count >= int(sizeof pt.events / sizeof pt.events[0])) return;
  const uint64_t block = clock_ - clock_ % kPluginBlock;
  const uint64_t into = sample > block ? sample - block : 0;
  pt.events[pt.count++] = PluginEvent{uint32_t(std::min<uint64_t>(into, kPluginBlock - 1)), pitch, velocity};
}

// A note now, and its end `gate` samples later (none: a live note, ended by the keyboard).
void Engine::plugin_note(int track, uint8_t pitch, uint8_t velocity, int64_t gate) {
  plugin_event(track, pitch, std::max<uint8_t>(1, velocity), clock_);
  if (gate <= 0) return;
  for (PluginOff& p : plugin_offs_)
    if (p.samples <= 0) {
      p = PluginOff{track, pitch, gate};
      return;
    }
  plugin_event(track, pitch, 0, clock_);  // no room to wait: end it now
}

// Stop: every note a plugin holds let go of.
void Engine::plugins_all_off() {
  for (PluginOff& p : plugin_offs_) p.samples = 0;
  for (int ti = 0; ti < kMaxTracks; ++ti)
    for (int k = 0; k < 128; ++k)
      while (plugin_[ti].held[k]) plugin_event(ti, uint8_t(k), 0, clock_);
}

// The new snapshot's instances, track by track. One that moved to another track takes its notes,
// its pending block and its output with it; one that went is no longer played (the app lets go of
// its notes once nothing plays it: PluginInstance::released).
void Engine::sync_plugins() {
  PluginTrack was[kMaxTracks];
  for (int ti = 0; ti < kMaxTracks; ++ti) was[ti] = plugin_[ti];
  for (int ti = 0; ti < kMaxTracks; ++ti) {
    PluginInstance* inst = nullptr;
    if (ti < int(snap_->tracks.size()) && snap_->tracks[size_t(ti)].instrument == InstrumentType::Plugin)
      inst = snap_->tracks[size_t(ti)].plugin;
    PluginTrack& pt = plugin_[ti];
    if (pt.inst == inst) continue;
    pt = PluginTrack{};
    pt.inst = inst;
    if (!inst) continue;
    for (int from = 0; from < kMaxTracks; ++from)
      if (was[from].inst == inst) {
        pt = was[from];
        for (PluginOff& p : plugin_offs_)
          if (p.track == from && p.samples > 0) p.track = ti;
        break;
      }
  }
  // Notes waiting to end on a track that has no plugin now: gone with it.
  for (PluginOff& p : plugin_offs_)
    if (p.samples > 0 && (p.track < 0 || p.track >= kMaxTracks || !plugin_[p.track].inst)) p.samples = 0;
}

void Engine::run_plugins() {
  for (PluginTrack& pt : plugin_) {
    if (!pt.inst) continue;
    pt.inst->process(pt.events, pt.count, pt.out[0], pt.out[1], kPluginBlock);
    pt.count = 0;
  }
}
#endif

}  // namespace twill
