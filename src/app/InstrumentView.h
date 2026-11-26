// The sound of the current track (design §7, "Instrument"): a list of settings on the left, and a
// picture on the right (the engine for the macro oscillator; the waveform, the kit's slots or a
// multisample's zones for the sampler; the envelope while its settings are chosen). Also the
// sample browser.
//
//   Up/Down        choose a setting           Left/Right   change it (R held: bigger steps)
//   A              on a sample: browse; on an action: do it
//   B              reset the setting to its default
//   Y              play the sound
//   L/R            previous / next track
//
// Browser: Up/Down choose, A open folder or pick, B up a folder (or close), Y preview, X close.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/App.h"

namespace twill {

class InstrumentView {
 public:
  explicit InstrumentView(App& app) : app_(app) {}

  void handle_input();
  void tick();  // the zone map follows the zones sounding
  void draw(Canvas& c);
  std::vector<Hint> hints() const;
#if TWILL_PLUGINS
  bool browsing() const { return browser_open_ || plugins_open_; }
#else
  bool browsing() const { return browser_open_; }
#endif

  // A global chord used L or R (L/R + Start): releasing it then does nothing here.

 private:
  struct Row {
    std::string label;
    std::function<std::string()> value;
    std::function<float()> fraction;            // for the bar; negative: no bar
    std::function<void(int step, bool coarse)> adjust;
    std::function<void()> press;                // A; null if A does nothing here
    std::function<void()> reset;                // B; null if nothing to reset
    ParamId param = kParamNone;                 // the setting it changes, if it's one
  };
  std::vector<Row> rows();
  Row param_row(ParamId id, const char* label = nullptr);
  enum class Browse { Samples, Presets, FmBanks };  // what the file browser lists besides folders
  void open_browser(std::function<void(const std::string&)> on_pick, const std::string& start,
                    Browse what = Browse::Samples);
  void browser_input();
  void draw_browser(Canvas& c, const Rect& r);
  void draw_waveform(Canvas& c, const Rect& r, const SampleData& s, const Track& t);
  void draw_kit(Canvas& c, const Rect& r, const Track& t);
  void draw_macro(Canvas& c, const Rect& r, const Track& t);
  void draw_zones(Canvas& c, const Rect& r, const Track& t);
  // While the amp envelope's settings are chosen: its shape, the selected stage marked.
  void draw_adsr(Canvas& c, const Rect& r, const Track& t, ParamId selected);
  void draw_lpg(Canvas& c, const Rect& r, const Track& t, ParamId selected);
  int audition_note() const;

  App& app_;
  int sel_ = 0, top_ = 0;
  int kit_slot_ = 0;
  std::vector<int> sounding_;  // the zones of this track's multisample sounding, as last drawn

  // Browser.
  bool browser_open_ = false;
  Browse browse_ = Browse::Samples;
  std::string dir_;                      // absolute, ends with '/'
  std::vector<Platform::DirEntry> entries_;  // starts with ".." ("Up a folder") below the root
  bool list_browser_dir(const std::string& dir, const std::string& select);  // false: unreadable
  int bsel_ = 0, btop_ = 0;
  std::function<void(const std::string&)> on_pick_;
  std::shared_ptr<const SampleData> preview_;

#if TWILL_PLUGINS
  // The plugin list (a Plugin track's "Plugin" row): what the platform has installed.
  // Up/Down choose (R held: by 10), A picks, B or X close.
  bool plugins_open_ = false;
  std::vector<PluginInfo> plugin_list_;
  int psel_ = 0, ptop_ = 0;
  void open_plugins();
  void plugins_input();
  void draw_plugins(Canvas& c, const Rect& r);
  void draw_plugin(Canvas& c, const Rect& r, const Track& t);
#endif

  // Waveform overview cache.
  const SampleData* wave_for_ = nullptr;
  std::shared_ptr<const SampleData> wave_hold_;  // the one shown, kept so its address stays its own
  int wave_width_ = 0;
  std::vector<float> wave_peaks_;  // per column: max |l|,|r|
};

}  // namespace twill
