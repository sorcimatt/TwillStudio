// SDL2 platform: macOS, Linux and Windows desktops, and Linux handhelds such as the TrimUI Brick Pro.
#pragma once
#include <SDL.h>

#include <cstdio>

#include <string>

#include <memory>

#include "platform/Platform.h"
#include "platform/sdl2/OutputCompressor.h"

namespace twill {

// Raw joystick button numbers for one device. -1 means "not present"; the d-pad uses the hat
// unless it has its own buttons.
struct PadMap {
  int a = -1, b = -1, x = -1, y = -1, l = -1, r = -1, l2 = -1, r2 = -1, select = -1, start = -1,
      menu = -1;
  int up = -1, down = -1, left = -1, right = -1;
};

class Sdl2Platform : public Platform {
 public:
  Sdl2Platform();
  ~Sdl2Platform() override;
  bool init();  // false if no window could be created

  const char* name() const override { return name_; }

  bool audio_open(int preferred_rate, int preferred_block, AudioCallback cb, void* user) override;
  AudioInfo audio_info() const override { return audio_info_; }
  void audio_close() override;

  Size display_size() const override { return size_; }
  float ui_scale() const override { return ui_scale_; }
  void present(const uint32_t* pixels, int stride, const std::vector<Rect>& dirty) override;

  void poll_input(InputState& out) override;
  const char* controller_name() const override;

  // From M1's benchmarks (Docs/DESIGN.md §6): on the Brick Pro the heaviest macro-oscillator engine
  // costs 2.7% of a core per voice, so 16 synth voices stay under half of one core. Playing a
  // sample costs far less, so samples get 16 voices more (multisample chords with release tails
  // and layered zones need them).
  int max_tracks() const override { return 8; }
  int max_voices() const override { return handheld_ ? 32 : 64; }
  int max_synth_voices() const override { return handheld_ ? 16 : 0; }

  bool read_file(const char* path, std::vector<uint8_t>& out) override;
  bool write_file(const char* path, const std::vector<uint8_t>& data) override;
  bool list_dir(const char* path, std::vector<DirEntry>& out) override;
  bool make_dir(const char* path) override;
  bool remove_file(const char* path) override { return std::remove(resolve(path).c_str()) == 0; }
  std::string data_dir() const override { return data_dir_; }
  MidiPort* midi() override { return midi_.get(); }
  void keep_awake(bool on) override;
  bool prefers_long_audio_buffer() const override;
  bool can_quit() const override;
  bool keyboard_labels() const override { return !handheld_ && keyboard_last_; }
  bool select_start_quits() const override;
  // Where the system sleeps without stopping programs (TWILL_SLEEP_FILE, muOS): the sound and the
  // app stop until it wakes, or asks the app to quit. Called between frames.
  void wait_while_asleep();
  // On a handheld, from the memory free when the app starts (design §6, "Multisamples"); no limit
  // on a computer.
  size_t sample_budget() const override { return sample_budget_; }

  uint64_t micros() const override;
  void log(const char* fmt, ...) override;
  // The budget for `available` bytes of free memory: 60% of what's left after a reserve for the
  // system and the app itself, from 64 MB to 1 GB.
  static size_t budget_for(size_t available);

 protected:
  // Hooks for a platform built on this one (SndstationPlatform): the system's MIDI devices; the
  // game controllers, opened at start; an event, before the default handling (true: handled); and
  // the buttons held, read once per poll after the events.
  virtual std::unique_ptr<MidiPort> make_midi();
  virtual void open_input();
  virtual bool take_event(const SDL_Event&) { return false; }
  virtual uint32_t read_buttons() { return keys_ | read_pad(); }

  const char* name_ = "SDL2";
  std::string data_dir_;  // ends with a slash
  bool handheld_ = false;
  bool keyboard_last_ = true;  // the last button pressed was a key (else a game controller's)
  size_t sample_budget_ = 0;

 private:
  static void sdl_audio_callback(void* user, Uint8* stream, int len);
  void open_controller(int device_index);
  void follow_page_size();
  void update_size();
  bool pad_map_from_frontend(const char* joystick_name);
  uint32_t read_pad() const;
  std::string resolve(const char* path) const { return path[0] == '/' ? path : data_dir_ + path; }

  bool awake_held_ = false;  // keep_awake's files are in place
#ifdef __linux__
  // Audio thread only: real-time scheduling, and the frames in a row that were late or in time.
  bool audio_rt_tried_ = false, audio_rt_ = false;
  int audio_late_run_ = 0, audio_ok_run_ = 0;
#endif
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  Size size_;      // what the app draws: landscape, even on a portrait panel
  int rotate_ = 0;  // degrees the picture is turned on its way to a portrait panel (90 or 270)
  float ui_scale_ = 1;

  SDL_AudioDeviceID audio_dev_ = 0;
  OutputCompressor compressor_;  // handhelds: the chip's own, off while audio is open
  AudioInfo audio_info_;
  AudioCallback audio_cb_ = nullptr;
  void* audio_user_ = nullptr;

  SDL_Joystick* joystick_ = nullptr;
  SDL_GameController* controller_ = nullptr;
  PadMap pad_map_;
  bool has_pad_map_ = false;
  int raw_presses_logged_ = 0;  // the first few raw button numbers go to the log
  uint32_t keys_ = 0;
  bool quit_ = false;
  std::unique_ptr<MidiPort> midi_;
};

}  // namespace twill
