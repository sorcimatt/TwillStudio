#include "model/MidiFile.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace twill {
namespace {

void put_u16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  put_u16(out, v >> 16);
  put_u16(out, v & 0xFFFF);
}

// A variable-length quantity: 7 bits a byte, most significant first, the high bit on all but the last.
void put_vlq(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t bytes[5];
  int n = 0;
  do {
    bytes[n++] = uint8_t(v & 0x7F);
    v >>= 7;
  } while (v && n < 5);
  while (n-- > 0) out.push_back(uint8_t(bytes[n] | (n ? 0x80 : 0)));
}

void put_meta(std::vector<uint8_t>& out, uint8_t type, const std::string& data) {
  put_vlq(out, 0);
  out.push_back(0xFF);
  out.push_back(type);
  put_vlq(out, uint32_t(data.size()));
  out.insert(out.end(), data.begin(), data.end());
}

void put_chunk(std::vector<uint8_t>& out, const char* id, const std::vector<uint8_t>& body) {
  out.insert(out.end(), id, id + 4);
  put_u32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
}

void end_of_track(std::vector<uint8_t>& out, uint32_t delta = 0) {
  put_vlq(out, delta);
  out.push_back(0xFF);
  out.push_back(0x2F);
  out.push_back(0x00);
}

}  // namespace

std::vector<SongNote> song_notes(const Project& p, int track) {
  std::vector<SongNote> out;
  if (track < 0 || track >= int(p.tracks.size())) return out;
  for (const Placement& pl : p.tracks[size_t(track)].arrangement) {
    const Clip* clip = p.find_clip(pl.clip_id);
    if (!clip || clip->length <= 0 || pl.length <= 0) continue;
    const Tick end = pl.start + pl.length;
    // The clip loops from `offset` into it: a note at clip time s plays at
    // pl.start - pl.offset + s + k * length, for each k that lands inside the placement.
    for (const Note& n : clip->notes) {
      int64_t at = int64_t(pl.start) - pl.offset + n.start;
      if (at < pl.start) at += (int64_t(pl.start) - at + clip->length - 1) / clip->length * clip->length;
      for (; at < end; at += clip->length) {
        SongNote s;
        s.start = Tick(at);
        s.length = std::max<Tick>(1, std::min<Tick>(n.length, Tick(end - at)));
        s.pitch = n.pitch;
        s.velocity = n.velocity;
        out.push_back(s);
      }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const SongNote& a, const SongNote& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
  return out;
}

int midi_file_channel(const Project& p, int track) {
  const Track& t = p.tracks[size_t(track)];
  if (t.instrument == InstrumentType::MidiOut) return t.midi_channel & 15;
  if (t.instrument == InstrumentType::Sampler && t.sampler.mode == SamplerMode::Kit) return 9;
  const int ch = track % 15;  // 0-8, then 10-15: 9 is the drums' channel
  return ch < 9 ? ch : ch + 1;
}

std::vector<uint8_t> make_midi_file(const Project& p, int max_tracks) {
  struct Event {
    uint32_t tick;
    uint8_t status, data1, data2;
  };
  std::vector<std::vector<uint8_t>> tracks;
  const int n = std::min(int(p.tracks.size()), max_tracks);
  for (int i = 0; i < n; ++i) {
    const std::vector<SongNote> notes = song_notes(p, i);
    if (notes.empty()) continue;
    const uint8_t ch = uint8_t(midi_file_channel(p, i));
    std::vector<Event> events;
    for (const SongNote& s : notes) {
      const uint8_t vel = uint8_t(std::max(1, std::min(127, int(s.velocity))));
      events.push_back({uint32_t(s.start), uint8_t(0x90 | ch), s.pitch, vel});
      events.push_back({uint32_t(s.start + s.length), uint8_t(0x80 | ch), s.pitch, 0});
    }
    // In time order; at the same tick, note-offs first, so a note ending where the same pitch
    // starts again doesn't cut the new one short.
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
      if (a.tick != b.tick) return a.tick < b.tick;
      return (a.status & 0xF0) == 0x80 && (b.status & 0xF0) == 0x90;
    });
    std::vector<uint8_t> body;
    put_meta(body, 0x03, p.tracks[size_t(i)].name);  // track name
    uint32_t now = 0;
    for (const Event& e : events) {
      put_vlq(body, e.tick - now);
      now = e.tick;
      body.push_back(e.status);
      body.push_back(e.data1);
      body.push_back(e.data2);
    }
    end_of_track(body);
    tracks.push_back(std::move(body));
  }
  if (tracks.empty()) return {};

  // The conductor track: the song's name, tempo and time signature.
  std::vector<uint8_t> conductor;
  put_meta(conductor, 0x03, p.name);
  const uint32_t us_per_quarter = uint32_t(std::lround(60000000.0 / std::max(1.0f, p.tempo)));
  put_meta(conductor, 0x51, std::string{char(us_per_quarter >> 16), char(us_per_quarter >> 8), char(us_per_quarter)});
  int den_pow = 0;
  while ((1 << den_pow) < p.time_sig_den && den_pow < 7) ++den_pow;
  put_meta(conductor, 0x58, std::string{char(p.time_sig_num), char(den_pow), char(24), char(8)});
  end_of_track(conductor);

  std::vector<uint8_t> out;
  std::vector<uint8_t> header;
  put_u16(header, 1);                              // format 1: tracks played together
  put_u16(header, uint32_t(tracks.size() + 1));    // the conductor, then the parts
  put_u16(header, uint32_t(kTicksPerQuarter));     // ticks a quarter, as Twill's own
  put_chunk(out, "MThd", header);
  put_chunk(out, "MTrk", conductor);
  for (const auto& t : tracks) put_chunk(out, "MTrk", t);
  return out;
}

}  // namespace twill
