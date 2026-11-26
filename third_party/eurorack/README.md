# Vendored eurorack code

The macro-oscillator DSP and the parts of `stmlib` it uses, copied from Emilie Gillet's eurorack
repository, keeping their original paths, and unmodified except for the change listed below:

- `plaits/` from https://github.com/pichenettes/eurorack at commit `f4cfa76` (2026-10-12): `plaits/dsp/`,
  `plaits/resources.{h,cc}` and `plaits/user_data.h`. Only the files the DSP needs.
- `clouds/` from the same eurorack commit: `clouds/dsp/fx/reverb.h`, `clouds/dsp/fx/fx_engine.h` and
  `clouds/dsp/frame.h`, the reverb used for the send effect.
- `stmlib/` from https://github.com/pichenettes/stmlib at commit `e3bd7c9` (2023-05-30), the commit the
  eurorack repository pins: 13 headers plus `dsp/units.cc` and `utils/random.cc`.

Licence: MIT (the STM32 projects in that repository), see `LICENSE` here and `stmlib/LICENSE`; every
file also carries its own notice.

Two local changes, marked "Twill Studio" in the code:

- The chiptune engine can restart its arpeggio (`ChiptuneEngine::restart_arpeggio()`, reached through
  `Voice::chiptune_engine()`), so that each note begins on its own pitch. Upstream, every trigger steps
  the arpeggio on from wherever it was.
- A voice can be given a DX7 bank loaded from a file (`Voice::set_user_fm_bank()`), which the six-op FM
  engines play in place of their built-in banks (`plaits/dsp/voice.{h,cc}`). Upstream reads user banks
  from the module's flash, through `user_data.h`, which a `-DTEST` build turns off.

Built with `-DTEST`, which swaps 32-bit ARM inline assembly for portable C and makes
`plaits/user_data.h` return no user data (the six-op FM engines then use their built-in DX7 banks).

The upstream README asks that derivative works not use the "Mutable Instruments" name (a registered
trademark) and recommends not keeping the original module names. The app's user-facing names for this
instrument and its engines must follow that; the code keeps the upstream names internally.
