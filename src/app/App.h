// The application: owns the project, its undo history and the engine; routes buttons to the
// current view; draws the top bar, the view and the hint bar; autosaves.
//
// Global buttons (design §7): Select + left/right switches view, Select + B undoes, Select + A
// redoes, Select + up/down moves between notes and lanes in the roll, Menu or Select + X opens the
// settings of what the screen shows, Start plays/stops, L or R + Start plays from the cursor, Menu +
// Start quits.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/ClockFollower.h"
#include "app/InputTracker.h"
#include "app/Theme.h"
#include "app/MidiSender.h"
#include "engine/Engine.h"
#include "engine/PlaitsVoice.h"
#include "gfx/Canvas.h"
#include "model/Edit.h"
#include "model/Multisample.h"
#include "platform/Platform.h"

namespace twill {

class ArrangementView;
class InstrumentView;
class MixerView;
class PianoRollView;
class PresetLoader;

// A list of settings shown in a panel on the right; up/down picks one, left/right changes it.
struct InspectorField {
  std::string label;
  std::function<std::string()> value;
  std::function<void(int step, bool coarse)> adjust;  // step is -1 or +1; may be null
  std::function<void()> press = nullptr;               // A; null if A does nothing
  bool heading = false;  // a section title: drawn as one, and the selection passes over it
  bool buttons = false;  // the label is a button combination ("Sel + X"), drawn as keycaps
};

struct Hint {
  const char* keys;
  const char* action;
};

class App {
 public:
  enum class View { Arrangement, PianoRoll, Instrument, Mixer };
  static constexpr const char* kAutosaveName = "autosave.twill";

  explicit App(Platform& platform);
  ~App();

  bool start();  // loads the autosave (or the demo song) and opens audio
  void stop();   // saves and closes audio
  // One frame: input, engine hand-off, drawing. Returns false when the app should quit.
  bool frame(const InputState& input, Canvas& canvas);

  // For the views.
  Platform& platform() { return p_; }
  Project& project() { return project_; }
  EditHistory& history() { return history_; }
  Engine& engine() { return *engine_; }
  ArrangementView& arrangement() { return *arrangement_; }
  const Theme& theme() const { return theme_; }
  // A track's own colour (it keeps it when moved), from the theme's set.
  Color track_colour(int track) const {
    return track >= 0 && track < int(project_.tracks.size()) ? theme_.track(int(project_.tracks[size_t(track)].colour))
                                                              : theme_.track(track);
  }
  // App settings, kept in settings.txt beside the projects (not in a project, not undoable).
  bool dark_mode() const { return theme_.dark; }
  void set_dark_mode(bool on);
  // New projects get a made-up two-word name ("Velvet Harbour") instead of "Untitled".
  bool auto_names() const { return auto_names_; }
  void set_auto_names(bool on);
  // Output boost: gain before the master limiter for live playback, for a device that plays
  // quietly (0, 3, 6, 9 or 12 dB). Renders are left as mixed.
  int output_boost_db() const { return boost_db_; }
  void set_output_boost_db(int db);
  // The audio buffer: Low keeps the delay short for playing live (512 frames on a handheld, about
  // 11 ms); Safe doubles it (1024, about 21 ms), for systems that drop out with the short one.
  // Until the user picks one, it's whichever the platform suggests. Changing it reopens the audio.
  bool safe_audio() const { return audio_pref_ < 0 ? p_.prefers_long_audio_buffer() : audio_pref_ == 1; }
  void set_safe_audio(bool on);
  std::string audio_buffer_text() const;  // "Low, 11 ms"
  std::string fresh_song_name();  // one no saved project has
  // Track names: a list of parts to step through ("Drums", "Bass", "Lead"...), and the editor for
  // a name of one's own. Both are undoable.
  static const std::vector<std::string>& role_names();
  static std::string next_role_name(const std::string& name, int step);
  void step_track_name(int track, int step);
  void edit_track_name(int track);
  const InputTracker& input() const { return input_; }
  float u(float units) const { return units * scale_; }
  float scale_value() const { return scale_; }

  int track() const { return track_; }
  // Clipboards: notes (starts relative to the first) from the roll, and a clip from the arrangement.
  std::vector<Note>& note_clipboard() { return note_clipboard_; }
  Clip& clip_clipboard() { return clip_clipboard_; }  // empty (id 0) until something's copied
  void set_track(int t);
  // The clip the piano roll shows for the current track, and where it's placed (null if none).
  const Placement* open_placement() const;
  Clip* open_clip();
  const Clip* open_clip() const { return const_cast<App*>(this)->open_clip(); }
  void open(int track, const Placement& pl);  // makes `pl` the track's open clip
  void show(View v);
  // Moves through the views in order (arrangement, roll, sound, mixer), wrapping round.
  void step_view(int by);
  // The main menu (song files, the song's tempo and key, MIDI, the app's settings, help): Menu or
  // Select + X in the arrangement and mixer, and the last row of the clip's and the track's
  // settings. It's titled kMainMenuTitle, with build_id() beside the title.
  static constexpr const char* kMainMenuTitle = "Main menu";
  static std::string build_id();  // "0.7.0 500fa1e"
  void open_main_menu();
  // Menu, or Select + X: the settings of what the screen shows (the main menu in the arrangement
  // and mixer, the clip in the roll, the track in the sound view), or puts them away if open. The
  // clip's and the track's end with a row that opens the main menu.
  void toggle_screen_menu();
  // Arms or disarms recording from MIDI (the settings panel's "Record from MIDI").
  void toggle_recording();
  // A panel listing every button combination.
  void open_controls();
  bool quit_menu_open() const { return quit_menu_; }
  std::vector<std::string> quit_menu_items() const;  // as shown, top first
  View view() const { return view_; }

  void audition(int track, int pitch);
  // Y in the views with nothing to select: a note of the track's sound (a kit's first slot, a
  // sample's own note, else middle C).
  void listen(int track);
  // The track's settings panel (X on its header in the arrangement, X in the sound view).
  void open_track_settings(int track);
  // A track's sound, as one list: the macro oscillator's engines, then the sampler, then MIDI out.
  // "Engine" in the app means a place in this list.
#if TWILL_PLUGINS
  static constexpr int kNumSounds = PlaitsVoice::kNumEngines + 3;  // ... MIDI out, Plugin
#else
  static constexpr int kNumSounds = PlaitsVoice::kNumEngines + 2;
#endif
  static int sound_index(const Track& t);
  static const char* sound_name(const Track& t);
  void step_sound(int track, int step);  // one undo step; plays a note to hear it
  void invalidate(const Rect& r);
  void invalidate_all();
  Rect content_rect() const { return content_; }
  // `note`: a few quiet words beside the title (the main menu's build).
  void open_inspector(std::string title, std::vector<InspectorField> fields, std::string note = "");
  bool inspector_open() const { return !inspector_fields_.empty(); }
  // The audio load as the About panel shows it: "45%, peaks 71%" (peaks over the last 3 s).
  std::string load_text();
  const std::string& inspector_title() const { return inspector_title_; }
  int inspector_scroll() const { return inspector_scroll_; }  // first row shown
  void close_inspector();
  int inspector_selection() const { return inspector_sel_; }
  const std::vector<InspectorField>& inspector_fields() const { return inspector_fields_; }
  void select_inspector_row(int row);  // scrolls it into view
  // True on the second press of the same thing within four seconds; the first shows `prompt`.
  bool confirm_twice(const char* what, const std::string& prompt);
  void ask_quit();  // the main menu's Quit Twill: on a second press
  void message(const std::string& text);  // shown briefly in the hint bar
  // Adds the project file actions (save, load, new, render) to a settings panel's fields.
  // The settings menu's song, export and MIDI rows.
  void add_song_fields(std::vector<InspectorField>& fields);
  void add_render_fields(std::vector<InspectorField>& fields);
  void add_midi_fields(std::vector<InspectorField>& fields);
  void open_about();  // version, commit and build date
  // A screenshot on request, for a handheld whose screen can't be read from outside: when the file
  // kScreenshotRequest appears in the data folder, the frame on screen is saved as
  // screenshots/screen-N.bmp (the first N not taken) and the request removed. Checked twice a second.
  static constexpr const char* kScreenshotRequest = "screenshot.request";
  static std::vector<uint8_t> encode_bmp(const Canvas& c);  // 24-bit, the whole canvas
  // Project files, in projects/ in the data folder.
  bool save_project_file();
  bool load_project_file(const std::string& name);
  void new_project();
  bool render_song_to_wav();
  // Each track that plays in the mix as its own 32-bit float WAV, with its share of the chorus,
  // delay and reverb but none of the master section, so that they add up to the mix before it:
  // renders/<song> stems/01 Drums.wav and so on, all the same length. Returns how many it wrote.
  int render_stems();
  std::vector<float> render_offline(const Project& song, bool master);  // the song's length + 3 s
  bool has_unsaved_changes();  // compared with its file in projects/ (true if it has none)

  // Samples, by path (relative to the data folder unless absolute), loaded once and kept while the
  // project uses them: voices and snapshots point into them. Null (with a reason) if it can't be
  // loaded. One the project stops using is freed once the engine has let go of it (see
  // free_unused_samples()); using it again loads it again.
  std::shared_ptr<const SampleData> load_sample(const std::string& path, std::string* error = nullptr);
  size_t loaded_samples() const { return samples_.size(); }
  size_t loaded_sample_bytes() const;
  size_t multisample_bytes(const std::string& preset) const;  // its samples' share of the memory
  // A DX7 32-voice bank file for the six-op FM engines (Track::fm_bank), named as samples are: its
  // voices (kFmBankBytes), read once and kept for the rest of the run (4 KB each), since voices
  // and snapshots point into them. Null (with a reason) if it can't be read.
  const uint8_t* load_fm_bank(const std::string& path, std::string* error = nullptr);
  // A multisample's preset (.dspreset or .sfz), read once with its zones' samples loaded (they're
  // kept while a track uses the preset). It loads in the background: null until it's ready
  // (multisample_loading() says when it's on its way), or null with why if the preset can't be
  // read or its samples would take more memory than the platform allows. Missing samples are left
  // out, and counted in multisample_missing().
  const Multisample* load_multisample(const std::string& path, std::string* error = nullptr);
  bool multisample_loading(const std::string& path) const;  // queued, or loading now
  // The preset loading now: its name, and its samples read so far of how many. False if none.
  bool loading_status(std::string& name, int& done, int& total) const;
  void finish_loading();  // waits until every queued preset has loaded (export, tests)
  int multisample_missing(const std::string& path) const;  // its zones whose samples couldn't load
  // Forgets that a preset couldn't be loaded, so the next load tries again (picked in the browser).
  void retry_multisample(const std::string& path) { multisample_errors_.erase(path); }
  // A preset picked for a track: once it has loaded, the track takes it and its amp envelope (one
  // undo step), and a notice says what in it isn't played. A later pick replaces a waiting one.
  void pick_multisample(int track, const std::string& path);
  // Memory for samples: what's loaded, and what the platform allows (0: no limit).
  size_t sample_budget() const { return p_.sample_budget(); }
  // A note over the screen, a title and a paragraph, until A or B.
  void show_notice(std::string title, std::string text);
  bool notice_open() const { return !notice_title_.empty(); }
  const std::string& notice_text() const { return notice_text_; }
  void free_unused_samples();  // every frame; checks twice a second

#if TWILL_PLUGINS
  // Plugin tracks (platform/Plugins.h). Each plays the instance in its slot: loaded the first time
  // a snapshot needs it, with the track's saved state, and kept while the engine may play it, as a
  // sample is. Null when the platform has none, the track has none chosen, or it won't load.
  PluginHost* plugin_host() { return p_.plugins(); }
  PluginInstance* plugin_instance(const Track& t);
  std::string plugin_error(const Track& t) const;  // why it isn't loaded; "" if it is
  // The track plays `info` from now: a new slot, no state yet. One undo step.
  void choose_plugin(int track, const PluginInfo& info);
  // A plugin's own parameter moved (not an edit of the project): the song is saved, as after one.
  void plugin_changed();
  // The track's plugin's own screen (PluginInstance::has_screen), over the whole display with the
  // buttons its, until Menu (or the plugin asks to close it). The music plays on meanwhile. False
  // if it has none or won't open it.
  bool open_plugin_screen(int track);
  bool plugin_screen_open() const { return screen_.inst != nullptr; }
  void close_plugin_screen();
#endif

 private:
  void handle_global();
  // The quit menu, where Menu is for quitting (Platform::menu_quits: sndstation): over whatever
  // shows, a plugin's screen too, until A on an item, B or Menu.
  struct QuitItem {
    std::string label;
    enum Action { Back, Sleep, Quit } action;
  };
  bool quit_menu_ = false;
  int quit_sel_ = 0;
  std::vector<QuitItem> quit_items_;
  void open_quit_menu();
  void close_quit_menu();
  void handle_quit_menu();
  void draw_quit_menu(Canvas& c);
  void screenshot_if_asked(const Canvas& c);
  uint64_t screenshot_checked_us_ = 0;
  void handle_inspector();
  void pump_loads();  // takes a finished preset load, and starts the next
  void apply_pick(int track, const std::string& path, const Multisample& m);
  void draw_notice(Canvas& c);
  void sync_engine();
  void play_from(Tick song_position);
  void autosave(bool force);
  void layout(const Canvas& c);
  void draw(Canvas& c);
  void draw_top_bar(Canvas& c);
  void draw_hint_bar(Canvas& c);
  void draw_inspector(Canvas& c);
  // A panel's sheet, shadow and title; returns the y below the title's rule.
  int draw_panel_frame(Canvas& c, const char* title);
  void validate_open_clips();
  void replace_project(Project p);
  // Asks for a second press within a few seconds before discarding unsaved work.
  bool confirm_discard(const char* what);
  void handle_name_edit();
  void handle_midi();
  // Following an external clock: the engine's tempo, nudged to keep it in step with the master.
  void follow_clock();
  int64_t midi_now_us() const;  // the clock incoming MIDI is stamped with
  ClockFollower clock_in_;
  double tempo_override_ = 0;  // sent to the engine; 0: the project's
  double phase_error_ = 0;     // beats the engine is ahead of the master, smoothed
  int output_latency_ = 0;     // frames from rendering to the speaker
  int block_frames_ = 256;
  int audio_pref_ = -1;  // the Audio buffer the user chose: 0 Low, 1 Safe, -1 not chosen
  bool open_audio();  // with the buffer the setting asks for; logs what the device gave
  void load_settings();
  bool auto_names_ = true;
  int boost_db_ = 0;
  uint32_t names_made_ = 0;
  void save_settings();
  // Recording from MIDI into the open clip (Select + X arms it).
  bool recording_ = false;
  struct HeldNote {
    uint8_t pitch;
    uint32_t note_id;
    Tick start;  // clip time
  };
  std::vector<HeldNote> held_notes_;
  void draw_name_edit(Canvas& c);

  Platform& p_;
  Theme theme_;
  Project project_;
  EditHistory history_{project_};
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<MidiSender> midi_sender_;  // after engine_: it reads the engine
  std::unique_ptr<ArrangementView> arrangement_;
  std::unique_ptr<PianoRollView> roll_;
  std::unique_ptr<InstrumentView> instrument_;
  std::unique_ptr<MixerView> mixer_;
  std::map<std::string, std::shared_ptr<const SampleData>> samples_;
  std::map<std::string, std::string> sample_errors_;  // so a missing file isn't retried every frame
  std::map<std::string, std::vector<uint8_t>> fm_banks_;  // never erased: voices point into them
  std::map<std::string, std::string> fm_bank_errors_;
#if TWILL_PLUGINS
  struct LoadedPlugin {
    std::unique_ptr<PluginInstance> inst;
    std::string id;
    bool idle = false, released = false;  // in no snapshot since idle_snapshot; told so
    uint64_t idle_snapshot = 0, idle_callbacks = 0;
  };
  std::map<uint32_t, LoadedPlugin> plugins_;         // by slot
  std::vector<LoadedPlugin> retired_plugins_;        // another song's, freed once the engine let go
  std::map<uint32_t, std::string> plugin_errors_;    // by slot: not tried again every snapshot
  bool plugin_dirty_ = false;                        // a parameter moved since the last autosave
  void capture_plugin_states();                      // into project_, before it is written
  void tend_plugins();                               // every frame: idle(), and what nothing plays
  void retire_plugins();                             // a new song: every instance let go of
  uint32_t next_plugin_slot() const;
  struct PluginScreen {
    PluginInstance* inst = nullptr;  // null: none open
    int w = 0, h = 0;                // the size it draws at
    std::vector<uint32_t> pixels;    // its last frame
    uint32_t ignored = 0;            // held when it opened: not its until let go of
    bool cleared = false;            // the display is blanked round it
  } screen_;
  void run_plugin_screen(Canvas& c);  // a frame of it: its buttons, then what it drew
  void forget_plugin_screen(const PluginInstance* inst);  // closed first if `inst` is its plugin
#endif
  std::map<std::string, std::shared_ptr<Multisample>> multisamples_;
  std::map<std::string, std::string> multisample_errors_;
  std::map<std::string, int> multisample_missing_;
  std::unique_ptr<PresetLoader> loader_;
  std::deque<std::string> load_queue_;
  struct Pick {
    int track = -1;
    std::string path;
  } pick_;
  uint64_t loads_done_ = 0, synced_loads_ = ~0ull;  // a finished load changes what the engine plays
  uint64_t load_redraw_us_ = 0;
  std::string notice_title_, notice_text_;
  // Loaded samples the project no longer uses, waiting for the engine to let go of them: the
  // snapshot it must be playing, and the callback count it must pass (two on from when nothing else
  // held the sample, so a preview that was playing it has stopped).
  struct UnusedSample {
    uint64_t snapshot = 0, callbacks = 0;
  };
  std::map<std::string, UnusedSample> unused_samples_;
  uint64_t unused_checked_us_ = 0;
  InputTracker input_;
  float scale_ = 1;
  View view_ = View::Arrangement;
  int track_ = 0;
  std::vector<Placement> open_;  // per track: the placement the roll shows (clip_id 0: none)

  uint64_t synced_revision_ = ~0ull;
  uint64_t transport_changes_ = 0, synced_transport_ = ~0ull, snapshot_counter_ = 0;
  uint64_t saved_revision_ = 0, last_edit_us_ = 0;
  uint64_t last_revision_seen_ = 0;
  bool quit_ = false;

  // Layout and drawing.
  Size size_;
  Rect top_bar_, content_, hint_bar_, inspector_rect_;
  bool inspector_live_ = false;  // its values change by themselves (the About panel's audio load)
  uint64_t live_redraw_us_ = 0;
  uint64_t last_midi_in_us_ = 0;  // when a MIDI keyboard last sent anything
  bool awake_ = false;            // the platform has been asked to keep the device awake
  float load_peaks_[6] = {};     // the engine's peak load, a slot per half second
  int load_slot_ = 0;
  std::vector<Rect> dirty_;
  std::string top_text_cache_, hint_cache_;
  std::string message_;
  uint64_t message_until_us_ = 0;

  // Name editing (a character at a time, with the d-pad).
  bool name_edit_ = false;
  int name_track_ = -1;  // the track being named; -1: the song
  std::string name_draft_;
  int name_cursor_ = 0;
  // Discard confirmations.
  std::string confirm_what_;
  uint64_t confirm_until_us_ = 0;

  // Inspector.
  std::string inspector_title_, inspector_note_;
  std::vector<InspectorField> inspector_fields_;
  // Notes from a MIDI keyboard passed on to a MIDI Out track: where each one went, for its note off.
  struct LiveOut {
    int8_t channel = -1;
    int32_t source = -1;
  };
  LiveOut live_out_[128];
  int inspector_sel_ = 0;
  uint32_t voices_stolen_seen_ = 0;
  uint64_t voices_full_until_us_ = 0;  // "Voices full" shows in the top bar until then
  int inspector_scroll_ = 0;
  bool menu_used_ = false;
  bool menu_panel_ = false;  // the panel open came from tapping Menu: another tap closes it  // another button went with Menu (Menu + Start quits), so no panel
  std::vector<Note> note_clipboard_;
  Clip clip_clipboard_;  // first row shown, when the fields don't all fit
};

}  // namespace twill
