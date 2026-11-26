#include "engine/PlaitsVoice.h"

#include <cmath>
#include <cstring>
#include <new>

#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"
#include "stmlib/utils/random.h"

namespace twill {

namespace {

// Plaits computes pitch against its hardware's measured clock (plaits::kCorrectedSampleRate,
// 47872.34 Hz). Running at exactly 48 kHz, everything would be this many semitones sharp.
const float kTuningCorrection = 12.0f * std::log2(48000.0f / plaits::kCorrectedSampleRate);

// Descriptive names, in Plaits' engine order (third_party/eurorack/plaits/dsp/voice.cc).
const char* const kEngineNames[PlaitsVoice::kNumEngines] = {
    "Analog + filter", "Phase distortion", "6-op FM bank 1", "6-op FM bank 2", "6-op FM bank 3",
    "Wave terrain",    "String machine",   "Chiptune Arp",   "Virtual analog", "Waveshaper",
    "Two-op FM",       "Formant",          "Additive",       "Wavetable",      "Chords",
    "Speech",          "Swarm",            "Filtered noise", "Particles",      "Plucked string",
    "Modal resonator", "Bass drum",        "Snare drum",     "Hi-hat",
};

}  // namespace

const char* PlaitsVoice::engine_name(int engine) {
  return engine >= 0 && engine < kNumEngines ? kEngineNames[engine] : "?";
}

void PlaitsVoice::seed_random(uint32_t seed) { stmlib::Random::Seed(seed); }

PlaitsVoice::PlaitsVoice() {
  // Start from zeroed memory: parts of the DSP read state before writing it, and leftover bytes
  // from earlier allocations would otherwise leak faintly into the sound (and make renders differ
  // from run to run).
  void* mem = ::operator new(sizeof(plaits::Voice));
  std::memset(mem, 0, sizeof(plaits::Voice));
  voice_ = new (mem) plaits::Voice;
  std::memset(buffer_, 0, sizeof buffer_);
  stmlib::BufferAllocator allocator(buffer_, sizeof buffer_);
  voice_->Init(&allocator);
}

PlaitsVoice::~PlaitsVoice() {
  voice_->~Voice();
  ::operator delete(voice_);
}

void PlaitsVoice::render_block(const MacroParams& p) {
  plaits::Patch patch = {};
  patch.note = p.note - kTuningCorrection;
  patch.harmonics = p.harmonics;
  patch.timbre = p.timbre;
  patch.morph = p.morph;
  patch.frequency_modulation_amount = p.fm_amount;
  patch.timbre_modulation_amount = p.timbre_amount;
  patch.morph_modulation_amount = p.morph_amount;
  patch.engine = p.engine;
  patch.decay = p.decay;
  patch.lpg_colour = p.lpg_colour;
  // The FM engines' bank: reloaded only when it changes (or the engine does).
  voice_->set_user_fm_bank(p.fm_bank);

  // The chiptune engine steps its arpeggio at each trigger: while a note is held, a new edge every
  // arp_step samples (the gate dropped for one block, as for a retriggered note).
  if (restart_arp_) {
    voice_->chiptune_engine().restart_arpeggio();
    restart_arp_ = false;
  }
  if (p.engine == 7 && p.arp_step > 0 && gate_) {
    arp_pos_ += kBlockSize;
    if (arp_pos_ >= p.arp_step) {
      arp_pos_ = std::fmod(arp_pos_, p.arp_step);
      retrigger_ = true;
    }
  }

  plaits::Modulations mod = {};
  mod.trigger_patched = true;
  mod.trigger = gate_ && !retrigger_ ? 1.0f : 0.0f;
  retrigger_ = false;
  const bool was_high = rendered_high_;
  rendered_high_ = mod.trigger > 0;
  mod.level_patched = !p.use_lpg;
  mod.level = p.level;
  if (!p.use_lpg) {
    // The ADSR in place of the internal envelope, as if patched into the FM, timbre and morph
    // inputs; the frequency one curved and scaled like the internal envelope (squared, 48 semitones).
    // Two engines use those amounts for something else, kept as they are: speech's FM and morph
    // amounts (prosody and speed) and chiptune's timbre amount (its own envelope's shape).
    const float e = p.envelope;
    if (p.engine != 15) {
      mod.frequency_patched = mod.morph_patched = true;
      mod.frequency = e * e * 48.0f;
      mod.morph = e;
    }
    if (p.engine != 7) {
      mod.timbre_patched = true;
      mod.timbre = e;
    }
  }

  plaits::Voice::Frame frames[kBlockSize];
  voice_->Render(patch, mod, frames, kBlockSize);
  // Triggered, the chiptune engine makes its own envelope and passes the low-pass gate by, and that
  // envelope only shapes each arpeggio step (with the timbre amount; at 0 it holds): left alone a
  // note would never end. So under the gate setting the gate's part is played here: each trigger
  // (a note, or an arpeggio step) rises to full in a millisecond, without a click, then dies away
  // as the other engines do under the gate, held or not: to -60 dB in about 0.9 s at a decay of
  // 0.25, 2 s at 0.5, 5.5 s at 0.75 and 14 s at 1.
  float from = 1, to = 1;
  if (p.engine == 7 && p.use_lpg) {
    if (mod.trigger > 0 && !was_high) chip_attack_ = true;
    from = chip_env_;
    if (chip_attack_) {
      chip_env_ += kBlockSize / (0.001f * kSampleRate);
      if (chip_env_ >= 1) chip_env_ = 1, chip_attack_ = false;
    } else {
      const float t60 = 0.35f * std::pow(10.0f, 1.6f * p.decay);
      chip_env_ *= std::exp(-6.9f * kBlockSize / (t60 * kSampleRate));
      if (chip_env_ < 1e-5f) chip_env_ = 0;
    }
    to = chip_env_;
  }
  for (int i = 0; i < kBlockSize; ++i) {
    const float g = from + (to - from) * float(i + 1) / kBlockSize;
    block_out_[i] = frames[i].out * (g / 32768.0f);
    block_aux_[i] = frames[i].aux * (g / 32768.0f);
  }
  block_pos_ = 0;
}

void PlaitsVoice::render(const MacroParams& p, float* out, float* aux, int frames) {
  for (int i = 0; i < frames; ++i) {
    if (block_pos_ == kBlockSize) render_block(p);
    if (out) out[i] = block_out_[block_pos_];
    if (aux) aux[i] = block_aux_[block_pos_];
    ++block_pos_;
  }
}

}  // namespace twill
