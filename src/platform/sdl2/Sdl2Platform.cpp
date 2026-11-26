#include "platform/sdl2/Sdl2Platform.h"

#include "platform/sdl2/SystemMidi.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

namespace twill {

namespace {

// The web build keeps its files in the browser's IndexedDB, mounted at /twill (web/pre.js). A
// write lands in memory; this copies it to IndexedDB (web/pre.js runs one copy at a time).
void persist() {
#ifdef __EMSCRIPTEN__
  EM_ASM({ if (Module.twillPersist) Module.twillPersist(); });
#endif
}

// Which way a portrait panel is mounted, as the kernel has it: the console rotation on the boot
// command line (fbcon=rotate:1 or 3), or the DRM panel's orientation. 0 when it doesn't say.
int kernel_rotation() {
  if (FILE* f = std::fopen("/proc/cmdline", "r")) {
    char line[2048] = {};
    const size_t n = std::fread(line, 1, sizeof line - 1, f);
    std::fclose(f);
    line[n] = 0;
    if (const char* r = std::strstr(line, "fbcon=rotate:")) {
      if (r[13] == '1') return 90;
      if (r[13] == '3') return 270;
    }
  }
  if (DIR* d = opendir("/sys/class/drm")) {
    int rot = 0;
    while (dirent* e = readdir(d)) {
      if (std::strncmp(e->d_name, "card", 4) != 0 || !std::strchr(e->d_name, '-')) continue;
      const std::string path = std::string("/sys/class/drm/") + e->d_name + "/panel_orientation";
      if (FILE* f = std::fopen(path.c_str(), "r")) {
        char v[32] = {};
        if (std::fgets(v, sizeof v, f)) {
          if (std::strncmp(v, "left_side_up", 12) == 0) rot = 270;
          if (std::strncmp(v, "right_side_up", 13) == 0) rot = 90;
        }
        std::fclose(f);
      }
      if (rot) break;
    }
    closedir(d);
    return rot;
  }
  return 0;
}


// Joysticks whose raw button numbers we know, matched by name. Anything else is looked up in the
// front end's own controller config (below), then SDL's game controller mapping.
struct KnownPad {
  const char* name;
  PadMap map;
};
PadMap pad(int a, int b, int x, int y, int l, int r, int l2, int r2, int select, int start, int menu) {
  PadMap m;
  m.a = a; m.b = b; m.x = x; m.y = y; m.l = l; m.r = r; m.l2 = l2; m.r2 = r2;
  m.select = select; m.start = start; m.menu = menu;
  return m;
}
const KnownPad kPadMaps[] = {
    {"TRIMUI Brick Pro Controller", pad(6, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15)},
    {"TRIMUI Brick Controller", pad(5, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14)},
};

// The value of attribute `name` in the XML tag that starts at `tag`, or "" (no unescaping: the
// front end writes plain names and numbers).
std::string xml_attr(const std::string& s, size_t tag, const char* name) {
  const size_t end = s.find('>', tag);
  const std::string key = std::string(" ") + name + "=\"";
  const size_t at = s.find(key, tag);
  if (at == std::string::npos || at > end) return "";
  const size_t from = at + key.size();
  const size_t to = s.find('"', from);
  return to == std::string::npos ? "" : s.substr(from, to - from);
}

struct KeyBinding {
  SDL_Keycode key;
  uint32_t button;
};
// Desktop keyboard layout.
const KeyBinding kKeys[] = {
    {SDLK_UP, kUp},       {SDLK_DOWN, kDown},     {SDLK_LEFT, kLeft},  {SDLK_RIGHT, kRight},
    {SDLK_x, kA},         {SDLK_z, kB},           {SDLK_s, kX},        {SDLK_a, kY},
    {SDLK_q, kL},         {SDLK_w, kR},           {SDLK_1, kL2},       {SDLK_2, kR2},
    {SDLK_RETURN, kStart}, {SDLK_SPACE, kStart},  {SDLK_TAB, kSelect}, {SDLK_RSHIFT, kSelect},
    {SDLK_ESCAPE, kMenu},
    // The keypad's arrows. Safari reports the Mac's arrow keys as keypad keys (macOS flags them
    // that way), and SDL's web port passes that on.
    {SDLK_KP_8, kUp},     {SDLK_KP_2, kDown},     {SDLK_KP_4, kLeft},  {SDLK_KP_6, kRight},
};

bool env_flag(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && std::strcmp(v, "0") != 0;
}

// Bytes of memory free for new work, as the kernel reckons it (MemAvailable); 0 if unknown.
size_t available_memory() {
  FILE* f = std::fopen("/proc/meminfo", "r");
  if (!f) return 0;
  char line[128];
  size_t kb = 0;
  while (std::fgets(line, sizeof line, f))
    if (std::sscanf(line, "MemAvailable: %zu kB", &kb) == 1) break;
  std::fclose(f);
  return kb * 1024;
}

}  // namespace

size_t Sdl2Platform::budget_for(size_t available) {
  const size_t reserve = size_t(160) << 20, low = size_t(64) << 20, high = size_t(1024) << 20;
  const size_t room = available > reserve ? (available - reserve) / 10 * 6 : 0;
  return std::max(low, std::min(high, room));
}

Sdl2Platform::Sdl2Platform() {
#ifdef TWILL_HANDHELD
  handheld_ = true;
#endif
  if (env_flag("TWILL_FULLSCREEN")) handheld_ = true;
  // Handhelds share 1 GB or so with the system: samples get a share of what's free at the start.
  // Without a reading, a cautious 256 MB.
  if (handheld_) {
    const size_t free = available_memory();
    sample_budget_ = free ? budget_for(free) : size_t(256) << 20;
  }
}

Sdl2Platform::~Sdl2Platform() {
  keep_awake(false);
  audio_close();
  if (controller_) SDL_GameControllerClose(controller_);
  else if (joystick_) SDL_JoystickClose(joystick_);
  if (texture_) SDL_DestroyTexture(texture_);
  if (renderer_) SDL_DestroyRenderer(renderer_);
  if (window_) SDL_DestroyWindow(window_);
  SDL_Quit();
}

bool Sdl2Platform::init() {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
    log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  // Handhelds keep data next to the app (the Ports folder); desktops use the per-user folder; the
  // web build, the folder web/pre.js keeps in the browser's storage.
#ifdef __EMSCRIPTEN__
  data_dir_ = "/twill/";
#else
  char* pref = handheld_ ? nullptr : SDL_GetPrefPath("Twill Studio", "Twill Studio");
  if (pref) {
    data_dir_ = pref;
    SDL_free(pref);
  } else {
    char cwd[4096];
    data_dir_ = getcwd(cwd, sizeof cwd) ? std::string(cwd) + "/" : "./";
  }
#endif
  // The SDL running, which on a handheld is the system's own; the one built against, if different.
  SDL_version run;
  SDL_GetVersion(&run);
  char built[24] = "";
  if (run.major != SDL_MAJOR_VERSION || run.minor != SDL_MINOR_VERSION || run.patch != SDL_PATCHLEVEL)
    std::snprintf(built, sizeof built, " (built with %d.%d.%d)", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
  log("SDL %d.%d.%d%s, video driver %s, audio driver %s", run.major, run.minor, run.patch, built,
      SDL_GetCurrentVideoDriver(), SDL_GetCurrentAudioDriver());
  if (sample_budget_) log("samples may take %zu MB (%zu MB free at the start)", sample_budget_ >> 20, available_memory() >> 20);

  if (handheld_) {
    // Use the panel's own mode: the highest resolution the device offers.
    SDL_DisplayMode mode;
    SDL_GetDesktopDisplayMode(0, &mode);
    window_ = SDL_CreateWindow("Twill Studio", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                               mode.w, mode.h, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    name_ = "SDL2 handheld";
  } else {
    window_ = SDL_CreateWindow("Twill Studio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280,
                               800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
#ifdef __EMSCRIPTEN__
    name_ = "SDL2 web";
#else
    name_ = "SDL2 desktop";
#endif
  }
  if (!window_) {
    log("SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer_) {
    log("accelerated renderer unavailable (%s); trying software", SDL_GetError());
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!renderer_) {
    log("SDL_CreateRenderer failed: %s", SDL_GetError());
    return false;
  }
  SDL_RendererInfo ri;
  SDL_GetRendererInfo(renderer_, &ri);
  log("renderer %s%s", ri.name, (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? " (vsync)" : "");
  SDL_ShowCursor(handheld_ ? SDL_DISABLE : SDL_ENABLE);
  update_size();
  follow_page_size();

  midi_ = make_midi();
  if (midi_) log("MIDI: %s", midi_->describe().c_str());
  open_input();
  return true;
}

std::unique_ptr<MidiPort> Sdl2Platform::make_midi() { return make_system_midi(); }

void Sdl2Platform::open_input() {
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    open_controller(i);
    if (joystick_ || controller_) break;
  }
}

void Sdl2Platform::update_size() {
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(renderer_, &w, &h);  // real pixels, including high-DPI
  // Some handhelds (the Anbernic RG28XX) have a portrait panel in a landscape case. The app draws
  // landscape and the picture is turned on its way out: the way the kernel says the panel is
  // mounted, else 270. TWILL_ROTATE=90 or 270 overrides that (0 turns it off).
  rotate_ = 0;
  if (handheld_ && h > w) {
    const int kernel = kernel_rotation();
    log("portrait panel; the kernel says it is turned %d (0: it doesn't say)", kernel);
    rotate_ = kernel ? kernel : 270;
    if (const char* r = std::getenv("TWILL_ROTATE")) rotate_ = std::atoi(r);
    if (rotate_ != 0 && rotate_ != 90 && rotate_ != 270) rotate_ = 270;
    if (rotate_) std::swap(w, h);
  }
  if (w == size_.w && h == size_.h && texture_) return;
  size_ = Size{w, h};
  if (texture_) SDL_DestroyTexture(texture_);
  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);

  if (const char* s = std::getenv("TWILL_UI_SCALE")) {
    ui_scale_ = float(std::atof(s));
  } else if (handheld_) {
    // Handheld panels are small and dense: size the UI from the panel height (768 px -> 2x).
    // Panels of 480 lines and fewer are also physically small (the RG28XX's is 2.8"), so they get
    // a little more: 480 px -> 1.5x, and the views fit themselves into the shorter screen.
    ui_scale_ = std::round(h / (h <= 480 ? 320.0f : 384.0f) * 4) / 4;
  } else {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window_, &ww, &wh);
    ui_scale_ = ww > 0 ? float(w) / ww : 1.0f;  // 2 on a Retina display
  }
  if (ui_scale_ < 1) ui_scale_ = 1;
  log("display %dx%d pixels%s, UI scale %.2f", w, h, rotate_ == 90 ? " (turned 90)" : rotate_ == 270 ? " (turned 270)" : "", ui_scale_);
}

void Sdl2Platform::present(const uint32_t* pixels, int stride, const std::vector<Rect>& dirty) {
  for (const Rect& r : dirty) {
    SDL_Rect sr{r.x, r.y, r.w, r.h};
    SDL_UpdateTexture(texture_, &sr, pixels + size_t(r.y) * stride + r.x, stride * 4);
  }
  if (rotate_) {
    // The landscape picture, centred on the portrait output and turned about its centre.
    const SDL_Rect dst{(size_.h - size_.w) / 2, (size_.w - size_.h) / 2, size_.w, size_.h};
    SDL_RenderCopyEx(renderer_, texture_, nullptr, &dst, rotate_, nullptr, SDL_FLIP_NONE);
  } else {
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  }
  SDL_RenderPresent(renderer_);
}

void Sdl2Platform::open_controller(int index) {
  const char* jname = SDL_JoystickNameForIndex(index);
  for (const KnownPad& k : kPadMaps)
    if (jname && std::strcmp(jname, k.name) == 0) {
      joystick_ = SDL_JoystickOpen(index);
      pad_map_ = k.map;
      has_pad_map_ = true;
      log("controller \"%s\" (known button map)", jname);
      return;
    }
  if (handheld_ && jname && pad_map_from_frontend(jname)) {
    joystick_ = SDL_JoystickOpen(index);
    has_pad_map_ = true;
    return;
  }
  if (SDL_IsGameController(index)) {
    controller_ = SDL_GameControllerOpen(index);
    if (controller_) {
      log("controller \"%s\" (SDL game controller mapping)", SDL_GameControllerName(controller_));
      return;
    }
  }
  joystick_ = SDL_JoystickOpen(index);
  if (joystick_) log("joystick \"%s\" has no known map; only the d-pad will work", jname ? jname : "?");
}

// KNULLI (like Batocera) keeps every controller it has been set up with in EmulationStation's
// es_input.cfg: <inputConfig deviceName="..."> holding <input name="a" type="button" id="3"/>.
// Reading it means a new device works without a build that knows it.
bool Sdl2Platform::pad_map_from_frontend(const char* joystick_name) {
  std::vector<uint8_t> data;
  const char* paths[] = {"/userdata/system/configs/emulationstation/es_input.cfg",
                         "/usr/share/emulationstation/es_input.cfg"};
  for (const char* path : paths) {
    if (!read_file(path, data)) continue;
    const std::string s(data.begin(), data.end());
    for (size_t at = s.find("<inputConfig"); at != std::string::npos; at = s.find("<inputConfig", at + 1)) {
      if (xml_attr(s, at, "type") != "joystick" || xml_attr(s, at, "deviceName") != joystick_name)
        continue;
      const size_t end = s.find("</inputConfig>", at);
      PadMap m;
      struct {
        const char* name;
        int PadMap::*field;
      } const names[] = {
          {"a", &PadMap::a},           {"b", &PadMap::b},           {"x", &PadMap::x},
          {"y", &PadMap::y},           {"pageup", &PadMap::l},      {"pagedown", &PadMap::r},
          {"l2", &PadMap::l2},         {"r2", &PadMap::r2},         {"select", &PadMap::select},
          {"start", &PadMap::start},   {"hotkey", &PadMap::menu},   {"up", &PadMap::up},
          {"down", &PadMap::down},     {"left", &PadMap::left},     {"right", &PadMap::right},
      };
      int found = 0;
      for (size_t in = s.find("<input ", at); in != std::string::npos && in < end; in = s.find("<input ", in + 1)) {
        if (xml_attr(s, in, "type") != "button") continue;  // hats and axes: the d-pad reads the hat
        const std::string name = xml_attr(s, in, "name");
        for (const auto& n : names)
          if (name == n.name) {
            m.*n.field = std::atoi(xml_attr(s, in, "id").c_str());
            ++found;
          }
      }
      if (found == 0) continue;
      pad_map_ = m;
      log("controller \"%s\" (button map from %s: A %d B %d X %d Y %d L %d R %d L2 %d R2 %d "
          "Select %d Start %d Menu %d)", joystick_name, path, m.a, m.b, m.x, m.y, m.l, m.r, m.l2,
          m.r2, m.select, m.start, m.menu);
      return true;
    }
  }
  return false;
}

const char* Sdl2Platform::controller_name() const {
  if (controller_) return SDL_GameControllerName(controller_);
  if (joystick_) return SDL_JoystickName(joystick_);
  return "keyboard";
}

uint32_t Sdl2Platform::read_pad() const {
  uint32_t b = 0;
  if (controller_) {
    struct {
      SDL_GameControllerButton sdl;
      uint32_t button;
    } const map[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, kUp},       {SDL_CONTROLLER_BUTTON_DPAD_DOWN, kDown},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, kLeft},   {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kRight},
        {SDL_CONTROLLER_BUTTON_A, kA},              {SDL_CONTROLLER_BUTTON_B, kB},
        {SDL_CONTROLLER_BUTTON_X, kX},              {SDL_CONTROLLER_BUTTON_Y, kY},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kL},   {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kR},
        {SDL_CONTROLLER_BUTTON_START, kStart},      {SDL_CONTROLLER_BUTTON_BACK, kSelect},
        {SDL_CONTROLLER_BUTTON_GUIDE, kMenu},
        // Not every handheld has a Menu (guide) button; a stick click stands in for it.
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, kMenu},   {SDL_CONTROLLER_BUTTON_RIGHTSTICK, kMenu},
    };
    for (const auto& m : map)
      if (SDL_GameControllerGetButton(controller_, m.sdl)) b |= m.button;
    if (SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= kL2;
    if (SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= kR2;
    return b;
  }
  if (!joystick_) return 0;
  if (SDL_JoystickNumHats(joystick_) > 0) {
    Uint8 hat = SDL_JoystickGetHat(joystick_, 0);
    if (hat & SDL_HAT_UP) b |= kUp;
    if (hat & SDL_HAT_DOWN) b |= kDown;
    if (hat & SDL_HAT_LEFT) b |= kLeft;
    if (hat & SDL_HAT_RIGHT) b |= kRight;
  }
  if (has_pad_map_) {
    auto btn = [&](int index, uint32_t button) {
      if (index >= 0 && SDL_JoystickGetButton(joystick_, index)) b |= button;
    };
    const PadMap& m = pad_map_;
    btn(m.a, kA); btn(m.b, kB); btn(m.x, kX); btn(m.y, kY);
    btn(m.l, kL); btn(m.r, kR); btn(m.l2, kL2); btn(m.r2, kR2);
    btn(m.select, kSelect); btn(m.start, kStart); btn(m.menu, kMenu);
    btn(m.up, kUp); btn(m.down, kDown); btn(m.left, kLeft); btn(m.right, kRight);
  }
  return b;
}

void Sdl2Platform::poll_input(InputState& out) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (take_event(e)) continue;
    switch (e.type) {
      case SDL_QUIT: quit_ = true; break;
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        for (const KeyBinding& k : kKeys)
          if (k.key == e.key.keysym.sym) {
            if (e.type == SDL_KEYDOWN) keys_ |= k.button, keyboard_last_ = true;
            else keys_ &= ~k.button;
          }
        break;
      case SDL_CONTROLLERBUTTONDOWN:  // (not stick motion: a resting stick's noise would flip it)
        keyboard_last_ = false;
        break;
      case SDL_JOYBUTTONDOWN:
        keyboard_last_ = false;
        // A record of raw button numbers, for mapping a new device from its log.
        if (handheld_ && raw_presses_logged_ < 64) {
          ++raw_presses_logged_;
          log("raw button %d", int(e.jbutton.button));
        }
        break;
      case SDL_JOYDEVICEADDED:
        if (!joystick_ && !controller_) open_controller(e.jdevice.which);
        break;
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_size();
        break;
    }
  }
  follow_page_size();
  out.held = read_buttons();
  out.quit_requested = quit_;
}

// The web build: the page lays out the canvas (web/shell.html), and the app draws at the size the
// page gives it, in real pixels, rather than drawing at a fixed size for the browser to scale.
void Sdl2Platform::follow_page_size() {
#ifdef __EMSCRIPTEN__
  double cw = 0, ch = 0;
  if (emscripten_get_element_css_size("#canvas", &cw, &ch) != EMSCRIPTEN_RESULT_SUCCESS) return;
  const int w = int(cw), h = int(ch);
  int ww = 0, wh = 0;
  SDL_GetWindowSize(window_, &ww, &wh);
  if (w > 0 && h > 0 && (w != ww || h != wh)) {
    SDL_SetWindowSize(window_, w, h);
    update_size();
  }
#endif
}

bool Sdl2Platform::audio_open(int rate, int block, AudioCallback cb, void* user) {
  audio_cb_ = cb;
  audio_user_ = user;
  SDL_AudioSpec want{}, have{};
  want.freq = rate;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  // Handhelds get longer blocks than asked for: about 21 ms of output latency, but room for the
  // scheduling jitter of a small Linux system (256 frames underran on the TrimUI Brick Pro).
  if (handheld_) block = std::max(block, 512);
#ifdef __EMSCRIPTEN__
  // In a browser the audio is made on the same thread as the drawing, between frames, so it needs
  // enough in hand to ride out a frame or two: 2048 frames is about 43 ms.
  block = std::max(block, 2048);
  // The engine runs at 48 kHz; where the browser's audio runs at another rate, SDL converts.
  const int allow = SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
#else
  // Not the frequency: the engine runs at 48 kHz, and where the device runs at another rate (stock
  // TrimUI firmware's ALSA mixer can be at 44.1 kHz) SDL converts, rather than the music playing
  // out of tune.
  const int allow = SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
#endif
  want.samples = Uint16(block);
  want.callback = &Sdl2Platform::sdl_audio_callback;
  want.userdata = this;
  audio_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, allow);
  if (!audio_dev_) {
    log("SDL_OpenAudioDevice failed: %s", SDL_GetError());
    return false;
  }
  audio_info_ = AudioInfo{have.freq, have.samples};
  if (handheld_) {
    const std::string note = compressor_.disable();
    if (!note.empty()) log("%s", note.c_str());
  }
  SDL_PauseAudioDevice(audio_dev_, 0);
  return true;
}

void Sdl2Platform::audio_close() {
  if (audio_dev_) SDL_CloseAudioDevice(audio_dev_);
  audio_dev_ = 0;
  compressor_.restore();
}

#ifdef __linux__
namespace {
bool set_real_time(bool on) {
  sched_param sp{};
  sp.sched_priority = on ? 70 : 0;
  return pthread_setschedparam(pthread_self(), on ? SCHED_FIFO : SCHED_OTHER, &sp) == 0;
}
}  // namespace
#endif

void Sdl2Platform::sdl_audio_callback(void* user, Uint8* stream, int len) {
  Sdl2Platform* self = static_cast<Sdl2Platform*>(user);
#ifdef __linux__
  // Handhelds: real-time scheduling for the audio thread, so the launcher and the Wi-Fi driver
  // can't hold it off for milliseconds. These devices run apps as root, so it's granted. (SDL's
  // own request for it was ignored on KNULLI.) Below PipeWire's own threads, which run at 83.
  if (self->handheld_ && !self->audio_rt_tried_) {
    self->audio_rt_tried_ = true;
    self->audio_rt_ = set_real_time(true);
  }
  const uint64_t start = self->micros();
#endif
  int frames = len / int(2 * sizeof(float));
  self->audio_cb_(self->audio_user_, reinterpret_cast<float*>(stream), frames);
#ifdef __linux__
  // Not while it can't keep up, though: a real-time thread with more work than a core can do would
  // leave nothing for the rest of the system (a handheld down to one slow core, asleep, may not see
  // its power button). After two seconds of late blocks it's scheduled as any thread is, until two
  // seconds of blocks have been in time.
  const int rate = self->audio_info_.sample_rate;
  if (self->handheld_ && rate > 0) {
    const bool late = (self->micros() - start) * uint64_t(rate) > uint64_t(frames) * 1000000;
    self->audio_late_run_ = late ? std::min(self->audio_late_run_ + frames, 3 * rate) : 0;
    self->audio_ok_run_ = late ? 0 : std::min(self->audio_ok_run_ + frames, 3 * rate);
    if (self->audio_rt_ && self->audio_late_run_ > 2 * rate) {
      set_real_time(false);
      self->audio_rt_ = false;
    } else if (!self->audio_rt_ && self->audio_ok_run_ > 2 * rate) {
      self->audio_rt_ = set_real_time(true);
      self->audio_ok_run_ = 0;
    }
  }
#endif
}

bool Sdl2Platform::read_file(const char* name, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(resolve(name).c_str(), "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

bool Sdl2Platform::write_file(const char* name, const std::vector<uint8_t>& data) {
  // Write a temporary file, then rename it over the old one, so a crash or power loss mid-write
  // never leaves a half-written file.
  const std::string path = resolve(name), tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = std::fflush(f) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
  if (ok) ok = std::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) std::remove(tmp.c_str());
  if (ok) persist();
  return ok;
}

bool Sdl2Platform::list_dir(const char* path, std::vector<DirEntry>& out) {
  out.clear();
  DIR* d = opendir(resolve(path).c_str());
  if (!d) return false;
  const std::string base = resolve(path);
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == ".." || name[0] == '.') continue;  // also skips macOS "._" files
    struct stat st;
    bool is_dir = stat((base + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    out.push_back(DirEntry{name, is_dir});
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
    if (a.is_dir != b.is_dir) return a.is_dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return true;
}

bool Sdl2Platform::make_dir(const char* path) {
  const std::string full = resolve(path);
  for (size_t i = 1; i <= full.size(); ++i)
    if (i == full.size() || full[i] == '/') {
      const std::string part = full.substr(0, i);
      struct stat st;
      if (stat(part.c_str(), &st) != 0 && mkdir(part.c_str(), 0755) != 0) return false;
    }
  persist();
  return true;
}

uint64_t Sdl2Platform::micros() const {
  return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency();
}

void Sdl2Platform::log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

// TWILL_KEEP_AWAKE, set by a handheld's launcher, names files (separated by ':') whose presence keeps
// that system from dimming, muting or sleeping while nobody presses a button: KNULLI's battery
// saver, for one, pauses while any /var/run/battery-saver/*.pause exists. Each holds Twill's process
// id, which muOS checks is still running. They're there only while music plays, so the device still
// saves its battery when Twill sits idle.
void Sdl2Platform::keep_awake(bool on) {
  const char* files = SDL_getenv("TWILL_KEEP_AWAKE");
  if (!files || !*files || on == awake_held_) return;
  awake_held_ = on;
  std::string list = files;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(':', start);
    if (end == std::string::npos) end = list.size();
    const std::string path = list.substr(start, end - start);
    if (!path.empty()) {
      if (on) {
        if (FILE* f = std::fopen(path.c_str(), "w")) {
          std::fprintf(f, "%d\n", int(getpid()));
          std::fclose(f);
        } else {
          log("keep awake: can't create %s", path.c_str());
        }
      } else {
        std::remove(path.c_str());
      }
    }
    start = end + 1;
  }
}

// muOS's sleep on the Anbernic H700 handhelds turns off all the cores but one, slows that one to its
// lowest speed and pauses its own emulators; any other program goes on running. TWILL_SLEEP_FILE
// names a file that exists while it's asleep.
void Sdl2Platform::wait_while_asleep() {
  const char* file = SDL_getenv("TWILL_SLEEP_FILE");
  if (!file || !*file || access(file, F_OK) != 0) return;
  if (audio_dev_) SDL_PauseAudioDevice(audio_dev_, 1);
  log("asleep");
  while (access(file, F_OK) == 0) {
    SDL_Delay(100);
    SDL_PumpEvents();
    if (SDL_HasEvent(SDL_QUIT)) break;  // shutting down: the app saves on its way out
  }
  if (audio_dev_) SDL_PauseAudioDevice(audio_dev_, 0);
  log("awake");
}

bool Sdl2Platform::can_quit() const {
#ifdef __EMSCRIPTEN__
  return false;
#else
  return true;
#endif
}

// TWILL_SELECT_START_QUITS is set by a launcher that has no helper watching for the chord.
bool Sdl2Platform::select_start_quits() const { return env_flag("TWILL_SELECT_START_QUITS"); }

// PipeWire's socket, where its clients find it: in $XDG_RUNTIME_DIR, or the system-wide places
// handheld systems run it from.
bool Sdl2Platform::prefers_long_audio_buffer() const {
  if (!handheld_) return false;
  std::vector<std::string> dirs;
  if (const char* x = SDL_getenv("XDG_RUNTIME_DIR")) dirs.push_back(x);
  dirs.push_back("/run");
  dirs.push_back("/var/run");
  for (const std::string& d : dirs) {
    struct stat st;
    if (::stat((d + "/pipewire-0").c_str(), &st) == 0) return true;
  }
  return false;
}

}  // namespace twill
