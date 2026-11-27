#include "app/ArrangementView.h"

#include <algorithm>
#include <cstdio>

#include "app/Paper.h"
#include "engine/PlaitsVoice.h"

namespace twill {

namespace {
// Sizes, in layout units (x2 on the Brick Pro). Tuned for reading on a handheld at arm's length.
constexpr float kRulerH = 22, kHeaderW = 128, kRowH = 46, kBarW = 64;
constexpr float kNameSize = 20, kSmallSize = 12, kRulerText = 14;
constexpr float kNameBaseline = 21, kEngineBaseline = 37;  // from the top of the row
}  // namespace

ArrangementView::Layout ArrangementView::layout() const {
  const Rect c = app_.content_rect();
  Layout l;
  const int ruler_h = int(app_.u(kRulerH)), header_w = int(app_.u(kHeaderW));
  l.ruler = Rect{c.x + header_w, c.y, c.w - header_w, ruler_h};
  l.headers = Rect{c.x, c.y + ruler_h, header_w, c.h - ruler_h};
  l.grid = Rect{c.x + header_w, c.y + ruler_h, c.w - header_w, c.h - ruler_h};
  l.row_h = int(app_.u(kRowH));
  l.bar_w = std::max(4, int(app_.u(kBarW)) / bars_per_cell_);
  return l;
}

int ArrangementView::bar_x(const Layout& l, double bar) const {
  return l.grid.x + int((bar - scroll_bar_) * l.bar_w);
}

int ArrangementView::placement_at(int track, int bar) const {
  const auto& arr = app_.project().tracks[size_t(track)].arrangement;
  const Tick t = bar * kBar;
  for (size_t i = 0; i < arr.size(); ++i)
    if (arr[i].start <= t && t < arr[i].start + arr[i].length) return int(i);
  return -1;
}

void ArrangementView::copy() {
  const int track = app_.track();
  const int pi = cursor_bar_ >= 0 ? placement_at(track, cursor_bar_) : -1;
  const Clip* c = pi >= 0 ? app_.project().find_clip(app_.project().tracks[size_t(track)].arrangement[size_t(pi)].clip_id)
                          : nullptr;
  if (!c) {
    app_.message("Nothing to copy: put the cursor on a clip");
    return;
  }
  app_.clip_clipboard() = *c;
  app_.message("Copied the clip");
}

void ArrangementView::paste() {
  const Clip& board = app_.clip_clipboard();
  if (board.id == 0) {
    app_.message("Nothing to paste: copy a clip first (Select + Y)");
    return;
  }
  const int track = app_.track();
  if (cursor_bar_ < 0) return;
  const Tick at = cursor_bar_ * kBar;
  for (const Placement& o : app_.project().tracks[size_t(track)].arrangement)
    if (at < o.start + o.length && o.start < at + board.length) {
      app_.message("No room here for the clip");
      return;
    }
  EditHistory& h = app_.history();
  h.begin_group();
  const uint32_t id = h.add_clip(track, board.length, at);
  if (id) {
    Clip copy = board;  // its notes, lanes and name, as a new clip on this track
    copy.id = id;
    copy.track = track;
    h.change_clip(copy);
  }
  h.end_group();
  app_.message(id ? "Pasted the clip" : "Couldn't paste here");
}

void ArrangementView::clamp_scroll(const Layout& l) {
  const int visible_bars = std::max(1, l.grid.w / l.bar_w);
  if (cursor_bar_ >= 0 && cursor_bar_ < scroll_bar_) scroll_bar_ = cursor_bar_;
  if (cursor_bar_ >= scroll_bar_ + visible_bars) scroll_bar_ = cursor_bar_ - visible_bars + 1;
  const int visible_tracks = std::max(1, l.grid.h / l.row_h);
  const int t = app_.track();
  if (t < top_track_) top_track_ = t;
  if (t >= top_track_ + visible_tracks) top_track_ = t - visible_tracks + 1;
}

void ArrangementView::open_track_settings(int track, int select_row) {
  App& app = app_;
  std::vector<InspectorField> fields;
  const InstrumentType instrument = app.project().tracks[size_t(track)].instrument;
  // Name: Left/Right step through part names, A spells one out.
  fields.push_back({"Name", [&app, track] { return app.project().tracks[size_t(track)].name; },
                    [&app, track](int step, bool) { app.step_track_name(track, step); },
                    [&app, track] { app.edit_track_name(track); }});
  fields.push_back({"Engine", [&app, track] { return App::sound_label(app.project().tracks[size_t(track)]); },
                    [this, &app, track, instrument](int step, bool) {
                      app.step_sound(track, step);
                      // Past the synth engines to the sampler (or back): its settings differ.
                      if (app.project().tracks[size_t(track)].instrument != instrument) open_track_settings(track, 1);
                    }});
  if (instrument != InstrumentType::Macro)
    fields.push_back({"Sound settings", [] { return std::string("press A"); }, nullptr, [&app] {
                        app.close_inspector();
                        app.show(App::View::Instrument);
                      }});
  for (const ParamInfo& info : macro_params()) {
    if (instrument != InstrumentType::Macro) break;
    if (info.id == kParamEngine) continue;
    // The low-pass gate's settings do nothing while the ADSR is the amplifier.
    if ((info.id == kParamDecay || info.id == kParamLpgColour) &&
        app.project().tracks[size_t(track)].tone.amp_mode == 1)
      continue;
    const ParamId id = info.id;
    const float span = info.max - info.min;
    fields.push_back({info.name,
                      [&app, track, id] {
                        char b[16];
                        std::snprintf(b, sizeof b, "%.2f", app.project().track_param(track, id));
                        return std::string(b);
                      },
                      [&app, track, id, span](int step, bool coarse) {
                        float v = app.project().track_param(track, id) + step * span * (coarse ? 0.1f : 0.01f);
                        app.history().set_track_param(track, id, v);
                      }});
  }
  auto flags = [&app, track](bool mute, bool solo, int voices) {
    app.history().set_track_flags(track, mute, solo, voices);
  };
  const Track& tr = app.project().tracks[size_t(track)];
  const bool kit = tr.instrument == InstrumentType::Sampler && tr.sampler.mode == SamplerMode::Kit;
  fields.push_back({kit ? "Voices per drum" : "Voices",
                    [&app, track] { return voices_label(app.project().tracks[size_t(track)].voices); },
                    [&app, track, flags](int step, bool) {
                      const Track& t = app.project().tracks[size_t(track)];
                      flags(t.mute, t.solo, t.voices + step);
                    }});
  fields.push_back({"Level",
                    [&app, track] {
                      char b[16];
                      std::snprintf(b, sizeof b, "%.0f%%", app.project().tracks[size_t(track)].level * 100);
                      return std::string(b);
                    },
                    [&app, track](int step, bool coarse) {
                      float v = app.project().tracks[size_t(track)].level + step * (coarse ? 0.1f : 0.01f);
                      app.history().set_track_param(track, kParamLevel, v);
                    }});
  fields.push_back({"Pan",
                    [&app, track] {
                      char b[16];
                      std::snprintf(b, sizeof b, "%+.2f", app.project().tracks[size_t(track)].pan);
                      return std::string(b);
                    },
                    [&app, track](int step, bool coarse) {
                      float v = app.project().tracks[size_t(track)].pan + step * (coarse ? 0.2f : 0.02f);
                      app.history().set_track_param(track, kParamPan, v);
                    }});
  fields.push_back({"Mute", [&app, track] { return std::string(app.project().tracks[size_t(track)].mute ? "On" : "Off"); },
                    [&app, track, flags](int, bool) {
                      const Track& t = app.project().tracks[size_t(track)];
                      flags(!t.mute, t.solo, t.voices);
                    }});
  fields.push_back({"Solo", [&app, track] { return std::string(app.project().tracks[size_t(track)].solo ? "On" : "Off"); },
                    [&app, track, flags](int, bool) {
                      const Track& t = app.project().tracks[size_t(track)];
                      flags(t.mute, !t.solo, t.voices);
                    }});
  // Where the track sits. Left/Right moves it one place, R + Left/Right to the top or the end.
  // The panel reopens for the track at its new place, on this row.
  const int position_row = int(fields.size());
  fields.push_back({"Position",
                    [&app, track] {
                      return std::to_string(track + 1) + " of " + std::to_string(app.project().tracks.size());
                    },
                    [this, &app, track, position_row](int step, bool coarse) {
                      const int n = int(app.project().tracks.size());
                      const int to = coarse ? (step > 0 ? n - 1 : 0) : std::max(0, std::min(n - 1, track + step));
                      if (to == track) return;
                      app.history().move_track(track, to);
                      app.set_track(to);
                      open_track_settings(to, position_row);  // last: this replaces the field
                    }});
  fields.push_back({"Clear track", [] { return std::string("press A"); }, nullptr,
                    [this, &app, track] {
                      const int row = app.inspector_selection();
                      app.history().clear_track(track);
                      open_track_settings(track, row);  // new name and sound; last, as above
                    }});
  // The way up to the main menu (Menu and Select + X open this panel in the sound view).
  fields.push_back({"Main menu", [] { return std::string("press A"); }, nullptr, [&app] { app.open_main_menu(); }});
  app.open_inspector(app.project().tracks[size_t(track)].name, std::move(fields));
  if (select_row > 0) app.select_inspector_row(select_row);
}

void ArrangementView::open_main_menu() {
  App& app = app_;
  std::vector<InspectorField> fields;
  // In sections, as a desktop app's menus run: the song's file (new, load, save, render) first,
  // then its music, MIDI, and last the app's own settings and help, which are rarely changed.
  auto heading = [&fields](const char* name) {
    InspectorField h{name, nullptr, nullptr};
    h.heading = true;
    fields.push_back(h);
  };
  heading("Song");
  app.add_song_fields(fields);
  app.add_render_fields(fields);
  heading("Music");
  fields.push_back({"Tempo",
                    [&app] {
                      char b[16];
                      std::snprintf(b, sizeof b, "%.0f BPM", app.project().tempo);
                      return std::string(b);
                    },
                    [&app](int step, bool coarse) { app.history().set_tempo(app.project().tempo + step * (coarse ? 10 : 1)); }});
  fields.push_back({"Key", [&app] { return std::string(key_name(app.project().key)); },
                    [&app](int step, bool) { app.history().set_key_scale(app.project().key + step + 12, app.project().scale); }});
  fields.push_back({"Scale", [&app] { return std::string(scale_name(app.project().scale)); },
                    [&app](int step, bool) {
                      const int n = int(ScaleType::kCount);
                      app.history().set_key_scale(app.project().key, ScaleType((int(app.project().scale) + step + n) % n));
                    }});
  heading("MIDI");
  app.add_midi_fields(fields);
  heading("Settings");
  fields.push_back({"Theme", [&app] { return std::string(app.dark_mode() ? "Dark" : "Light"); },
                    [&app](int, bool) { app.set_dark_mode(!app.dark_mode()); }});
  fields.push_back({"New song names", [&app] { return std::string(app.auto_names() ? "Made up" : "Untitled"); },
                    [&app](int, bool) { app.set_auto_names(!app.auto_names()); }});
  // Louder playback on a device that plays quietly; renders stay as mixed.
  fields.push_back({"Output boost",
                    [&app] { return app.output_boost_db() ? "+" + std::to_string(app.output_boost_db()) + " dB" : std::string("Off"); },
                    [&app](int step, bool) { app.set_output_boost_db(app.output_boost_db() + 3 * step); }});
  if (!app.platform().system_sets_audio_buffer())
    fields.push_back({"Audio buffer", [&app] { return app.audio_buffer_text(); },
                      [&app](int, bool) { app.set_safe_audio(!app.safe_audio()); }});
  heading("Help");
  fields.push_back({"Quick start", [] { return std::string("press A"); }, nullptr, [&app] { app.open_quick_start(); }});
  fields.push_back({"Controls", [] { return std::string("press A"); }, nullptr, [&app] { app.open_controls(); }});
  fields.push_back({"About", [] { return std::string("press A"); }, nullptr, [&app] { app.open_about(); }});
  if (app.platform().can_quit())
    fields.push_back({"Quit Twill", [] { return std::string("press A"); }, nullptr, [&app] { app.ask_quit(); }});
  app.open_inspector(App::kMainMenuTitle, std::move(fields), App::build_id());
}

void ArrangementView::handle_input() {
  const InputTracker& in = app_.input();
  Project& p = app_.project();
  EditHistory& h = app_.history();
  const int track = app_.track();
  const Layout l = layout();
  bool moved = false;

  // L/R held: zoom and page.
  if (in.held(kL) || in.held(kR)) {
    if (!in.held(kA) && !in.held(kB)) {
      if (in.pressed_or_repeat(kLeft)) bars_per_cell_ = std::min(4, bars_per_cell_ * 2);
      if (in.pressed_or_repeat(kRight)) bars_per_cell_ = std::max(1, bars_per_cell_ / 2);
      const int page = std::max(1, l.grid.h / l.row_h);
      if (in.pressed_or_repeat(kUp)) app_.set_track(track - page);
      if (in.pressed_or_repeat(kDown)) app_.set_track(track + page);
      if (in.pressed_or_repeat(kLeft | kRight | kUp | kDown)) {
        clamp_scroll(layout());
        app_.invalidate_all();
      }
      return;
    }
  }

  const int pi = cursor_bar_ >= 0 ? placement_at(track, cursor_bar_) : -1;
  std::vector<Placement> arr = p.tracks[size_t(track)].arrangement;

  // A: create or select; held: resize (or change engine on the header).
  if (in.pressed(kA)) {
    a_used_ = false;
    h.begin_group();
    if (cursor_bar_ >= 0 && pi < 0) {
      h.add_clip(track, kBar, cursor_bar_ * kBar);
      app_.message("New clip");
    }
  }
  if (in.held(kA)) {
    int step = in.pressed_or_repeat(kRight) ? 1 : in.pressed_or_repeat(kLeft) ? -1 : 0;
    if (step && cursor_bar_ < 0) {
      app_.step_sound(track, step);
      a_used_ = true;
    } else if (step && pi >= 0) {
      Placement& pl = arr[size_t(pi)];
      Tick len = std::max(kBar, pl.length + step * kBar);
      // Don't run into the next clip.
      for (const Placement& o : arr)
        if (o.start > pl.start) len = std::min(len, o.start - pl.start);
      pl.length = len;
      h.set_arrangement(track, arr);
      a_used_ = true;
    }
    // Y: no repeats. The clip's loop becomes as long as the placement plays it (its notes stay
    // where they are; a longer loop has empty bars at its end, a shorter one hides later notes).
    // Every placement of the clip plays the new loop.
    if (in.pressed(kY) && pi >= 0) {
      const Placement& pl = arr[size_t(pi)];
      if (const Clip* c = p.find_clip(pl.clip_id)) {
        Clip copy = *c;
        copy.length = pl.offset + pl.length;
        if (copy.length != c->length) h.change_clip(copy);
        char b[64];
        std::snprintf(b, sizeof b, "Loop %g bars: no repeats", double(copy.length) / kBar);
        app_.message(b);
      }
      a_used_ = true;
    }
  }
  if (in.released(kA)) {
    h.end_group();
    // A tapped on a track's name: its sound, in the Sound view.
    if (cursor_bar_ < 0 && !a_used_) {
      app_.show(App::View::Instrument);
      return;
    }
  }

  // B: delete; held: move along the track.
  if (in.pressed(kB)) {
    b_used_ = false;
    h.begin_group();
  }
  if (in.held(kB) && pi >= 0) {
    int step = in.pressed_or_repeat(kRight) ? 1 : in.pressed_or_repeat(kLeft) ? -1 : 0;
    if (step) {
      Placement moved_pl = arr[size_t(pi)];
      Tick start = moved_pl.start + step * kBar;
      bool clear = start >= 0;
      for (size_t i = 0; i < arr.size() && clear; ++i)
        if (int(i) != pi && start < arr[i].start + arr[i].length && arr[i].start < start + moved_pl.length)
          clear = false;
      if (clear) {
        arr[size_t(pi)].start = start;
        h.set_arrangement(track, arr);
        cursor_bar_ += step;
        moved = true;
      }
      b_used_ = true;
    }
  }
  if (in.released(kB)) {
    if (!b_used_ && pi >= 0) {
      arr.erase(arr.begin() + pi);
      h.set_arrangement(track, arr);
      app_.message_undoable("Clip removed");
    }
    h.end_group();
  }

  // Y: duplicate after the clip (the copy plays the same clip). With A held, see above.
  if (in.pressed(kY) && pi >= 0 && !in.held(kA)) {
    Placement copy = arr[size_t(pi)];
    copy.start += copy.length;
    bool clear = true;
    for (const Placement& o : arr)
      if (copy.start < o.start + o.length && o.start < copy.start + copy.length) clear = false;
    if (clear) {
      arr.push_back(copy);
      h.set_arrangement(track, arr);
      cursor_bar_ = copy.start / kBar;
      moved = true;
      app_.message("Duplicated (linked)");
    } else {
      app_.message("No room after this clip");
    }
  }

  // X: open the clip in the roll; the track's settings (on its header); the project's.
  if (in.pressed(kX)) {
    if (pi >= 0) {
      app_.open(track, arr[size_t(pi)]);
      app_.show(App::View::PianoRoll);
    } else if (cursor_bar_ < 0) {
      open_track_settings(track);
    } else {
      app_.open_main_menu();
    }
    return;
  }

  // Plain d-pad: move.
  if (!in.held(kA) && !in.held(kB)) {
    // Left and Right step a clip at a time, and an empty bar at a time between clips: whatever is
    // done on a clip is done to all of it, so the cursor never sits part way along one.
    auto bars_of = [&](int t, int bar, int& first, int& end) {
      const int i = bar >= 0 ? placement_at(t, bar) : -1;
      if (i < 0) return false;
      const Placement& pl = p.tracks[size_t(t)].arrangement[size_t(i)];
      first = int(pl.start / kBar);
      end = int((pl.start + pl.length + kBar - 1) / kBar);  // the first bar after it
      return true;
    };
    int first = 0, end = 0;
    if (in.pressed_or_repeat(kLeft)) {
      cursor_bar_ = bars_of(track, cursor_bar_, first, end) ? first - 1 : cursor_bar_ - 1;
      if (bars_of(track, cursor_bar_, first, end)) cursor_bar_ = first;  // onto the clip before
      cursor_bar_ = std::max(-1, cursor_bar_);
      moved = true;
    }
    if (in.pressed_or_repeat(kRight)) {
      cursor_bar_ = bars_of(track, cursor_bar_, first, end) ? end : cursor_bar_ + 1;
      moved = true;
    }
    if (in.pressed_or_repeat(kUp)) app_.set_track(track - 1), moved = true;
    if (in.pressed_or_repeat(kDown)) app_.set_track(track + 1), moved = true;
  }
  if (moved || in.pressed(~0u)) {
    // The roll follows the clip under the arrangement cursor.
    const int t = app_.track();
    const int cur = cursor_bar_ >= 0 ? placement_at(t, cursor_bar_) : -1;
    if (cur >= 0) {
      const Placement& pl = p.tracks[size_t(t)].arrangement[size_t(cur)];
      if (!(app_.open_placement() && *app_.open_placement() == pl)) app_.open(t, pl);
    }
    clamp_scroll(layout());
    app_.invalidate(app_.content_rect());
  }
}

void ArrangementView::tick() {
  const Layout l = layout();
  int x = -1;
  if (app_.engine().playing()) x = bar_x(l, double(app_.engine().position()) / kBar);
  if (x != last_playhead_x_) {
    const int w = int(app_.u(6)) + 2;  // the line, and the marker in the ruler (5 points each side)
    if (last_playhead_x_ >= 0) app_.invalidate(Rect{last_playhead_x_ - w, l.ruler.y, 2 * w + 1, l.ruler.h + l.grid.h});
    if (x >= 0) app_.invalidate(Rect{x - w, l.ruler.y, 2 * w + 1, l.ruler.h + l.grid.h});
    last_playhead_x_ = x;
  }
}

std::vector<Hint> ArrangementView::hints() const {
  const InputTracker& in = app_.input();
  if (in.held(kA)) return cursor_bar_ < 0 ? std::vector<Hint>{{"Left/Right", "engine"}}
                                          : std::vector<Hint>{{"Left/Right", "clip length"}, {"Y", "no repeat"}};
  if (in.held(kB)) return {{"Left/Right", "move clip"}, {"release", "delete"}};
  if (in.held(kL) || in.held(kR)) return {{"Left/Right", "zoom"}, {"Up/Down", "page"}, {"Start", "from cursor"}};
  if (cursor_bar_ < 0) return {{"A", "sound"}, {"A+Left/Right", "engine"}, {"X", "track settings"}, {"Select+Right", "roll"}};
  if (placement_at(app_.track(), cursor_bar_) < 0) return {{"A", "new clip"}, {"X", "menu"}, {"Start", "play"}};
  return {{"A", "select"}, {"B", "delete"}, {"Y", "duplicate"}, {"X", "open"}, {"Start", "play"}};
}

void ArrangementView::draw(Canvas& c) {
  const Theme& th = app_.theme();
  const Project& p = app_.project();
  const Layout l = layout();
  const Rect content = app_.content_rect();
  const Font& name_f = Font::get(FontFace::CondensedBold, app_.u(kNameSize));
  const Font& small = Font::get(FontFace::CondensedSemiBold, app_.u(kSmallSize));
  const Font& ruler_f = Font::get(FontFace::CondensedBold, app_.u(kRulerText));
  const float sp = app_.u(kCapsSpacing);
  const int line = std::max(1, int(app_.u(1)));
  const int ntracks = int(p.tracks.size());

  // Ruler, and the corner above the headers (a sheet like them).
  c.fill(Rect{content.x, content.y, l.headers.w, l.ruler.h}, th.panel);
  c.text(ruler_f, content.x + app_.u(10), ruler_f.baseline_in(l.ruler.y, l.ruler.h), "ARRANGE", th.text_secondary,
         app_.u(0.6f));
  c.fill(l.ruler, th.header);
  const int base = ruler_f.baseline_in(l.ruler.y, l.ruler.h);
  const int label_every = bars_per_cell_ >= 4 ? 4 : bars_per_cell_ >= 2 ? 2 : 1;
  for (int bar = scroll_bar_; bar_x(l, bar) < l.ruler.right(); ++bar) {
    int x = bar_x(l, bar);
    if (bar % label_every) continue;
    c.vline(x, l.ruler.y + int(app_.u(6)), l.ruler.bottom(), th.grid_bar);
    c.text(ruler_f, x + app_.u(4), base, std::to_string(bar + 1).c_str(), th.text);
  }
  c.fill(Rect{content.x, l.ruler.bottom() - line, content.w, line}, th.text);

  // Rows.
  const Tick loop_end = std::max(kBar, p.song_end());
  for (int row = 0; ; ++row) {
    const int t = top_track_ + row;
    const int y = l.grid.y + row * l.row_h;
    if (y >= l.grid.bottom()) break;
    Rect header{l.headers.x, y, l.headers.w, l.row_h};
    Rect lane{l.grid.x, y, l.grid.w, l.row_h};
    if (t >= ntracks) {
      c.fill(Rect{l.headers.x, y, l.headers.w, l.grid.bottom() - y}, th.panel);
      c.fill(Rect{l.grid.x, y, l.grid.w, l.grid.bottom() - y}, th.background);
      break;
    }
    const Track& tr = p.tracks[size_t(t)];
    const Color col = app_.track_colour(t);

    // Header: colour swatch, number, name, engine, mute/solo.
    c.fill(header, t == app_.track() ? th.chip : th.panel);
    const int sw = int(app_.u(9));
    c.fill(Rect{header.x + int(app_.u(8)), y + int(app_.u(10)), sw, sw}, col);
    char num[8];
    std::snprintf(num, sizeof num, "%02d", t + 1);
    c.text(small, header.x + app_.u(22), y + int(app_.u(19)), num, th.text_secondary);
    std::string name = caps(tr.name);
    const char* badge = tr.solo ? "S" : tr.mute ? "M" : "";
    const float name_x = header.x + app_.u(38);
    const float name_room = header.right() - app_.u(8) - name_x - (badge[0] ? name_f.measure(badge) + app_.u(6) : 0);
    while (name.size() > 1 && name_f.measure(name.c_str()) > name_room) name.pop_back();
    const int nb = y + int(app_.u(kNameBaseline));
    c.text(name_f, name_x, nb, name.c_str(), tr.mute ? th.text_secondary : th.text);
    if (badge[0]) c.text(name_f, header.right() - app_.u(8) - name_f.measure(badge), nb, badge, tr.solo ? th.cursor : th.text_secondary);
    std::string engine = caps(app_.sound_summary(tr));
    while (engine.size() > 1 && small.measure(engine.c_str(), sp) > header.right() - app_.u(6) - name_x) engine.pop_back();
    c.text(small, name_x, y + int(app_.u(kEngineBaseline)), engine.c_str(), th.text_secondary, sp);
    c.hline(header.x, header.right(), y + l.row_h - 1, th.divider);

    // Lane: bar lines, stronger every four bars, and beat lines when the bars are wide.
    c.fill(lane, th.row_white);
    for (int bar = scroll_bar_; bar_x(l, bar) < lane.right(); ++bar) {
      const int x = bar_x(l, bar);
      if (bars_per_cell_ == 1)
        for (int q = 1; q < 4; ++q) c.vline(x + l.bar_w * q / 4, y, y + l.row_h, th.grid_step);
      if (bars_per_cell_ == 1 || bar % 4 == 0) c.vline(x, y, y + l.row_h, bar % 4 == 0 ? th.grid_bar : th.grid_beat);
    }
    c.hline(lane.x, lane.right(), y + l.row_h - 1, th.grid_step);

    // Clips, cut off at the lane's edges when they start before the scrolled view: a pale fill of
    // the track's colour, the notes in a deep shade of it.
    const Color fill = th.track_fill(int(tr.colour)), ink = th.track_ink(int(tr.colour));
    const Rect saved_clip = c.clip();
    c.set_clip(saved_clip.intersect(lane));
    for (const Placement& pl : tr.arrangement) {
      int x0 = bar_x(l, double(pl.start) / kBar), x1 = bar_x(l, double(pl.start + pl.length) / kBar);
      if (x1 < lane.x || x0 > lane.right()) continue;
      Rect block{x0 + 1, y + int(app_.u(4)), x1 - x0 - 2, l.row_h - int(app_.u(8))};
      c.fill(block, fill);
      const Clip* clip = p.find_clip(pl.clip_id);
      if (clip && clip->length > 0) {
        // A miniature of the notes, the clip repeating across the placement.
        int lo = 127, hi = 0;
        for (const Note& n : clip->notes) lo = std::min(lo, int(n.pitch)), hi = std::max(hi, int(n.pitch));
        const Rect inner = block.inset(int(app_.u(5)));
        const double px_per_tick = double(block.w) / pl.length;
        for (Tick rep = -pl.offset; rep < pl.length; rep += clip->length)
          for (const Note& n : clip->notes) {
            Tick s = rep + n.start;
            if (s < 0 || s >= pl.length) continue;
            int nx = block.x + int(s * px_per_tick);
            int nw = std::max(1, int(std::min<Tick>(n.length, pl.length - s) * px_per_tick) - 1);
            int ny = hi == lo ? inner.y + inner.h / 2
                              : inner.bottom() - 1 - (n.pitch - lo) * (inner.h - 2) / std::max(1, hi - lo);
            c.fill(Rect{nx, ny, nw, std::max(1, int(app_.u(2.5f)))}, ink);
          }
        // Clips placed more than once are linked: editing one changes all.
        int uses = 0;
        for (const Track& ot : p.tracks)
          for (const Placement& op : ot.arrangement) uses += op.clip_id == pl.clip_id;
        if (uses > 1 && block.w > app_.u(24)) {
          char b[8];
          std::snprintf(b, sizeof b, "x%d", uses);
          c.text(small, block.right() - small.measure(b) - app_.u(4), block.y + small.cap_height() + int(app_.u(3)), b, ink);
        }
      }
    }
    c.set_clip(saved_clip);
  }
  // Song end marker.
  int ex = bar_x(l, double(loop_end) / kBar);
  if (ex > l.grid.x && ex < l.grid.right()) c.vline(ex, l.grid.y, l.grid.bottom(), th.grid_bar);

  // The headers lie on the lanes like a sheet: a soft shadow over the lanes' edge.
  c.vline(l.headers.right() - 1, content.y, l.headers.bottom(), th.grid_bar);
  draw_shadow(c, l.headers.right(), l.grid.y, l.grid.h, int(app_.u(6)), false, th.shadow_strength * 3 / 4);

  // Cursor: viewfinder corners around the clip, the empty bar or the header.
  const int crow = app_.track() - top_track_;
  if (crow >= 0 && l.grid.y + crow * l.row_h < l.grid.bottom()) {
    const int y = l.grid.y + crow * l.row_h;
    Rect cur = cursor_bar_ < 0 ? Rect{l.headers.x + int(app_.u(4)), y + int(app_.u(4)), l.headers.w - int(app_.u(8)),
                                      l.row_h - int(app_.u(8))}
                               : Rect{bar_x(l, cursor_bar_) + 1, y + int(app_.u(4)), l.bar_w - 2, l.row_h - int(app_.u(8))};
    const int pi = cursor_bar_ >= 0 ? placement_at(app_.track(), cursor_bar_) : -1;
    if (pi >= 0) {
      const Placement& pl = p.tracks[size_t(app_.track())].arrangement[size_t(pi)];
      int x0 = bar_x(l, double(pl.start) / kBar), x1 = bar_x(l, double(pl.start + pl.length) / kBar);
      cur = Rect{x0 + 1, cur.y, x1 - x0 - 2, cur.h};
    }
    // A clip that starts before the view: its corners stop at the grid's edge.
    const Rect saved_clip = c.clip();
    if (cursor_bar_ >= 0) c.set_clip(saved_clip.intersect(Rect{l.grid.x, content.y, l.grid.w, content.h}));
    draw_brackets(c, cur, th.text, app_.u(1));
    c.set_clip(saved_clip);
  }

  // Playhead: an ink line with a marker in the ruler.
  if (app_.engine().playing()) {
    int x = bar_x(l, double(app_.engine().position()) / kBar);
    if (x >= l.grid.x && x < l.grid.right()) {
      c.fill(Rect{x, l.ruler.y + int(app_.u(2)), line, l.ruler.h + l.grid.h - int(app_.u(2))}, th.playhead_line);
      draw_marker(c, x, l.ruler.y + int(app_.u(15)), int(app_.u(5)), th.playhead_line);
    }
  }
}

}  // namespace twill
