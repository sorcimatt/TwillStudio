// DX7 voice banks for the six-op FM engines: a track can play its own bank, read from a SysEx file,
// in place of the engine's built-in one. The same files that DX7 editors (Dexed, for one) and
// DX7-compatible synths read and write.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace twill {

constexpr int kFmBankVoices = 32;
constexpr int kFmVoiceBytes = 128;                            // one voice, packed as in a bulk dump
constexpr int kFmBankBytes = kFmBankVoices * kFmVoiceBytes;  // 4096

// Whether a file name looks like a SysEx file (".syx", any case).
bool is_fm_bank_file(const std::string& name);

// Reads a 32-voice bank: a DX7 bulk dump (F0 43 0n 09 20 00, 4096 bytes of voices, a checksum, F7),
// found anywhere in the file, or the 4096 bytes of voices on their own. Fills `bank` with the voices
// and returns true; otherwise says why in `error`. A wrong checksum is accepted (many banks shared
// online have one), and noted in `warning`.
bool parse_fm_bank(const uint8_t* data, size_t size, std::vector<uint8_t>& bank, std::string& error,
                   std::string* warning = nullptr);

// Voice `index`'s name (0-31) in a bank of kFmBankBytes, trimmed: "E.PIANO 1".
std::string fm_voice_name(const uint8_t* bank, int index);

// A bank file's name as shown: without its folder and extension ("banks/Rhodes.syx" -> "Rhodes").
std::string fm_bank_title(const std::string& path);

// A bulk dump of `bank` (kFmBankBytes), as a .syx file holds it, on MIDI channel 1.
std::vector<uint8_t> make_fm_bank_sysex(const uint8_t* bank);

}  // namespace twill
