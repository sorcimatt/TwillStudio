#include "app/PianoRollView.h"
#include "app/Paper.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "app/PianoKeys.h"

namespace twill {

namespace {

struct Snap {
  Tick ticks;
  const char* name;
};
// Coarse to fine. Zooming steps through these.
const Snap kSnaps[] = {{3840, "1 bar"}, {1920, "1/2"},  {960, "1/4"},   {640, "1/4 T"},
                       {480, "1/8"},    {320, "1/8 T"}, {240, "1/16"},  {160, "1/16 T"},
                       {120, "1/32"},   {60, "1/64"}};
constexpr int kNumSnaps = int(sizeof kSnaps / sizeof kSnaps[0]);
constexpr Tick kBar = 4 * kTicksPerQuarter;
constexpr Tick kSixteenth = kTicksPerQuarter / 4;

// " +30 ticks" for whatever is left below a sixteenth, else nothing.
std::string tick_rest(Tick t) {
  const Tick rest = t % kSixteenth;
  return rest ? " +" + std::to_string(rest) + " ticks" : std::string();
}

// A position in a clip: bar.beat.sixteenth, all from 1, then any ticks off the sixteenth.
std::string format_position(Tick t) {
  char b[32];
  std::snprintf(b, sizeof b, "%d.%d.%d", int(t / kBar) + 1, int(t % kBar / kTicksPerQuarter) + 1,
                int(t % kTicksPerQuarter / kSixteenth) + 1);
  return b + tick_rest(t);
}

// A length in sixteenths, then any ticks over.
std::string format_length(Tick t) { return std::to_string(t / kSixteenth) + "/16" + tick_rest(t); }

// To the nearest multiple of `grid`.
Tick round_to(Tick t, Tick grid) { return (t + grid / 2) / grid * grid; }

// Snaps a note's start and length to the grid, keeping it inside the clip.
void snap_note(Note& n, Tick grid, Tick clip_length) {
  n.start = std::min(round_to(n.start, grid), std::max<Tick>(0, (clip_length - 1) / grid * grid));
  n.length = std::max(grid, round_to(n.length, grid));
  n.length = std::min(n.length, clip_length - n.start);
}

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
std::string note_name(int pitch) { return std::string(kNoteNames[pitch % 12]) + std::to_string(pitch / 12 - 1); }

bool contains(const std::vector<uint32_t>& v, uint32_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

}  // namespace

Tick PianoRollView::snap() const { return kSnaps[snap_index_].ticks; }

PianoRollView::Layout PianoRollView::layout() const {
  const Rect c = app_.content_rect();
  Layout l;
  const Track& tr = app_.project().tracks[size_t(app_.track())];
  const bool kit = tr.instrument == InstrumentType::Sampler && tr.sampler.mode == SamplerMode::Kit;
  const int ruler_h = int(app_.u(22)), keys_w = int(app_.u(kit ? 92 : 52));
  const int lane_h = lane_ >= 0 ? int(app_.u(70)) : 0;
  l.ruler = Rect{c.x + keys_w, c.y, c.w - keys_w, ruler_h};
  l.keys = Rect{c.x, c.y + ruler_h, keys_w, c.h - ruler_h - lane_h};
  l.grid = Rect{c.x + keys_w, c.y + ruler_h, c.w - keys_w, c.h - ruler_h - lane_h};
  if (lane_h) {
    l.lane = Rect{l.grid.x, l.grid.bottom(), l.grid.w, lane_h};
    l.lane_label = Rect{c.x, l.grid.bottom(), keys_w, lane_h};
  }
  l.row_h = int(app_.u(kit ? 16 : 13));  // a kit's rows are taller, for its names
  l.cell_w = int(app_.u(22));
  return l;
}

int PianoRollView::tick_x(const Layout& l, Tick t) const {
  return l.grid.x + int(int64_t(t - scroll_) * l.cell_w / snap());
}

namespace {
bool kit_track(const Track& t) {
  return t.instrument == InstrumentType::Sampler && t.sampler.mode == SamplerMode::Kit;
}
std::string slot_label(const Track& t, int pitch) {
  const int i = pitch - kKitFirstNote;
  if (i < 0 || i >= kKitSlots || t.sampler.kit[i].sample.empty()) return std::to_string(i + 1);
  std::string name = t.sampler.kit[i].sample;
  name = name.substr(name.find_last_of('/') + 1);
  name = name.substr(0, name.find_last_of('.'));
  // Factory names start with a number ("05 Hat closed"); the row shows the name.
  if (name.size() > 3 && std::isdigit(static_cast<unsigned char>(name[0])) && name[2] == ' ') name = name.substr(3);
  return name;
}
}  // namespace

void PianoRollView::rebuild_rows() {
  const Project& p = app_.project();
  const Clip* clip = app_.open_clip();
  rows_.clear();
  // Kits: one row per slot (drum mode), first slot at the bottom.
  if (kit_track(p.tracks[size_t(app_.track())])) {
    for (int pitch = kKitFirstNote + kKitSlots - 1; pitch >= kKitFirstNote; --pitch) rows_.push_back(pitch);
    return;
  }
  for (int pitch = 127; pitch >= 0; --pitch) {
    bool keep = !fold_ || in_scale(pitch, p.key, p.scale);
    if (!keep && clip)
      for (const Note& n : clip->notes)
        if (n.pitch == pitch) keep = true;
    if (keep) rows_.push_back(pitch);
  }
}

int PianoRollView::row_of(int pitch) const {
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i] == pitch) return int(i);
  return -1;
}

int PianoRollView::shifted_pitch(int pitch, int rows, bool octave) const {
  if (octave) return std::max(0, std::min(127, pitch + 12 * rows));
  int r = row_of(pitch);
  if (r < 0) {  // not on a row (folded away): find the nearest row below
    r = 0;
    while (r < int(rows_.size()) - 1 && rows_[size_t(r)] > pitch) ++r;
  }
  r = std::max(0, std::min(int(rows_.size()) - 1, r - rows));  // up the screen is a lower index
  return rows_[size_t(r)];
}

const Note* PianoRollView::note_under_cursor() const {
  const Clip* clip = app_.open_clip();
  if (!clip) return nullptr;
  // Any note overlapping the cursor's grid cell (notes nudged off the grid still count), preferring
  // the one that starts latest within or before the cell.
  const Tick cell_end = cursor_ + snap();
  const Note* best = nullptr;
  for (const Note& n : clip->notes)
    if (n.pitch == cursor_pitch_ && n.start < cell_end && cursor_ < n.start + n.length)
      if (!best || n.start > best->start) best = &n;
  return best;
}

std::vector<uint32_t> PianoRollView::targets() {
  const Clip* clip = app_.open_clip();
  if (!clip) return {};
  std::vector<uint32_t> sel;
  for (uint32_t id : selection_)
    if (clip->find(id)) sel.push_back(id);
  selection_ = sel;
  const Note* n = note_under_cursor();
  if (!sel.empty() && (!n || contains(sel, n->id))) return sel;
  if (n) return {n->id};
  return {};
}

void PianoRollView::keep_cursor_visible(const Layout& l) {
  const Tick s = snap();
  const Tick visible = std::max<Tick>(s, Tick(int64_t(l.grid.w / l.cell_w) * s));
  if (cursor_ < scroll_) scroll_ = cursor_ / s * s;
  if (cursor_ + s > scroll_ + visible) scroll_ = (cursor_ + s - visible + s - 1) / s * s;
  if (scroll_ < 0) scroll_ = 0;
  const int visible_rows = std::max(1, l.grid.h / l.row_h);
  int r = row_of(cursor_pitch_);
  if (r < 0) {
    cursor_pitch_ = shifted_pitch(cursor_pitch_, 0, false);
    r = row_of(cursor_pitch_);
  }
  if (r < top_row_) top_row_ = r;
  if (r >= top_row_ + visible_rows) top_row_ = r - visible_rows + 1;
  top_row_ = std::max(0, std::min(top_row_, int(rows_.size()) - visible_rows));
}

void PianoRollView::set_lane_focus(bool on) {
  if (on && lane_ < 0) lane_ = 0;  // show the velocity lane if none is shown
  lane_focus_ = on;
  app_.invalidate(app_.content_rect());
}

std::string PianoRollView::lane_name() const {
  if (lane_ == 0) return "Velocity";
  return param_name(app_.project().tracks[size_t(app_.track())], ParamId(lane_));
}

void PianoRollView::lane_range(float& lo, float& hi) const {
  if (lane_ == 0) {
    lo = 1, hi = 127;
    return;
  }
  const ParamInfo* info = param_info(ParamId(lane_));
  lo = info ? info->min : 0;
  hi = info ? info->max : 1;
}

void PianoRollView::lane_input() {
  const InputTracker& in = app_.input();
  EditHistory& h = app_.history();
  Clip* clip = app_.open_clip();
  if (!clip) return;
  const Tick s = snap();
  const Layout l = layout();
  bool changed = false;
  const Tick last = std::max<Tick>(0, (clip->length - 1) / s * s);
  if (in.pressed_or_repeat(kLeft)) cursor_ = std::max<Tick>(0, cursor_ - s), changed = true;
  if (in.pressed_or_repeat(kRight)) cursor_ = std::min(last, cursor_ + s), changed = true;

  const int step = in.pressed_or_repeat(kUp) ? 1 : in.pressed_or_repeat(kDown) ? -1 : 0;
  // A run of up/down presses is one undo step.
  if ((in.pressed(kUp) || in.pressed(kDown)) && !lane_group_open_) {
    h.begin_group();
    lane_group_open_ = true;
  }
  if (lane_group_open_ && !in.held(kUp) && !in.held(kDown)) {
    h.end_group();
    lane_group_open_ = false;
  }
  const bool coarse = in.held(kR);
  if (lane_ == 0) {
    // Velocity of the notes that start in the cursor's cell.
    if (step || in.pressed(kB))
      for (const Note& n : std::vector<Note>(clip->notes))
        if (n.start >= cursor_ && n.start < cursor_ + s) {
          Note copy = n;
          copy.velocity = in.pressed(kB) ? 100 : uint8_t(std::max(1, std::min(127, n.velocity + step * (coarse ? 16 : 4))));
          h.change_note(clip->id, copy);
          changed = true;
        }
  } else if (step || in.pressed(kB)) {
    const ParamId id = ParamId(lane_);
    float lo, hi;
    lane_range(lo, hi);
    Clip copy = *clip;
    Lane* lane = copy.lane(id);
    if (!lane && step) {
      if (copy.lanes.size() >= size_t(kMaxLanesPerClip)) {
        app_.message("Up to 8 lanes per clip");
        return;
      }
      copy.lanes.push_back(Lane{id, {}});
      lane = &copy.lanes.back();
    }
    if (lane) {
      if (in.pressed(kB)) {
        lane->remove_point(cursor_);
        if (lane->points.empty()) copy.lanes.erase(copy.lanes.begin() + (lane - copy.lanes.data()));
      } else {
        const float current = lane->points.empty() ? app_.project().track_param(app_.track(), id) : lane->value_at(cursor_);
        const float next = step_track_param(app_.project().tracks[size_t(app_.track())], id, current, step, coarse, 0.02f);
        lane->set_point(cursor_, std::max(lo, std::min(hi, next)));
      }
      h.change_clip(copy);
      changed = true;
    }
  }
  if (changed || in.pressed(~0u)) {
    keep_cursor_visible(l);
    app_.invalidate(app_.content_rect());
  }
}

Tick PianoRollView::cursor_song_position() const {
  const Placement* pl = app_.open_placement();
  if (!pl) return 0;
  return std::max<Tick>(pl->start, pl->start + cursor_ - pl->offset);
}

void PianoRollView::select_all() {
  const Clip* clip = app_.open_clip();
  if (!clip) return;
  selection_.clear();
  for (const Note& n : clip->notes) selection_.push_back(n.id);
  app_.message(clip->notes.empty() ? "No notes to select"
                                   : clip->notes.size() == 1 ? "1 note selected"
                                                             : std::to_string(clip->notes.size()) + " notes selected");
  app_.invalidate(app_.content_rect());
}

void PianoRollView::copy() {
  const Clip* clip = app_.open_clip();
  const std::vector<uint32_t> ids = targets();
  if (!clip || ids.empty()) {
    app_.message("Nothing to copy: select notes, or put the cursor on one");
    return;
  }
  std::vector<Note>& board = app_.note_clipboard();
  board.clear();
  Tick first = clip->length;
  for (uint32_t id : ids)
    if (const Note* n = clip->find(id)) {
      board.push_back(*n);
      first = std::min(first, n->start);
    }
  for (Note& n : board) n.start -= first;
  app_.message(board.size() == 1 ? "Copied 1 note" : "Copied " + std::to_string(board.size()) + " notes");
}

void PianoRollView::paste() {
  const Clip* clip = app_.open_clip();
  const std::vector<Note>& board = app_.note_clipboard();
  if (!clip) return;
  if (board.empty()) {
    app_.message("Nothing to paste: copy notes first (Select + Y)");
    return;
  }
  const uint32_t cid = clip->id;
  const Tick clip_len = clip->length;
  EditHistory& h = app_.history();
  h.begin_group();
  std::vector<uint32_t> pasted;
  int missed = 0;
  for (Note n : board) {
    n.start += cursor_;
    if (n.start >= clip_len) {
      ++missed;
      continue;
    }
    n.length = std::min(n.length, clip_len - n.start);
    if (uint32_t id = h.add_note(cid, n)) pasted.push_back(id);
  }
  h.end_group();
  selection_ = pasted;
  rebuild_rows();
  std::string msg = pasted.size() == 1 ? "Pasted 1 note" : "Pasted " + std::to_string(pasted.size()) + " notes";
  if (missed) msg += " (" + std::to_string(missed) + " past the clip's end left out)";
  app_.message(msg);
  app_.invalidate_all();
}

void PianoRollView::open_settings() {
  App& app = app_;
  Clip* clip = app.open_clip();
  if (!clip) return;
  const uint32_t cid = clip->id;
  std::vector<InspectorField> fields;
  std::vector<uint32_t> ids = targets();

  if (!ids.empty()) {
    // The selected notes: values shown are the first note's; changes apply to all.
    auto first = [&app, cid, ids]() -> const Note* {
      const Clip* c = app.project().find_clip(cid);
      return c ? c->find(ids.front()) : nullptr;
    };
    auto change_all = [&app, cid, ids](const std::function<void(Note&)>& f) {
      Clip* c = app.project().find_clip(cid);
      if (!c) return;
      app.history().begin_group();
      for (uint32_t id : ids)
        if (const Note* n = c->find(id)) {
          Note copy = *n;
          f(copy);
          app.history().change_note(cid, copy);
        }
      app.history().end_group();
    };
    fields.push_back({"Velocity", [first] { const Note* n = first(); return n ? std::to_string(n->velocity) : std::string("-"); },
                      [change_all](int step, bool coarse) {
                        change_all([&](Note& n) { n.velocity = uint8_t(std::max(1, std::min(127, n.velocity + step * (coarse ? 10 : 1)))); });
                      }});
    fields.push_back({"Probability", [first] { const Note* n = first(); return n ? std::to_string(n->probability) + "%" : std::string("-"); },
                      [change_all](int step, bool coarse) {
                        change_all([&](Note& n) { n.probability = uint8_t(std::max(0, std::min(100, n.probability + step * (coarse ? 10 : 1)))); });
                      }});
    fields.push_back({"Glide", [first] { const Note* n = first(); return std::string(n && n->glide ? "On" : "Off"); },
                      [change_all](int, bool) { change_all([](Note& n) { n.glide = !n.glide; }); }});
    // Start and length: an eighth of the grid per step (off the grid), with R a whole grid step.
    const Tick s = snap(), clip_len = clip->length, fine = std::max<Tick>(1, s / 8);
    fields.push_back({"Start", [first] { const Note* n = first(); return n ? format_position(n->start) : std::string("-"); },
                      [&app, cid, ids, change_all, s, fine, clip_len](int step, bool coarse) {
                        const Tick by = step * (coarse ? s : fine);
                        // Only if every selected note stays inside the clip.
                        const Clip* c = app.project().find_clip(cid);
                        if (!c) return;
                        for (uint32_t id : ids)
                          if (const Note* n = c->find(id))
                            if (n->start + by < 0 || n->start + n->length + by > clip_len) return;
                        change_all([&](Note& n) { n.start += by; });
                      }});
    fields.push_back({"Length", [first] { const Note* n = first(); return n ? format_length(n->length) : std::string("-"); },
                      [change_all, s, fine, clip_len](int step, bool coarse) {
                        const Tick by = step * (coarse ? s : fine);
                        change_all([&](Note& n) { n.length = std::max(fine, std::min<Tick>(clip_len - n.start, n.length + by)); });
                      }});
    fields.push_back({"Snap to grid", [] { return std::string("press A"); }, nullptr,
                      [change_all, s, clip_len, &app] {
                        change_all([&](Note& n) { snap_note(n, s, clip_len); });
                        app.message("Snapped to the grid");
                      }});
    fields.push_back({"Copy", [] { return std::string("press A (or Select + Y)"); }, nullptr, [this, &app] {
                        copy();
                        app.close_inspector();
                      }});
    fields.push_back({"Cut", [] { return std::string("press A"); }, nullptr, [this, &app, cid, ids] {
                        copy();
                        app.history().begin_group();
                        for (uint32_t id : ids) app.history().remove_note(cid, id);
                        app.history().end_group();
                        selection_.clear();
                        app.close_inspector();
                      }});
    // Parameter locks: this note's own value for a setting ("-" when it follows the track).
    const int track = app.track();
    for (ParamId pid : automatable_params(app.project().tracks[size_t(track)].instrument)) {
      if (pid >= 100) continue;  // mixer and effect settings aren't per note
      fields.push_back({"Lock: " + param_name(app.project().tracks[size_t(track)], pid),
                        [first, pid, &app, track] {
                          const Note* n = first();
                          const ParamLock* lk = n ? n->lock(pid) : nullptr;
                          return lk ? format_track_param(app.project().tracks[size_t(track)], pid, lk->value) : std::string("-");
                        },
                        [&app, cid, ids, pid, track](int step, bool coarse) {
                          Clip* c = app.project().find_clip(cid);
                          if (!c) return;
                          app.history().begin_group();
                          bool full = false;
                          for (uint32_t id : ids)
                            if (const Note* n = c->find(id)) {
                              Note copy = *n;
                              const ParamLock* lk = copy.lock(pid);
                              const float from = lk ? lk->value : app.project().track_param(track, pid);
                              const float v = step_track_param(app.project().tracks[size_t(track)], pid, from, step, coarse, 0.01f);
                              if (!copy.set_lock(pid, v)) full = true;
                              else app.history().change_note(cid, copy);
                            }
                          app.history().end_group();
                          if (full) app.message("Up to 4 locks per note");
                        }});
    }
    fields.push_back({"Clear locks", [] { return std::string("press A"); }, nullptr,
                      [change_all, &app] {
                        change_all([](Note& n) {
                          while (n.num_locks) n.clear_lock(n.locks[0].id);
                        });
                        app.message("Locks cleared");
                      }});
    char title[32];
    std::snprintf(title, sizeof title, ids.size() == 1 ? "1 note" : "%zu notes", ids.size());
    app.open_inspector(title, std::move(fields));
    return;
  }
  open_clip_settings();
}

void PianoRollView::open_clip_settings() {
  App& app = app_;
  Clip* clip = app.open_clip();
  if (!clip) return;
  const uint32_t cid = clip->id;
  std::vector<InspectorField> fields;
  // The clip's loop first (what a placement in the arrangement repeats), then the grid and the rest.
  fields.push_back({"Loop length", [&app, cid] {
                      const Clip* c = app.project().find_clip(cid);
                      char b[24];
                      std::snprintf(b, sizeof b, "%.2f bars", c ? double(c->length) / kBar : 0.0);
                      return std::string(b);
                    },
                    [&app, cid](int step, bool coarse) {
                      const Clip* c = app.project().find_clip(cid);
                      if (!c) return;
                      Clip copy = *c;
                      const Tick unit = coarse ? kBar : kTicksPerQuarter;
                      copy.length = std::max(unit, (copy.length + step * unit) / unit * unit);
                      app.history().change_clip(copy);
                    }});
  fields.push_back({"Grid", [this] { return std::string(kSnaps[snap_index_].name); },
                    [this](int step, bool) {
                      snap_index_ = std::max(0, std::min(kNumSnaps - 1, snap_index_ + step));
                      cursor_ = cursor_ / snap() * snap();
                      keep_cursor_visible(layout());
                    }});
  fields.push_back({"Key", [&app] { return std::string(key_name(app.project().key)); },
                    [&app](int step, bool) { app.history().set_key_scale(app.project().key + step + 12, app.project().scale); }});
  fields.push_back({"Scale", [&app] { return std::string(scale_name(app.project().scale)); },
                    [&app](int step, bool) {
                      int n = int(ScaleType::kCount);
                      app.history().set_key_scale(app.project().key, ScaleType((int(app.project().scale) + step + n) % n));
                    }});
  fields.push_back({"Fold to scale", [this] { return std::string(fold_ ? "On" : "Off"); },
                    [this](int, bool) {
                      fold_ = !fold_;
                      rebuild_rows();
                      keep_cursor_visible(layout());
                    }});
  fields.push_back({"Lane", [this] { return lane_ < 0 ? std::string("Off") : lane_name(); },
                    [this, &app](int step, bool) {
                      // Off, Velocity, then each automatable setting.
                      std::vector<int> options = {-1, 0};
                      for (ParamId pid : automatable_params(app.project().tracks[size_t(app.track())].instrument))
                        options.push_back(int(pid));
                      auto it = std::find(options.begin(), options.end(), lane_);
                      int idx = it == options.end() ? 1 : int(it - options.begin());
                      lane_ = options[size_t((idx + step + int(options.size())) % int(options.size()))];
                      if (lane_ < 0) lane_focus_ = false;
                    }});
  if (!app.note_clipboard().empty())
    fields.push_back({"Paste at cursor", [&app] {
                        const size_t n = app.note_clipboard().size();
                        return std::to_string(n) + (n == 1 ? " note: press A" : " notes: press A");
                      },
                      nullptr, [this, &app] {
                        app.close_inspector();
                        paste();
                      }});
  fields.push_back({"Select all", [] { return std::string("press A (or Y + A)"); }, nullptr, [this, &app] {
                      app.close_inspector();
                      select_all();
                    }});
  fields.push_back({"Snap all notes to grid", [] { return std::string("press A"); }, nullptr,
                    [this, &app, cid] {
                      const Clip* c = app.project().find_clip(cid);
                      if (!c) return;
                      Clip copy = *c;
                      for (Note& n : copy.notes) snap_note(n, snap(), copy.length);
                      copy.sort();
                      app.history().change_clip(copy);
                      app.message("Snapped to the grid");
                    }});
  // The way up to the main menu (Menu and Select + X here open this panel).
  fields.push_back({"Main menu", [] { return std::string("press A"); }, nullptr, [&app] { app.open_main_menu(); }});
  app.open_inspector("Clip", std::move(fields));
}

void PianoRollView::handle_input() {
  const InputTracker& in = app_.input();
  EditHistory& h = app_.history();
  Clip* clip = app_.open_clip();
  const int track = app_.track();
  const Layout l = layout();
  rebuild_rows();

  // A new clip (or a different one) resets the cursor and selection.
  if (clip && clip->id != shown_clip_) {
    shown_clip_ = clip->id;
    selection_.clear();
    cursor_ = scroll_ = 0;
    if (!clip->notes.empty()) cursor_pitch_ = clip->notes.front().pitch;
    top_row_ = std::max(0, row_of(cursor_pitch_) - 10);
    keep_cursor_visible(l);
  }

  // L/R held with the d-pad: the grid and paging.
  // The lane strip has its own d-pad meanings; there R means bigger steps.
  if (lane_focus_ && lane_ >= 0 && clip) {
    if (in.pressed(kX)) {
      open_settings();
      return;
    }
    lane_input();
    return;
  }
  const bool lr = in.held(kL) || in.held(kR);
  if (lr && !in.held(kA) && !in.held(kB) && !in.held(kY)) {
    bool used = false;
    const int snap_was = snap_index_;
    if (in.pressed_or_repeat(kLeft)) snap_index_ = std::max(0, snap_index_ - 1), used = true;
    if (in.pressed_or_repeat(kRight)) snap_index_ = std::min(kNumSnaps - 1, snap_index_ + 1), used = true;
    if (snap_index_ != snap_was) app_.message(std::string("Grid: ") + kSnaps[snap_index_].name);
    const int page = std::max(1, l.grid.h / l.row_h - 2);
    if (in.pressed_or_repeat(kUp)) cursor_pitch_ = shifted_pitch(cursor_pitch_, page, false), used = true;
    if (in.pressed_or_repeat(kDown)) cursor_pitch_ = shifted_pitch(cursor_pitch_, -page, false), used = true;
    if (used) {
      cursor_ = cursor_ / snap() * snap();
      keep_cursor_visible(l);
      app_.invalidate(app_.content_rect());
    }
    return;
  }
  if (!clip) {
    // Nothing to edit yet: A makes a one-bar clip after the track's last one.
    if (in.pressed(kA)) {
      Tick at = 0;
      for (const Placement& pl : app_.project().tracks[size_t(track)].arrangement) at = std::max(at, pl.start + pl.length);
      h.add_clip(track, kBar, (at + kBar - 1) / kBar * kBar);
      app_.message("New clip");
    }
    return;
  }
  const uint32_t cid = clip->id;
  const Tick s = snap();
  // L held with A or B: fine steps, an eighth of the grid, for notes off the grid.
  const bool fine = in.held(kL) && (in.held(kA) || in.held(kB));
  const Tick f = std::max<Tick>(1, s / 8);
  bool moved = false;

  // A: add or select; held: resize (left/right) or transpose (up/down). With Y held: select every
  // note in the clip.
  if (in.held(kY) && in.pressed(kA)) {
    y_selected_all_ = true;  // releasing Y mustn't toggle a note or select an area
    a_used_ = true;
    h.begin_group();  // the release of A ends it
    select_all();
    moved = true;
  } else if (in.pressed(kA)) {
    a_used_ = false;
    h.begin_group();
    if (const Note* existing = note_under_cursor()) {
      if (!contains(selection_, existing->id)) selection_ = {existing->id};
      last_length_ = existing->length;
      audition(existing->pitch);
    } else {
      Note n;
      n.start = cursor_;
      n.length = std::max<Tick>(1, std::min(last_length_, clip->length - cursor_));
      n.pitch = uint8_t(cursor_pitch_);
      uint32_t id = h.add_note(cid, n);
      selection_ = {id};
      audition(cursor_pitch_);
    }
    moved = true;
  }
  if (in.held(kA) && !in.pressed(kA)) {
    const int dt = in.pressed_or_repeat(kRight) ? 1 : in.pressed_or_repeat(kLeft) ? -1 : 0;
    const int dp = in.pressed_or_repeat(kUp) ? 1 : in.pressed_or_repeat(kDown) ? -1 : 0;
    if (dt || dp) {
      std::vector<uint32_t> ids = targets();
      const bool octave = in.held(kR);
      for (uint32_t id : ids) {
        Note n = *clip->find(id);
        if (dt && fine) {
          n.length = std::max(f, std::min(n.length + dt * f, clip->length - n.start));
          last_length_ = n.length;
        } else if (dt) {
          Tick len = n.length + dt * s;
          if (len < s) len = dt < 0 ? std::min(n.length, s) : s;
          n.length = std::min(len, clip->length - n.start);
          last_length_ = n.length;
        }
        if (dp) n.pitch = uint8_t(shifted_pitch(n.pitch, dp, octave));
        h.change_note(cid, n);
        clip = app_.open_clip();
      }
      if (dp) {
        cursor_pitch_ = shifted_pitch(cursor_pitch_, dp, octave);
        audition(cursor_pitch_);
      }
      a_used_ = moved = true;
    }
  }
  if (in.released(kA)) h.end_group();

  // B: delete; held: move.
  if (in.pressed(kB)) {
    b_used_ = false;
    h.begin_group();
  }
  if (in.held(kB) && !in.pressed(kB)) {
    const int dt = in.pressed_or_repeat(kRight) ? 1 : in.pressed_or_repeat(kLeft) ? -1 : 0;
    const int dp = in.pressed_or_repeat(kUp) ? 1 : in.pressed_or_repeat(kDown) ? -1 : 0;
    if (dt || dp) {
      std::vector<uint32_t> ids = targets();
      selection_ = ids;
      const bool octave = in.held(kR);
      // Move in time only if every note stays inside the clip. Fine moves leave the cursor where it
      // is, on the grid.
      const Tick by = dt * (fine ? f : s);
      bool time_ok = dt != 0;
      for (uint32_t id : ids) {
        const Note& n = *clip->find(id);
        if (n.start + by < 0 || n.start + n.length + by > clip->length) time_ok = false;
      }
      for (uint32_t id : ids) {
        Note n = *clip->find(id);
        if (time_ok) n.start += by;
        if (dp) n.pitch = uint8_t(shifted_pitch(n.pitch, dp, octave));
        h.change_note(cid, n);
        clip = app_.open_clip();
      }
      if (time_ok && !fine) cursor_ = std::max<Tick>(0, cursor_ + by);
      if (dp) cursor_pitch_ = shifted_pitch(cursor_pitch_, dp, octave);
      if (!ids.empty()) audition(cursor_pitch_);
      b_used_ = moved = true;
    }
  }
  if (in.released(kB)) {
    if (!b_used_) {
      std::vector<uint32_t> ids = targets();
      for (uint32_t id : ids) {
        h.remove_note(cid, id);
        selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
      }
      if (!ids.empty()) app_.message_undoable(ids.size() > 1 ? "Deleted " + std::to_string(ids.size()) + " notes" : "Note deleted");
      moved = true;
    }
    h.end_group();
  }

  // Y: rectangle selection while held; alone, toggle the note under the cursor.
  if (in.pressed(kY)) {
    y_used_ = false;
    y_selected_all_ = false;
    y_anchor_tick_ = cursor_;
    y_anchor_pitch_ = cursor_pitch_;
  }

  // X: settings for the selection, or the clip.
  if (in.pressed(kX)) {
    open_settings();
    return;
  }

  // Plain d-pad (also while Y is held, to stretch the selection).
  if (!in.held(kA) && !in.held(kB)) {
    const Tick last = std::max<Tick>(0, (clip->length - 1) / s * s);
    if (in.pressed_or_repeat(kLeft)) cursor_ = std::max<Tick>(0, cursor_ - s), moved = true;
    if (in.pressed_or_repeat(kRight)) cursor_ = std::min(last, cursor_ + s), moved = true;
    if (in.pressed_or_repeat(kUp)) cursor_pitch_ = shifted_pitch(cursor_pitch_, 1, false), moved = true;
    if (in.pressed_or_repeat(kDown)) cursor_pitch_ = shifted_pitch(cursor_pitch_, -1, false), moved = true;
    if (moved && in.held(kY)) y_used_ = true;
  }

  if (in.released(kY) && y_selected_all_) {
    y_selected_all_ = false;
  } else if (in.released(kY)) {
    if (y_used_) {
      const Tick t0 = std::min(y_anchor_tick_, cursor_), t1 = std::max(y_anchor_tick_, cursor_) + s;
      const int p0 = std::min(y_anchor_pitch_, cursor_pitch_), p1 = std::max(y_anchor_pitch_, cursor_pitch_);
      selection_.clear();
      for (const Note& n : clip->notes)
        if (n.pitch >= p0 && n.pitch <= p1 && n.start < t1 && n.start + n.length > t0) selection_.push_back(n.id);
      app_.message(std::to_string(selection_.size()) + " selected");
    } else if (const Note* n = note_under_cursor()) {
      if (contains(selection_, n->id)) selection_.erase(std::find(selection_.begin(), selection_.end(), n->id));
      else selection_.push_back(n->id);
    } else {
      selection_.clear();
    }
    moved = true;
  }

  if (moved || in.pressed(~0u) || in.released(~0u)) {
    rebuild_rows();
    keep_cursor_visible(l);
    app_.invalidate(app_.content_rect());
  }
}

void PianoRollView::tick() {
  const Layout l = layout();
  int x = -1;
  const Placement* pl = app_.open_placement();
  const Clip* clip = app_.open_clip();
  if (app_.engine().playing() && pl && clip && clip->length > 0) {
    const Tick pos = app_.engine().position();
    if (pos >= pl->start && pos < pl->start + pl->length) {
      const Tick ct = (pos - pl->start + pl->offset) % clip->length;
      x = tick_x(l, ct);
      if (x < l.grid.x || x >= l.grid.right()) x = -1;
    }
  }
  if (x != last_playhead_x_) {
    const int w = int(app_.u(6)) + 2;  // the line, and the marker in the ruler (5 points each side)
    if (last_playhead_x_ >= 0) app_.invalidate(Rect{last_playhead_x_ - w, l.ruler.y, 2 * w + 1, l.ruler.h + l.grid.h + l.lane.h});
    if (x >= 0) app_.invalidate(Rect{x - w, l.ruler.y, 2 * w + 1, l.ruler.h + l.grid.h + l.lane.h});
    last_playhead_x_ = x;
  }
}

std::vector<Hint> PianoRollView::hints() const {
  const InputTracker& in = app_.input();
  if (lane_focus_ && lane_ >= 0)
    return {{"Left/Right", "move"}, {"Up/Down", lane_ == 0 ? "velocity" : "value"}, {"B", lane_ == 0 ? "reset" : "remove point"},
            {"Select+Up", "notes"}};
  if (in.held(kA)) return {{"Left/Right", "length"}, {"L+Left/Right", "fine"}, {"Up/Down", "pitch"}, {"R+Up/Down", "octave"}};
  if (in.held(kB)) return {{"D-pad", "move"}, {"L+Left/Right", "fine"}, {"R+Up/Down", "octave"}, {"release", "delete"}};
  if (in.held(kY)) return {{"D-pad", "select area"}, {"A", "select all"}, {"release", "toggle note"}};
  if (in.held(kL) || in.held(kR)) return {{"Left/Right", "grid"}, {"Up/Down", "page"}, {"Start", "from cursor"}};
  if (!app_.open_clip()) return {{"A", "new clip"}, {"L2", "arrange"}};
  return {{"A", "add"}, {"B", "delete"}, {"Y", "select"}, {"X", "note/clip"}, {"Start", "play"}};
}

void PianoRollView::draw(Canvas& c) {
  const Theme& th = app_.theme();
  const Project& p = app_.project();
  const Layout l = layout();
  const Rect content = app_.content_rect();
  const Clip* clip = app_.open_clip();
  const Placement* open_pl = app_.open_placement();
  const Font& small = Font::get(FontFace::CondensedBold, app_.u(14));
  const Font& label = Font::get(FontFace::CondensedSemiBold, app_.u(12));
  const Color col = app_.track_colour(app_.track());
  const int colour_index = int(p.tracks[size_t(app_.track())].colour);
  const Color deep = th.track_ink(colour_index);
  const int line = std::max(1, int(app_.u(1)));
  rebuild_rows();

  if (!clip) {
    c.fill(content, th.background);
    const Font& f = Font::get(FontFace::Regular, app_.u(14));
    const char* msg = "No clip on this track. Press A to make one.";
    c.text(f, content.x + (content.w - f.measure(msg)) / 2, content.y + content.h / 2, msg, th.text_secondary);
    return;
  }
  const Tick s = snap();
  const int visible_rows = (l.grid.h + l.row_h - 1) / l.row_h;

  // Ruler: bar numbers and beat ticks; the corner above the keys shows the grid.
  c.fill(Rect{content.x, content.y, l.keys.w, l.ruler.h}, th.panel);
  c.fill(l.ruler, th.header);
  const int rbase = small.baseline_in(l.ruler.y, l.ruler.h);
  c.text(small, content.x + app_.u(8), rbase, kSnaps[snap_index_].name, th.text_secondary);
  const Tick right_tick = scroll_ + Tick(int64_t(l.grid.w) * s / l.cell_w) + s;
  for (Tick t = scroll_ / kTicksPerQuarter * kTicksPerQuarter; t < right_tick; t += kTicksPerQuarter) {
    int x = tick_x(l, t);
    if (x < l.ruler.x) continue;
    if (t % kBar == 0) {
      c.vline(x, l.ruler.y + int(app_.u(6)), l.ruler.bottom(), th.grid_bar);
      c.text(small, x + app_.u(4), rbase, std::to_string(t / kBar + 1).c_str(), th.text);
    } else if (int64_t(kTicksPerQuarter) * l.cell_w / s >= int(app_.u(10))) {
      c.vline(x, l.ruler.bottom() - int(app_.u(4)), l.ruler.bottom(), th.grid_beat);
    }
  }
  c.fill(Rect{content.x, l.ruler.bottom() - line, content.w, line}, th.text);

  // Lanes.
  for (int r = 0; r < visible_rows; ++r) {
    const int ri = top_row_ + r;
    const int y = l.grid.y + r * l.row_h;
    Rect lane{l.grid.x, y, l.grid.w, std::min(l.row_h, l.grid.bottom() - y)};
    if (ri >= int(rows_.size())) {
      c.fill(Rect{content.x, y, content.w, l.grid.bottom() - y}, th.background);
      break;
    }
    const int pitch = rows_[size_t(ri)];
    const bool scale_on = p.scale != ScaleType::Chromatic && !kit_track(p.tracks[size_t(app_.track())]);
    const bool kit = kit_track(p.tracks[size_t(app_.track())]);
    const bool shaded = kit ? false : scale_on ? !in_scale(pitch, p.key, p.scale) : is_black_key(pitch);
    c.fill(lane, shaded ? th.row_black : th.row_white);
    if (scale_on && ((pitch - p.key) % 12 + 12) % 12 == 0) c.fill(lane, th.scale_root);
    if (kit)  // named rows: a firmer line between them
      c.fill(Rect{lane.x, y + l.row_h - line, lane.w, line}, th.grid_beat);
    else
      c.hline(lane.x, lane.right(), y + l.row_h - 1, th.grid_step);
  }

  // Grid lines: steps (if not too dense), beats, bars.
  if (l.cell_w >= int(app_.u(6)))
    for (Tick t = scroll_ / s * s; t < right_tick; t += s) c.vline(tick_x(l, t), l.grid.y, l.grid.bottom(), th.grid_step);
  for (Tick t = scroll_ / kTicksPerQuarter * kTicksPerQuarter; t < right_tick; t += kTicksPerQuarter) {
    int x = tick_x(l, t);
    if (x >= l.grid.x) c.vline(x, l.grid.y, l.grid.bottom(), t % kBar == 0 ? th.grid_bar : th.grid_beat);
  }
  // Past the end of the clip.
  const int end_x = tick_x(l, clip->length);
  if (end_x < l.grid.right()) {
    c.fill(Rect{std::max(end_x, l.grid.x), l.grid.y, l.grid.right() - std::max(end_x, l.grid.x), l.grid.h}, th.clip_end_shade);
    if (end_x >= l.grid.x) c.vline(end_x, l.ruler.y, l.grid.bottom(), th.text_secondary);
  }

  auto note_rect = [&](Tick start, Tick len, int pitch) -> Rect {
    const int r = row_of(pitch) - top_row_;
    if (r < 0 || r >= visible_rows) return Rect{};
    const int x0 = tick_x(l, start), x1 = tick_x(l, start + len);
    const int gap = std::max(1, int(app_.u(1)));
    return Rect{x0 + 1, l.grid.y + r * l.row_h + gap, std::max(2, x1 - x0 - 1), l.row_h - 2 * gap};
  };

  // Ghost notes: other tracks playing at the same time, faintly. (Not on kits: their rows are
  // sounds, not pitches.)
  if (open_pl && !kit_track(p.tracks[size_t(app_.track())])) {
    const Tick song0 = open_pl->start - open_pl->offset;  // song time of clip time 0
    for (int t = 0; t < int(p.tracks.size()); ++t) {
      if (t == app_.track()) continue;
      const Color gc = app_.track_colour(t).with_alpha(110);
      for (const Placement& op : p.tracks[size_t(t)].arrangement) {
        const Clip* oc = p.find_clip(op.clip_id);
        if (!oc || oc->length <= 0) continue;
        for (Tick rep = -op.offset; rep < op.length; rep += oc->length)
          for (const Note& n : oc->notes) {
            const Tick at = rep + n.start;
            if (at < 0 || at >= op.length) continue;
            const Tick ct = op.start + at - song0;
            if (ct + n.length < scroll_ || ct > right_tick || ct >= clip->length) continue;
            Rect r = note_rect(ct, n.length, n.pitch);
            if (!r.empty()) c.stroke_rect(r, 1, gc);
          }
      }
    }
  }

  // Selection rectangle while Y is held.
  if (app_.input().held(kY) && y_used_) {
    const Tick t0 = std::min(y_anchor_tick_, cursor_), t1 = std::max(y_anchor_tick_, cursor_) + s;
    const int r0 = std::min(row_of(y_anchor_pitch_), row_of(cursor_pitch_)) - top_row_;
    const int r1 = std::max(row_of(y_anchor_pitch_), row_of(cursor_pitch_)) - top_row_;
    c.fill(Rect{tick_x(l, t0), l.grid.y + r0 * l.row_h, tick_x(l, t1) - tick_x(l, t0), (r1 - r0 + 1) * l.row_h},
           th.selection_fill);
  }

  // Notes: the track's colour, paler for quieter notes, outlined in its deep shade on paper.
  // Selected notes are solid in the deep shade (pale in the dark).
  for (const Note& n : clip->notes) {
    Rect r = note_rect(n.start, n.length, n.pitch);
    if (r.empty() || r.right() < l.grid.x || r.x > l.grid.right()) continue;
    const bool sel = contains(selection_, n.id);
    const Color fill = col.mix(th.tint_base, (127 - n.velocity) * 150 / 127);  // quieter: paler
    if (sel) {
      c.fill(r, deep);
    } else {
      c.fill(r, fill);
      if (!th.dark) c.stroke_rect(r, std::max(1, int(app_.u(1) + 0.5f)), deep);
    }
    if (n.num_locks > 0 || n.probability < 100 || n.glide) {
      const int d = std::max(2, int(app_.u(2.5f)));
      c.fill(Rect{r.x + d + line, r.y + (r.h - d) / 2, d, d}, sel ? fill : deep);
    }
  }

  // Keys (or note names, when folded to a scale; or the kit's sounds).
  const Track& track = p.tracks[size_t(app_.track())];
  if (kit_track(track)) {
    c.fill(l.keys, th.name_column);
    const Font& kf = Font::get(FontFace::CondensedSemiBold, app_.u(14));
    const float sp = app_.u(0.3f);
    for (int r = 0; r < visible_rows && top_row_ + r < int(rows_.size()); ++r) {
      const int pitch = rows_[size_t(top_row_ + r)];
      const int y = l.grid.y + r * l.row_h;
      std::string name = caps(slot_label(track, pitch));
      while (name.size() > 1 && kf.measure(name.c_str(), sp) > l.keys.w - app_.u(12)) name.pop_back();
      c.text(kf, l.keys.x + app_.u(8), kf.baseline_in(y, l.row_h - line), name.c_str(), th.text, sp);
      c.fill(Rect{l.keys.x, y + l.row_h - line, l.keys.w, line}, th.grid_beat);
    }
  } else if (!fold_) {
    draw_piano_keys(c, PianoKeysLayout{l.keys, rows_[size_t(top_row_)], visible_rows, l.row_h, app_.scale_value()}, th);
  } else {
    c.fill(l.keys, th.name_column);
    for (int r = 0; r < visible_rows && top_row_ + r < int(rows_.size()); ++r) {
      const int pitch = rows_[size_t(top_row_ + r)];
      const int y = l.grid.y + r * l.row_h;
      const bool root = ((pitch - p.key) % 12 + 12) % 12 == 0;
      const Font& f = root ? Font::get(FontFace::CondensedBold, app_.u(12)) : label;
      std::string name = note_name(pitch);
      c.text(f, l.keys.right() - app_.u(6) - f.measure(name.c_str()), f.baseline_in(y, l.row_h),
             name.c_str(), root ? th.text : th.text_secondary);
      c.hline(l.keys.x, l.keys.right(), y + l.row_h - 1, th.name_column_line);
    }
  }
  // The column lies on the grid like a sheet: a soft shadow over the grid's edge.
  c.vline(l.keys.right() - 1, content.y, l.keys.bottom(), th.grid_bar);
  draw_shadow(c, l.keys.right(), l.grid.y, l.grid.h, int(app_.u(6)), false, th.shadow_strength * 3 / 4);

  // Cursor: viewfinder corners around the cell (grey while the lane below has the focus).
  {
    const int r = row_of(cursor_pitch_) - top_row_;
    if (r >= 0 && r < visible_rows) {
      const int gap = std::max(1, int(app_.u(1)));
      Rect cur{tick_x(l, cursor_) + 1, l.grid.y + r * l.row_h + gap, l.cell_w - 1, l.row_h - 2 * gap};
      const Rect saved = c.clip();
      c.set_clip(saved.intersect(Rect{l.grid.x, content.y, l.grid.w, content.h}));
      draw_brackets(c, cur, lane_focus_ ? th.grid_bar : th.text, app_.u(1));
      c.set_clip(saved);
    }
  }

  if (!l.lane.empty()) draw_lane(c, l, *clip);

  // Playhead: an ink line with a marker in the ruler.
  if (last_playhead_x_ >= 0) {
    c.fill(Rect{last_playhead_x_, l.ruler.y + int(app_.u(2)), line, l.ruler.h + l.grid.h + l.lane.h - int(app_.u(2))},
           th.playhead_line);
    draw_marker(c, last_playhead_x_, l.ruler.y + int(app_.u(15)), int(app_.u(5)), th.playhead_line);
  }
}

void PianoRollView::draw_lane(Canvas& c, const Layout& l, const Clip& clip) {
  const Theme& th = app_.theme();
  const Color col = th.track_ink(int(app_.project().tracks[size_t(app_.track())].colour));
  const Tick s = snap();
  const Rect& r = l.lane;
  c.fill(l.lane_label, lane_focus_ ? th.chip : th.panel);
  c.fill(r, th.header);
  c.fill(Rect{l.lane_label.x, r.y, r.right() - l.lane_label.x, std::max(1, int(app_.u(1)))}, th.text);
  // The lane's name, spaced out if it fits, else tight, else shortened.
  const Font& f = Font::get(FontFace::CondensedSemiBold, app_.u(12));
  const float room = l.lane_label.w - app_.u(10);
  std::string name = caps(lane_name());
  const float sp = f.measure(name.c_str(), app_.u(kCapsSpacing)) <= room ? app_.u(kCapsSpacing) : 0;
  while (name.size() > 1 && f.measure(name.c_str(), sp) > room) name.pop_back();
  c.text(f, l.lane_label.x + app_.u(8), l.lane_label.y + int(app_.u(18)), name.c_str(), lane_focus_ ? th.text : th.text_secondary, sp);
  // Under it, the value at the cursor: the lane's, else the track's own.
  if (lane_ > 0) {
    const Track& tr = app_.project().tracks[size_t(app_.track())];
    const Lane* ln = clip.lane(ParamId(lane_));
    const float v = ln && !ln->points.empty() ? ln->value_at(cursor_) : app_.project().track_param(app_.track(), ParamId(lane_));
    const Font& vf = Font::get(FontFace::Regular, app_.u(11));
    std::string text = format_track_param(tr, ParamId(lane_), v);
    while (text.size() > 1 && vf.measure(text.c_str()) > room) text.pop_back();
    c.text(vf, l.lane_label.x + app_.u(8), l.lane_label.y + int(app_.u(34)), text.c_str(), th.text_secondary);
  }
  const Rect plot = Rect{r.x, r.y + int(app_.u(6)), r.w, r.h - int(app_.u(10))};
  float lo, hi;
  lane_range(lo, hi);
  auto y_of = [&](float v) { return plot.bottom() - int((v - lo) / (hi - lo) * plot.h); };
  const Tick right_tick = scroll_ + Tick(int64_t(r.w) * s / l.cell_w) + s;
  // Beat lines, faintly.
  for (Tick t = scroll_ / kTicksPerQuarter * kTicksPerQuarter; t < right_tick; t += kTicksPerQuarter) {
    int x = tick_x(l, t);
    if (x >= r.x) c.vline(x, r.y + 1, r.bottom(), t % (4 * kTicksPerQuarter) == 0 ? th.grid_beat : th.grid_step);
  }
  if (lane_ == 0) {
    // A bar per note, as tall as its velocity.
    const int w = std::max(2, int(app_.u(3)));
    for (const Note& n : clip.notes) {
      const int x = tick_x(l, n.start);
      if (x < r.x || x > r.right()) continue;
      const int y = y_of(n.velocity);
      c.fill(Rect{x + 1, y, w, plot.bottom() - y}, col);
      c.fill(Rect{x + 1 - w / 2, y - w / 2, 2 * w, w}, col);
    }
  } else {
    const Lane* lane = clip.lane(ParamId(lane_));
    if (!lane) {
      // No automation yet: the track's setting, as a flat line.
      const int y = y_of(app_.project().track_param(app_.track(), ParamId(lane_)));
      for (int x = r.x; x < r.right(); x += int(app_.u(6))) c.hline(x, std::min(r.right(), x + int(app_.u(3))), y, th.grid_bar);
    } else {
      int px = -1, py = 0;
      for (int x = r.x; x < r.right(); x += 2) {
        const Tick t = scroll_ + Tick(int64_t(x - r.x) * s / l.cell_w);
        if (t > clip.length) break;
        const int y = y_of(lane->value_at(t));
        if (px >= 0) c.line(float(px), float(py), float(x), float(y), col);
        px = x, py = y;
      }
      const int d = std::max(3, int(app_.u(3)));
      for (const LanePoint& pt : lane->points) {
        const int x = tick_x(l, pt.tick);
        if (x < r.x - d || x > r.right()) continue;
        c.fill(Rect{x - d, y_of(pt.value) - d, 2 * d, 2 * d}, col);
      }
    }
  }
  // Past the clip's end.
  const int end_x = tick_x(l, clip.length);
  if (end_x < r.right()) c.fill(Rect{std::max(end_x, r.x), r.y + 1, r.right() - std::max(end_x, r.x), r.h - 1}, th.clip_end_shade);
  // The cursor's column, when the lane has focus.
  if (lane_focus_) {
    const int x = tick_x(l, cursor_);
    draw_brackets(c, Rect{x + 1, r.y + int(app_.u(5)), l.cell_w - 1, r.h - int(app_.u(10))}, th.text, app_.u(1));
  }
}

}  // namespace twill
