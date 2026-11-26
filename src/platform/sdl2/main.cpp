// Entry point for SDL2 builds.
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "app/App.h"
#include "app/Splash.h"
#include "gfx/Canvas.h"
#include "platform/sdl2/Sdl2Platform.h"
#ifdef TWILL_SNDSTATION
#include "platform/sndstation/SndstationPlatform.h"
#endif

namespace {
// Writes the canvas as a binary PPM (for TWILL_SCREENSHOT test captures).
bool write_ppm(const twill::Canvas& c, const char* path) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fprintf(f, "P6 %d %d 255\n", c.width(), c.height());
  for (int i = 0; i < c.width() * c.height(); ++i) {
    uint32_t p = c.pixels()[i];
    unsigned char rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
    std::fwrite(rgb, 1, 3, f);
  }
  return std::fclose(f) == 0;
}

// The app's frame loop, one frame at a time: desktops and handhelds call it in a loop, a browser
// once per animation frame.
struct Loop {
  Loop(twill::Sdl2Platform& p, twill::App& a, twill::Canvas& c, const char* shot = nullptr)
      : platform(p), app(a), canvas(c), screenshot(shot) {}
  twill::Sdl2Platform& platform;
  twill::App& app;
  twill::Canvas& canvas;
  const char* screenshot;
  twill::InputState input;
  int frames = 0, frames_shown = 0, idle_frames = 0;

  // False once the app has quit.
  bool step() {
    platform.poll_input(input);
    twill::Size size = platform.display_size();
    if (size.w != canvas.width() || size.h != canvas.height()) canvas.resize(size.w, size.h);

    if (!app.frame(input, canvas)) return false;
    if (screenshot && ++frames == 60) {
      platform.log(write_ppm(canvas, screenshot) ? "saved %s" : "could not write %s", screenshot);
      return false;
    }
    std::vector<twill::Rect> dirty = canvas.take_dirty();
    // Nothing changed: keep the last frame on screen rather than presenting it again, except that
    // it is presented every frame for the first two seconds and then once a second. Some drivers
    // drop the first frames (the RG28XX's mali driver showed a blank screen until a button press
    // redrew it), and other things can draw over the screen; the frame is already on the GPU.
    if (!dirty.empty() || frames_shown < 120 || ++idle_frames >= 60) {
      platform.present(canvas.pixels(), canvas.stride(), dirty);
      idle_frames = 0;
    }
    if (frames_shown < 120) ++frames_shown;
    return true;
  }
};

constexpr uint64_t kSplashMicros = 1500000;  // the splash stays up at least this long

#ifdef __EMSCRIPTEN__
// The browser runs the app one frame at a time and must get control back between frames, so the
// start-up is spread over frames too: the splash goes up, then the app starts, then runs.
struct Web {
  twill::Sdl2Platform platform;
  twill::Canvas canvas;
  std::unique_ptr<twill::App> app;
  std::unique_ptr<Loop> loop;
  uint64_t splash_start = 0;
  int splash_frames = 0;
};

void web_frame(void* user) {
  Web& w = *static_cast<Web*>(user);
  const bool splash = !w.app || w.platform.micros() - w.splash_start < kSplashMicros;
  if (splash) {
    w.platform.present(w.canvas.pixels(), w.canvas.stride(), {twill::Rect{0, 0, w.canvas.width(), w.canvas.height()}});
    // Start the app once the splash has been shown (starting loads the kit and the last song).
    if (!w.app && ++w.splash_frames >= 2) {
      w.app = std::make_unique<twill::App>(w.platform);
      w.app->start();
      w.loop = std::make_unique<Loop>(w.platform, *w.app, w.canvas);
    }
    return;
  }
  if (!w.loop->step()) {
    w.app->stop();
    emscripten_cancel_main_loop();
    EM_ASM({ if (Module.twillStopped) Module.twillStopped(); });
  }
}
#endif
}  // namespace

int main(int, char**) {
#ifdef __EMSCRIPTEN__
  Web* web = new Web;  // lives as long as the page
  if (!web->platform.init()) return 1;
  const twill::Size size = web->platform.display_size();
  web->canvas.resize(size.w, size.h);
  twill::draw_splash(web->canvas);
  web->splash_start = web->platform.micros();
  emscripten_set_main_loop_arg(web_frame, web, 0, false);
  return 0;
#else
#ifdef TWILL_SNDSTATION
  twill::SndstationPlatform platform;
#else
  twill::Sdl2Platform platform;
#endif
  if (!platform.init()) return 1;
  // Test hook: TWILL_SCREENSHOT=<file.ppm> saves a frame after about a second, then quits.
  const char* screenshot = std::getenv("TWILL_SCREENSHOT");

  // The splash, while the app starts (loading the factory kit and the last song), and for at least
  // a second and a half in all. Presented a few times: some drivers drop the first frames.
  twill::Canvas canvas;
  const uint64_t splash_start = platform.micros();
  if (!screenshot) {
    const twill::Size size = platform.display_size();
    canvas.resize(size.w, size.h);
    twill::draw_splash(canvas);
    for (int i = 0; i < 3; ++i) {
      platform.present(canvas.pixels(), canvas.stride(), canvas.take_dirty());
      SDL_Delay(16);
    }
  }
  twill::App app(platform);
  app.start();
  while (!screenshot && platform.micros() - splash_start < kSplashMicros) {
    platform.present(canvas.pixels(), canvas.stride(), {twill::Rect{0, 0, canvas.width(), canvas.height()}});
    SDL_Delay(16);
  }
  // Test hook: TWILL_VIEW=roll (or sound, mix) starts in that view.
  if (const char* v = std::getenv("TWILL_VIEW")) {
    const std::string view = v;
    if (view == "roll") app.show(twill::App::View::PianoRoll);
    if (view == "sound") app.show(twill::App::View::Instrument);
    if (view == "mix") app.show(twill::App::View::Mixer);
  }

  Loop loop(platform, app, canvas, screenshot);
  for (;;) {
    platform.wait_while_asleep();
    const uint64_t t0 = platform.micros();
    if (!loop.step()) break;
    // Vsync normally paces the loop; without it (or when idle), cap at about 60 frames a second.
    uint64_t spent = platform.micros() - t0;
    if (spent < 15000) SDL_Delay(Uint32((16667 - spent) / 1000));
  }
  app.stop();
  return 0;
#endif
}
