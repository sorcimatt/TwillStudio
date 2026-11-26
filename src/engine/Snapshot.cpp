#include "engine/Snapshot.h"

#include <algorithm>
#include <cmath>

#include "engine/Sample.h"

namespace twill {

void SnapSampler::set(ParamId id, float v) {
  SamplerSettings s;
  s.mode = mode, s.root = root, s.fine = fine, s.start = start, s.end = end, s.loop = loop;
  s.loop_start = loop_start, s.loop_end = loop_end, s.reverse = reverse, s.slices = slices;
  s.slice_transients = slice_transients;
  s.set(id, v);
  mode = s.mode, root = s.root, fine = s.fine, start = s.start, end = s.end, loop = s.loop;
  loop_start = s.loop_start, loop_end = s.loop_end, reverse = s.reverse, slices = s.slices;
}

std::unique_ptr<Snapshot> build_snapshot(const Project& p, int max_tracks, uint64_t revision,
                                         const SampleResolver& samples,
                                         const MultisampleResolver& multisamples
#if TWILL_PLUGINS
                                         , const PluginResolver& plugins
#endif
                                         , const FmBankResolver& fm_banks
) {
  auto s = std::make_unique<Snapshot>();
  s->revision = revision;
  s->tempo = p.tempo;
  s->midi_clock_out = p.midi_clock == MidiClock::Send;
  s->loop = p.loop && p.loop_end > p.loop_start;
  s->loop_start = p.loop_start;
  s->loop_end = p.loop_end;
  s->mix = p.mix;

  s->clips.reserve(p.clips.size());  // placements keep pointers into this vector
  for (const Clip& c : p.clips) {
    SnapClip sc;
    sc.length = std::max<Tick>(1, c.length);
    for (const Note& n : c.notes)
      if (n.start >= 0 && n.start < c.length && n.length > 0) sc.notes.push_back(n);
    std::stable_sort(sc.notes.begin(), sc.notes.end(),
                     [](const Note& a, const Note& b) { return a.start < b.start; });
    for (const Lane& l : c.lanes)
      if (!l.points.empty()) sc.lanes.push_back(l);
    s->clips.push_back(std::move(sc));
  }
  auto snap_clip = [&](uint32_t id) -> const SnapClip* {
    for (size_t i = 0; i < p.clips.size(); ++i)
      if (p.clips[i].id == id) return &s->clips[i];
    return nullptr;
  };

  const int n = std::min<int>(int(p.tracks.size()), max_tracks);
  bool any_solo = false;
  for (int i = 0; i < n; ++i) any_solo |= p.tracks[i].solo;
  for (int i = 0; i < n; ++i) {
    const Track& t = p.tracks[i];
    SnapTrack st;
    st.instrument = t.instrument;
    st.voices = std::max(1, std::min(t.voices, kMaxVoicesPerTrack));
    st.legato = t.voices == 0;
    st.macro = t.macro;
    if (fm_banks && t.instrument == InstrumentType::Macro && !t.fm_bank.empty()) st.fm_bank = fm_banks(t.fm_bank);
    st.tone = t.tone;
    const SamplerSettings& sm = t.sampler;
    st.sampler.mode = sm.mode;
    st.sampler.root = sm.root;
    st.sampler.fine = sm.fine;
    st.sampler.start = sm.start;
    st.sampler.end = sm.end;
    st.sampler.loop = sm.loop;
    st.sampler.loop_start = sm.loop_start;
    st.sampler.loop_end = sm.loop_end;
    st.sampler.reverse = sm.reverse;
    st.sampler.slices = sm.slices;
    st.sampler.slice_transients = sm.slice_transients;
    for (int k = 0; k < kKitSlots; ++k)
      st.sampler.kit[k] = SnapSampler::Slot{sm.kit[k].tune, sm.kit[k].level, sm.kit[k].pan, sm.kit[k].choke,
                                           sm.kit[k].decay};
    if (samples && t.instrument == InstrumentType::Sampler) {
      if (!sm.sample.empty()) st.sample = samples(sm.sample);
      if (st.sample && sm.mode == SamplerMode::Slice && sm.slice_transients)
        st.slice_points = transient_slices(*st.sample, sm.start, sm.end, sm.slices);
      for (int k = 0; k < kKitSlots; ++k)
        if (!sm.kit[k].sample.empty()) st.kit[k] = samples(sm.kit[k].sample);
      const Multisample* ms = sm.mode == SamplerMode::Multi && !sm.preset.empty() && multisamples
                                  ? multisamples(sm.preset) : nullptr;
      if (ms)
        for (size_t zi = 0; zi < ms->zones.size(); ++zi) {
          const Zone& z = ms->zones[zi];
          const SampleData* d = samples(z.sample);
          if (!d || d->frames < 2) continue;  // missing: said when the preset loaded
          // The zone's frames are the file's; the sample is kept at 48 kHz.
          const double scale = d->source_rate > 0 ? 48000.0 / d->source_rate : 1.0;
          auto frame = [&](int f, int fallback) {
            return f < 0 ? fallback : std::max(0, std::min(d->frames, int(std::lround(f * scale))));
          };
          SnapZone sz;
          sz.sample = d;
          sz.index = int(zi);
          sz.lo_key = z.lo_key, sz.hi_key = z.hi_key, sz.lo_vel = z.lo_vel, sz.hi_vel = z.hi_vel, sz.root = z.root;
          sz.tune = z.tune, sz.gain = z.gain, sz.pan = z.pan;
          sz.begin = std::min(d->frames - 1, frame(z.start, 0));
          sz.end = std::max(sz.begin + 1, frame(z.end, d->frames));
          sz.loop = z.loop || (z.loop_if_marked && d->loop_start >= 0 && d->loop_end > d->loop_start);
          sz.one_shot = z.one_shot;
          sz.keytrack = z.keytrack, sz.tune_after_keytrack = z.tune_after_keytrack, sz.veltrack = z.veltrack;
          sz.lo_rand = z.lo_rand, sz.hi_rand = z.hi_rand;
          // Loop points: the zone's, else the file's own markers, else the whole zone.
          const int la = z.loop_start >= 0 ? frame(z.loop_start, 0) : d->loop_start >= 0 ? d->loop_start : sz.begin;
          const int lb = z.loop_end >= 0 ? frame(z.loop_end, 0) + 1 : d->loop_end >= 0 ? d->loop_end : sz.end;
          sz.loop_a = std::max(sz.begin, std::min(sz.end - 1, la));
          sz.loop_b = std::max(sz.loop_a + 1, std::min(sz.end, lb));
          // The crossfade reads as far before the loop's start as it is long, so it fits in both.
          const int xf = z.loop_crossfade > 0 ? int(std::lround(z.loop_crossfade * scale))
                                              : int(std::lround(z.loop_crossfade_s * 48000.0f));
          sz.xfade = std::max(0, std::min({xf, sz.loop_b - sz.loop_a, sz.loop_a}));
          sz.xfade_linear = z.crossfade_linear;
          sz.seq = z.seq, sz.seq_group = z.seq_group, sz.seq_length = z.seq_length, sz.seq_position = z.seq_position;
          st.zones.push_back(sz);
        }
    }
    st.level = t.level;
    st.pan = t.pan;
    st.send_chorus = t.send_chorus;
    st.send_delay = t.send_delay;
    st.send_reverb = t.send_reverb;
    for (int m = 0; m < kModulators; ++m) st.mods[m] = t.mods[m];
    st.midi_channel = t.midi_channel;
    st.midi_cc[0] = t.midi_cc[0];
    st.midi_cc[1] = t.midi_cc[1];
#if TWILL_PLUGINS
    if (plugins && t.instrument == InstrumentType::Plugin) st.plugin = plugins(t);
#endif
    st.audible = !t.mute && (!any_solo || t.solo);
    for (const Placement& pl : t.arrangement) {
      const SnapClip* c = snap_clip(pl.clip_id);
      if (!c || pl.length <= 0) continue;
      st.placements.push_back(SnapPlacement{pl.start, pl.length, pl.offset, c});
    }
    std::sort(st.placements.begin(), st.placements.end(),
              [](const SnapPlacement& a, const SnapPlacement& b) { return a.start < b.start; });
    s->tracks.push_back(std::move(st));
  }
  return s;
}

}  // namespace twill
