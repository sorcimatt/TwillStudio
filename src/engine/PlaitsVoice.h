// One voice of the macro-oscillator instrument: wraps the vendored Plaits DSP
// (third_party/eurorack), renders float audio at 48 kHz in any block size, and corrects its tuning.
//
// User-facing names for this instrument must not be "Plaits" or "Mutable Instruments" (see
// third_party/eurorack/README.md); engine_name() gives descriptive names instead.
#pragma once
#include <cstddef>
#include <cstdint>

namespace plaits {
class Voice;
}

namespace twill {

struct MacroParams {
  int engine = 8;            // 0-23, see engine_name()
  float note = 60;           // MIDI note number, fractional for pitch bends
  float harmonics = 0.5f;    // 0-1
  float timbre = 0.5f;       // 0-1
  float morph = 0.5f;        // 0-1
  float decay = 0.5f;        // 0-1, the internal envelope and low-pass gate
  float lpg_colour = 0.5f;   // 0-1
  float fm_amount = 0;       // -1..1, internal envelope to frequency
  float timbre_amount = 0;   // -1..1, internal envelope to timbre
  float morph_amount = 0;    // -1..1, internal envelope to morph
  bool use_lpg = true;       // true: each trigger runs the internal low-pass gate envelope
  float level = 1;           // used only when use_lpg is false (an external envelope drives it)
  // Without the gate (the ADSR in charge): the ADSR's level, 0-1, which the FM, timbre and morph
  // amounts then follow in place of the internal decay envelope.
  float envelope = 0;
  // The chiptune engine's arpeggio: one step every this many samples while a note is held (0: it
  // doesn't step). Each note starts it again on the note's own pitch.
  float arp_step = 0;
  // The six-op FM engines' bank: a DX7 32-voice bank (model/FmBank.h), or null for the built-in
  // ones. Kept alive by whoever owns it (the app's bank cache) while a voice may play it.
  const uint8_t* fm_bank = nullptr;
};

class PlaitsVoice {
 public:
  static constexpr int kNumEngines = 24;
  static constexpr int kSampleRate = 48000;
  static constexpr int kBlockSize = 12;  // Plaits renders in blocks of this many samples
  static const char* engine_name(int engine);
  // The DSP draws noise from one generator shared by all voices. Seeding it makes renders repeat.
  static void seed_random(uint32_t seed);

  PlaitsVoice();
  ~PlaitsVoice();
  PlaitsVoice(const PlaitsVoice&) = delete;
  PlaitsVoice& operator=(const PlaitsVoice&) = delete;

  // Gate: high from note_on until note_off, as the module's trigger input sees a held gate. Engines
  // with their own envelopes (the six-op FM banks, for one) sustain while the gate is high. A
  // note_on while the gate is high, or when it went low since the last block rendered (a note
  // ending exactly where the next begins), lowers it for one block first, so Plaits sees a new edge.
  void note_on() {
    retrigger_ = rendered_high_;
    gate_ = true;
    arp_pos_ = 0;
    restart_arp_ = true;
  }
  void note_off() { gate_ = false; }
  // Legato: the gate stays (or goes back) high with no new edge, so nothing retriggers.
  void legato_on() {
    gate_ = true;
    retrigger_ = false;
  }

  // Renders `frames` samples of the main and auxiliary outputs, as floats in -1..1. Either output
  // pointer may be null. Any frame count works; partial blocks are carried over between calls.
  void render(const MacroParams& p, float* out, float* aux, int frames);

 private:
  void render_block(const MacroParams& p);

  plaits::Voice* voice_;
  alignas(16) uint8_t buffer_[16384];  // the working memory each voice needs
  float block_out_[kBlockSize], block_aux_[kBlockSize];
  int block_pos_ = kBlockSize;  // samples of the current block already handed out
  bool gate_ = false;
  bool retrigger_ = false;  // drop the gate for one block before the next one
  bool rendered_high_ = false;  // the gate Plaits saw in the last block
  bool restart_arp_ = false;    // the next block starts the chiptune arpeggio again
  float arp_pos_ = 0;           // samples since the arpeggio's last step
  float chip_env_ = 0;          // the chiptune engine's level under the low-pass gate setting
  bool chip_attack_ = false;    // rising to full after a trigger
};

}  // namespace twill
