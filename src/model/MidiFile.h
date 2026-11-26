// The song as a Standard MIDI File, for a DAW (Ableton, Cubase...): the notes as the arrangement
// plays them, so the parts can be edited, rearranged or played with other instruments there.
#pragma once
#include <cstdint>
#include <vector>

#include "model/Project.h"

namespace twill {

// One note as the song plays it: its start in song ticks, its length (cut at the end of the
// placement it's in), pitch and velocity.
struct SongNote {
  Tick start = 0, length = 0;
  uint8_t pitch = 60, velocity = 100;
};

// Every note a track plays in the arrangement, clips looped as they play, sorted by start. Notes
// with a probability below 100% are all in (the file can't say "sometimes").
std::vector<SongNote> song_notes(const Project& p, int track);

// The MIDI channel (0-15) a track's notes go on in the file: a MIDI Out track's own channel, 10
// (9 here) for a drum kit, otherwise one per track in order, skipping 10.
int midi_file_channel(const Project& p, int track);

// A type-1 Standard MIDI File at Twill's own 960 ticks a quarter: a first track with the song's
// name, tempo and time signature, then one track per Twill track (up to `max_tracks`) that has
// notes in the arrangement, named after it. Empty if no track has any.
std::vector<uint8_t> make_midi_file(const Project& p, int max_tracks = kMaxTracks);

}  // namespace twill
