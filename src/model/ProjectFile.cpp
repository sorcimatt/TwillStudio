#include "model/ProjectFile.h"

#include <algorithm>
#include <cstring>

namespace twill {

namespace {

constexpr uint32_t kFormatVersion = 1;
constexpr uint16_t kProjVersion = 6, kTrakVersion = 10, kClipVersion = 2;

class Writer {
 public:
  void u8(uint8_t v) { b_.push_back(v); }
  void u16(uint16_t v) { put(&v, 2); }
  void u32(uint32_t v) { put(&v, 4); }
  void i32(int32_t v) { put(&v, 4); }
  void f32(float v) { put(&v, 4); }
  void str(const std::string& s) {
    u16(uint16_t(std::min<size_t>(s.size(), 0xffff)));
    put(s.data(), std::min<size_t>(s.size(), 0xffff));
  }
  void tag(const char* t) { put(t, 4); }
#if TWILL_PLUGINS
  void raw(const std::vector<uint8_t>& v) {
    u32(uint32_t(v.size()));
    put(v.data(), v.size());
  }
#endif
  // Starts a chunk; returns where its size goes, for end_chunk().
  size_t begin_chunk(const char* t) {
    tag(t);
    u32(0);
    return b_.size();
  }
  void end_chunk(size_t start) {
    uint32_t size = uint32_t(b_.size() - start);
    std::memcpy(&b_[start - 4], &size, 4);
  }
  std::vector<uint8_t> take() { return std::move(b_); }

 private:
  void put(const void* p, size_t n) {  // little-endian: every target we build for is
    const uint8_t* c = static_cast<const uint8_t*>(p);
    b_.insert(b_.end(), c, c + n);
  }
  std::vector<uint8_t> b_;
};

class Reader {
 public:
  Reader(const uint8_t* d, size_t n) : d_(d), end_(n) {}
  bool ok() const { return ok_; }
  size_t pos() const { return pos_; }
  size_t end() const { return end_; }
  void set_end(size_t e) { end_ = e; }
  void seek(size_t p) { pos_ = p; }
  bool at_end() const { return pos_ >= end_; }

  uint8_t u8() { uint8_t v = 0; get(&v, 1); return v; }
  uint16_t u16() { uint16_t v = 0; get(&v, 2); return v; }
  uint32_t u32() { uint32_t v = 0; get(&v, 4); return v; }
  int32_t i32() { int32_t v = 0; get(&v, 4); return v; }
  float f32() { float v = 0; get(&v, 4); return v; }
  std::string str() {
    uint16_t n = u16();
    if (!ok_ || pos_ + n > end_) return fail(), std::string();
    std::string s(reinterpret_cast<const char*>(d_ + pos_), n);
    pos_ += n;
    return s;
  }
  bool tag(char out[4]) { return get(out, 4); }
#if TWILL_PLUGINS
  std::vector<uint8_t> raw() {
    const uint32_t n = u32();
    if (!ok_ || pos_ + n > end_) return fail(), std::vector<uint8_t>();
    std::vector<uint8_t> v(d_ + pos_, d_ + pos_ + n);
    pos_ += n;
    return v;
  }
#endif

 private:
  bool get(void* out, size_t n) {
    if (!ok_ || pos_ + n > end_) return fail();
    std::memcpy(out, d_ + pos_, n);
    pos_ += n;
    return true;
  }
  bool fail() { ok_ = false; return false; }
  const uint8_t* d_;
  size_t pos_ = 0, end_;
  bool ok_ = true;
};

}  // namespace

std::vector<uint8_t> save_project(const Project& p) {
  Writer w;
  w.tag("TWIL");
  w.u32(kFormatVersion);

  size_t c = w.begin_chunk("PROJ");
  w.u16(kProjVersion);
  w.str(p.name);
  w.f32(p.tempo);
  w.u8(uint8_t(p.time_sig_num));
  w.u8(uint8_t(p.time_sig_den));
  w.u8(p.loop);
  w.i32(p.loop_start);
  w.i32(p.loop_end);
  w.u32(p.next_clip_id);
  w.u8(uint8_t(p.key));    // version 2
  w.u8(uint8_t(p.scale));  // version 2
  const MixSettings& m = p.mix;  // version 3
  for (float v : {m.master, m.eq_low, m.eq_mid, m.eq_high, m.reverb_size, m.reverb_tone, m.delay_time,
                  m.delay_feedback, m.chorus_rate, m.chorus_depth})
    w.f32(v);
  w.u8(uint8_t(p.midi_clock));  // version 4 (was on/off; 2, follow, came later)
  w.f32(p.mix.comp_threshold);  // version 5
  w.f32(p.mix.comp_ratio);
  w.f32(p.mix.comp_release);
  w.u8(p.mix.limiter);  // version 6
  w.end_chunk(c);

  for (const Track& t : p.tracks) {
    c = w.begin_chunk("TRAK");
    w.u16(kTrakVersion);
    w.str(t.name);
    w.u32(t.colour);
    w.u8(uint8_t(t.instrument));
    w.u8(uint8_t(t.voices));
    w.f32(t.level);
    w.f32(t.pan);
    w.u8(t.mute);
    w.u8(t.solo);
    const MacroSettings& m = t.macro;
    w.u8(uint8_t(m.engine));
    for (float v : {m.harmonics, m.timbre, m.morph, m.decay, m.lpg_colour, m.fm_amount,
                    m.timbre_amount, m.morph_amount})
      w.f32(v);
    w.u8(uint8_t(m.output));
    w.u32(uint32_t(t.arrangement.size()));
    for (const Placement& pl : t.arrangement) {
      w.i32(pl.start);
      w.u32(pl.clip_id);
      w.i32(pl.offset);
      w.i32(pl.length);
    }
    // Version 2: sampler, tone, sends.
    const SamplerSettings& sm = t.sampler;
    w.u8(uint8_t(sm.mode));
    w.str(sm.sample);
    w.u8(uint8_t(sm.root));
    for (float v : {sm.fine, sm.start, sm.end, sm.loop_start, sm.loop_end}) w.f32(v);
    w.u8(uint8_t(sm.loop));
    w.u8(sm.reverse);
    w.u8(uint8_t(sm.slices));
    w.u8(kKitSlots);
    for (const KitSlot& k : sm.kit) {
      w.str(k.sample);
      for (float v : {k.tune, k.level, k.pan}) w.f32(v);
      w.u8(uint8_t(k.choke));
    }
    const ToneSettings& tn = t.tone;
    w.u8(uint8_t(tn.filter));
    for (float v : {tn.cutoff, tn.resonance, tn.drive, tn.attack, tn.decay, tn.sustain, tn.release}) w.f32(v);
    w.u8(uint8_t(tn.amp_mode));
    for (float v : {t.send_chorus, t.send_delay, t.send_reverb}) w.f32(v);
    // Version 3: modulators.
    w.u8(kModulators);
    for (const Modulator& m : t.mods) {
      w.u8(uint8_t(m.type));
      w.u16(m.dest);
      for (float v : {m.depth, m.rate, m.attack, m.decay}) w.f32(v);
      w.u8(uint8_t(m.shape));
      w.u8(m.sync);
    }
    w.u8(uint8_t(t.midi_channel));  // version 4
    w.u8(uint8_t(t.midi_cc[0]));    // version 5
    w.u8(uint8_t(t.midi_cc[1]));
    w.u8(t.sampler.slice_transients);  // version 6
    for (const Modulator& m : t.mods) w.u8(m.retrigger);  // version 7
    for (const KitSlot& k : t.sampler.kit) w.f32(k.decay);  // version 8
    w.u8(uint8_t(t.macro.arp_rate));                        // version 9
    w.str(t.sampler.preset);                                 // version 10
    w.end_chunk(c);
  }
  // A track's own DX7 bank (Track::fm_bank): a chunk of its own (FMBK), which builds that don't
  // know it skip, playing the engine's built-in bank. Version 1: the track's index, then the path.
  for (size_t i = 0; i < p.tracks.size(); ++i) {
    if (p.tracks[i].fm_bank.empty()) continue;
    c = w.begin_chunk("FMBK");
    w.u16(1);
    w.u8(uint8_t(i));
    w.str(p.tracks[i].fm_bank);
    w.end_chunk(c);
  }
#if TWILL_PLUGINS
  // Plugin tracks: a chunk of their own (PLUG), which a build without plugins skips as it skips
  // any chunk it doesn't know, playing the track as MIDI out. Version 1.
  for (size_t i = 0; i < p.tracks.size(); ++i) {
    const PluginSettings& pl = p.tracks[i].plugin;
    if (pl.id.empty() && p.tracks[i].instrument != InstrumentType::Plugin) continue;
    c = w.begin_chunk("PLUG");
    w.u16(1);
    w.u8(uint8_t(i));
    w.str(pl.id);
    w.str(pl.path);
    w.str(pl.name);
    w.u32(pl.slot);
    w.raw(pl.state);
    w.end_chunk(c);
  }
#endif

  for (const Clip& cl : p.clips) {
    c = w.begin_chunk("CLIP");
    w.u16(kClipVersion);
    w.u32(cl.id);
    w.u8(uint8_t(cl.track));
    w.str(cl.name);
    w.i32(cl.length);
    w.u32(cl.next_note_id);
    w.u32(uint32_t(cl.notes.size()));
    for (const Note& n : cl.notes) {
      w.u32(n.id);
      w.i32(n.start);
      w.i32(n.length);
      w.u8(n.pitch);
      w.u8(n.velocity);
      w.u8(n.probability);
      w.u8(n.glide);
      w.u8(n.num_locks);
      for (int i = 0; i < n.num_locks; ++i) {
        w.u16(n.locks[i].id);
        w.f32(n.locks[i].value);
      }
    }
    // Version 2: automation lanes.
    w.u8(uint8_t(std::min<size_t>(cl.lanes.size(), kMaxLanesPerClip)));
    for (size_t li = 0; li < cl.lanes.size() && li < size_t(kMaxLanesPerClip); ++li) {
      const Lane& lane = cl.lanes[li];
      w.u16(lane.param);
      w.u32(uint32_t(lane.points.size()));
      for (const LanePoint& pt : lane.points) {
        w.i32(pt.tick);
        w.f32(pt.value);
      }
    }
    w.end_chunk(c);
  }
  return w.take();
}

bool load_project(const uint8_t* data, size_t size, Project& out, std::string& error) {
  Reader r(data, size);
  char tag[4];
  // "FLUO": files from before the app was renamed (Fluorescent), the same format.
  if (!r.tag(tag) || (std::memcmp(tag, "TWIL", 4) != 0 && std::memcmp(tag, "FLUO", 4) != 0)) {
    error = "not a project file";
    return false;
  }
  uint32_t format = r.u32();
  if (!r.ok() || format == 0 || format > kFormatVersion) {
    error = "project file is from a newer version of the app";
    return false;
  }

  Project p;
  p.tracks.clear();
  bool have_proj = false;
#if TWILL_PLUGINS
  std::vector<std::pair<size_t, PluginSettings>> plugs;  // PLUG chunks, put on their tracks at the end
#endif
  std::vector<std::pair<size_t, std::string>> fm_banks;  // FMBK chunks, likewise
  while (r.ok() && !r.at_end()) {
    if (!r.tag(tag)) break;
    uint32_t len = r.u32();
    size_t start = r.pos(), stop = start + len, file_end = r.end();
    if (!r.ok() || stop > file_end) {
      error = "project file is truncated";
      return false;
    }
    r.set_end(stop);  // fields past a chunk's end read as failures, not as the next chunk

    if (std::memcmp(tag, "PROJ", 4) == 0) {
      const uint16_t version = r.u16();  // each version appends fields to the previous one
      p.name = r.str();
      p.tempo = r.f32();
      p.time_sig_num = r.u8();
      p.time_sig_den = r.u8();
      p.loop = r.u8() != 0;
      p.loop_start = r.i32();
      p.loop_end = r.i32();
      p.next_clip_id = r.u32();
      if (version >= 2) {
        p.key = r.u8() % 12;
        uint8_t sc = r.u8();
        p.scale = sc < uint8_t(ScaleType::kCount) ? ScaleType(sc) : ScaleType::Chromatic;
      }
      if (version >= 3) {
        MixSettings& m = p.mix;
        for (float* v : {&m.master, &m.eq_low, &m.eq_mid, &m.eq_high, &m.reverb_size, &m.reverb_tone,
                         &m.delay_time, &m.delay_feedback, &m.chorus_rate, &m.chorus_depth})
          *v = r.f32();
      }
      if (version >= 4) p.midi_clock = MidiClock(std::min<uint8_t>(r.u8(), 2));
      if (version >= 5)
        for (float* v : {&p.mix.comp_threshold, &p.mix.comp_ratio, &p.mix.comp_release}) *v = r.f32();
      if (version >= 6) p.mix.limiter = r.u8() != 0;
      have_proj = true;
    } else if (std::memcmp(tag, "TRAK", 4) == 0) {
      Track t;
      const uint16_t version = r.u16();
      t.name = r.str();
      t.colour = r.u32();
      if (t.colour > 7) t.colour = uint32_t(p.tracks.size() % 8);  // older files: an unused RGB value
#if TWILL_PLUGINS
      t.instrument = InstrumentType(std::min<uint8_t>(r.u8(), 3));
#else
      t.instrument = InstrumentType(std::min<uint8_t>(r.u8(), 2));
#endif
      t.voices = r.u8();
      t.level = r.f32();
      t.pan = r.f32();
      t.mute = r.u8() != 0;
      t.solo = r.u8() != 0;
      MacroSettings& m = t.macro;
      m.engine = r.u8();
      for (float* v : {&m.harmonics, &m.timbre, &m.morph, &m.decay, &m.lpg_colour, &m.fm_amount,
                       &m.timbre_amount, &m.morph_amount})
        *v = r.f32();
      m.output = MacroOutput(r.u8());
      uint32_t n = r.u32();
      for (uint32_t i = 0; i < n && r.ok(); ++i) {
        Placement pl;
        pl.start = r.i32();
        pl.clip_id = r.u32();
        pl.offset = r.i32();
        pl.length = r.i32();
        t.arrangement.push_back(pl);
      }
      if (version >= 2) {
        SamplerSettings& sm = t.sampler;
        sm.mode = SamplerMode(std::min<uint8_t>(r.u8(), 3));
        sm.sample = r.str();
        sm.root = r.u8() & 127;
        for (float* v : {&sm.fine, &sm.start, &sm.end, &sm.loop_start, &sm.loop_end}) *v = r.f32();
        sm.loop = LoopMode(std::min<uint8_t>(r.u8(), 2));
        sm.reverse = r.u8() != 0;
        sm.slices = std::max<int>(2, r.u8());
        const int slots = r.u8();
        for (int i = 0; i < slots && r.ok(); ++i) {
          KitSlot k;
          k.sample = r.str();
          for (float* v : {&k.tune, &k.level, &k.pan}) *v = r.f32();
          k.choke = r.u8();
          if (i < kKitSlots) sm.kit[i] = k;
        }
        ToneSettings& tn = t.tone;
        tn.filter = FilterType(std::min<uint8_t>(r.u8(), 3));
        for (float* v : {&tn.cutoff, &tn.resonance, &tn.drive, &tn.attack, &tn.decay, &tn.sustain, &tn.release})
          *v = r.f32();
        tn.amp_mode = r.u8() ? 1 : 0;
        for (float* v : {&t.send_chorus, &t.send_delay, &t.send_reverb}) *v = r.f32();
      }
      if (version >= 3) {
        const int n = r.u8();
        for (int i = 0; i < n && r.ok(); ++i) {
          Modulator m;
          m.type = ModType(std::min<uint8_t>(r.u8(), 4));
          m.dest = ParamId(r.u16());
          for (float* v : {&m.depth, &m.rate, &m.attack, &m.decay}) *v = r.f32();
          m.shape = LfoShape(std::min<uint8_t>(r.u8(), 4));
          m.sync = r.u8() != 0;
          if (i < kModulators) t.mods[i] = m;
        }
      }
      if (version >= 4) t.midi_channel = r.u8() & 15;
      if (version >= 5)
        for (int& cc : t.midi_cc) cc = r.u8() & 127;
      if (version >= 6) t.sampler.slice_transients = r.u8() != 0;
      if (version >= 7)
        for (Modulator& m : t.mods) m.retrigger = r.u8() != 0;
      if (version >= 8)
        for (KitSlot& k : t.sampler.kit) k.decay = std::max(0.0f, std::min(1.0f, r.f32()));
      if (version >= 9) t.macro.arp_rate = ArpRate(std::min<uint8_t>(r.u8(), kNumArpRates - 1));
      if (version >= 10) t.sampler.preset = r.str();
      if (r.ok() && p.tracks.size() < size_t(kMaxTracks)) p.tracks.push_back(std::move(t));
    } else if (std::memcmp(tag, "CLIP", 4) == 0) {
      Clip cl;
      const uint16_t version = r.u16();
      cl.id = r.u32();
      cl.track = r.u8();
      cl.name = r.str();
      cl.length = r.i32();
      cl.next_note_id = r.u32();
      uint32_t n = r.u32();
      for (uint32_t i = 0; i < n && r.ok(); ++i) {
        Note note;
        note.id = r.u32();
        note.start = r.i32();
        note.length = r.i32();
        note.pitch = r.u8();
        note.velocity = r.u8();
        note.probability = r.u8();
        note.glide = r.u8() != 0;
        // Read every stored lock, keeping as many as this build supports.
        const int stored = r.u8();
        for (int k = 0; k < stored && r.ok(); ++k) {
          ParamLock lock{ParamId(r.u16()), 0};
          lock.value = r.f32();
          if (k < kMaxLocksPerNote) note.locks[note.num_locks++] = lock;
        }
        cl.notes.push_back(note);
      }
      if (version >= 2) {
        const int lanes = r.u8();
        for (int li = 0; li < lanes && r.ok(); ++li) {
          Lane lane;
          lane.param = ParamId(r.u16());
          const uint32_t pts = r.u32();
          for (uint32_t k = 0; k < pts && r.ok(); ++k) {
            LanePoint pt;
            pt.tick = r.i32();
            pt.value = r.f32();
            lane.points.push_back(pt);
          }
          if (li < kMaxLanesPerClip) cl.lanes.push_back(std::move(lane));
        }
      }
      if (r.ok()) p.clips.push_back(std::move(cl));
    }
#if TWILL_PLUGINS
    else if (std::memcmp(tag, "PLUG", 4) == 0) {
      r.u16();  // version 1
      const size_t track = r.u8();
      PluginSettings pl;
      pl.id = r.str();
      pl.path = r.str();
      pl.name = r.str();
      pl.slot = r.u32();
      pl.state = r.raw();
      if (r.ok()) plugs.emplace_back(track, std::move(pl));
    }
#endif
    else if (std::memcmp(tag, "FMBK", 4) == 0) {
      r.u16();  // version 1
      const size_t track = r.u8();
      std::string path = r.str();
      if (r.ok()) fm_banks.emplace_back(track, std::move(path));
    }
    // Unknown chunks, and anything newer at the end of a known one, are skipped.
    if (!r.ok()) {
      error = "project file is damaged";
      return false;
    }
    r.set_end(file_end);
    r.seek(stop);
  }
  if (!have_proj) {
    error = "project file has no project settings";
    return false;
  }
#if TWILL_PLUGINS
  for (auto& [track, pl] : plugs)
    if (track < p.tracks.size()) p.tracks[track].plugin = std::move(pl);
#endif
  for (auto& [track, path] : fm_banks)
    if (track < p.tracks.size()) p.tracks[track].fm_bank = std::move(path);
  out = std::move(p);
  return true;
}

}  // namespace twill
