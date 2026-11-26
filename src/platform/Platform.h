// What the app needs from a platform. Each port (SDL2, headless, later web and mobile) implements
// this; nothing outside src/platform/ includes OS or SDL headers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gfx/Geometry.h"
#include "platform/Midi.h"
#if TWILL_PLUGINS
#include "platform/Plugins.h"
#endif

namespace twill {

// Abstract buttons. Every platform maps its controls onto these.
enum Button : uint32_t {
  kUp = 1u << 0,
  kDown = 1u << 1,
  kLeft = 1u << 2,
  kRight = 1u << 3,
  kA = 1u << 4,
  kB = 1u << 5,
  kX = 1u << 6,
  kY = 1u << 7,
  kL = 1u << 8,
  kR = 1u << 9,
  kL2 = 1u << 10,
  kR2 = 1u << 11,
  kStart = 1u << 12,
  kSelect = 1u << 13,
  kMenu = 1u << 14,  // the handheld's function/hotkey button, where there is one
};
constexpr int kNumButtons = 15;
const char* button_name(Button b);

struct InputState {
  uint32_t held = 0;          // Button bits currently down
  bool quit_requested = false;  // window closed, OS asked us to quit
};

// Called on the audio thread with interleaved stereo float frames to fill.
using AudioCallback = void (*)(void* user, float* out, int frames);

struct AudioInfo {
  int sample_rate = 0;
  int block_frames = 0;  // frames per callback, as granted by the device
};

class Platform {
 public:
  virtual ~Platform() = default;

  virtual const char* name() const = 0;

  // Audio.
  virtual bool audio_open(int preferred_rate, int preferred_block, AudioCallback cb, void* user) = 0;
  virtual AudioInfo audio_info() const = 0;
  virtual void audio_close() = 0;

  // Display: the full-resolution pixel size, and the scale the UI should be laid out at.
  virtual Size display_size() const = 0;
  virtual float ui_scale() const = 0;
  // Shows the finished frame. `dirty` lists the regions that changed since the last present.
  virtual void present(const uint32_t* pixels, int stride, const std::vector<Rect>& dirty) = 0;

  // Input.
  virtual void poll_input(InputState& out) = 0;
  virtual const char* controller_name() const = 0;
  // True while the buttons come from a computer keyboard (the last button pressed was a key), so
  // on-screen hints should name keys rather than the handheld's buttons. Never on a handheld.
  virtual bool keyboard_labels() const { return false; }

  // Files. A path is relative to the app's data folder unless it starts with '/'. write_file
  // replaces the file whole or not at all.
  virtual bool read_file(const char* path, std::vector<uint8_t>& out) = 0;
  virtual bool write_file(const char* path, const std::vector<uint8_t>& data) = 0;
  struct DirEntry {
    std::string name;
    bool is_dir = false;
  };
  // Lists a folder (not "." or ".."), folders first, then by name. False if it can't be read.
  virtual bool list_dir(const char* path, std::vector<DirEntry>& out) = 0;
  virtual bool make_dir(const char* path) = 0;  // creates missing parent folders too
  virtual bool remove_file(const char* path) { (void)path; return false; }
  virtual std::string data_dir() const = 0;     // absolute, ends with '/'

  // MIDI: every connected device, merged; null if the platform has no MIDI.
  virtual MidiPort* midi() { return nullptr; }
#if TWILL_PLUGINS
  // Instruments it hosts (Plugin tracks); null if it has none.
  virtual PluginHost* plugins() { return nullptr; }
#endif
  // False when time is simulated (tests): the app then sends MIDI from its frame loop at once
  // rather than from a thread on the wall clock.
  virtual bool real_time() const { return true; }

  // True where Menu is the system's way to quit and sleep (sndstation, whose SDK also reports Select
  // then Start as Menu): Menu opens the app's quit menu (Sleep, Quit), and the screen's settings are
  // on Select + X alone.
  virtual bool menu_quits() const { return false; }
  // False where closing the app means nothing (the browser): the main menu then has no Quit.
  virtual bool can_quit() const { return true; }
  // True where Select + Start quits because nothing outside the app watches for it: PortMaster's
  // packages have a helper that closes the app on it, a package of Twill's own (KNULLI) doesn't.
  virtual bool select_start_quits() const { return false; }
  // The device sleeps, and this returns once it's awake. False, with why, where it can't.
  virtual bool sleep(std::string& why) {
    why = "Sleep isn't available here";
    return false;
  }

  // While true, music is playing (or being played on a MIDI keyboard): the device shouldn't dim,
  // mute or go to sleep for want of button presses. Called when it changes.
  virtual void keep_awake(bool) {}

  // True where the shortest audio buffer is known to drop out now and then, so the app should
  // start with the longer one unless the user chose otherwise: handhelds whose sound goes through
  // PipeWire (KNULLI on the TrimUI Brick Pro clicks a few times every 10 minutes at 512 frames).
  virtual bool prefers_long_audio_buffer() const { return false; }
  // True where the system sets the buffer size for every app (sndstation's Audio page), so the app
  // offers no setting of its own.
  virtual bool system_sets_audio_buffer() const { return false; }

  // How many bytes of samples may be loaded at once (multisamples stop loading past it); 0: no limit.
  virtual size_t sample_budget() const { return 0; }

  // Limits from this platform's benchmarks (design §6).
  virtual int max_tracks() const = 0;
  virtual int max_voices() const = 0;
  // How many of them may play the macro oscillator at once; 0: all of them.
  virtual int max_synth_voices() const { return 0; }

  virtual uint64_t micros() const = 0;
  virtual void log(const char* fmt, ...) = 0;
};

}  // namespace twill
