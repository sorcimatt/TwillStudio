#include "model/FmBank.h"

#include <cctype>

namespace twill {

namespace {

constexpr size_t kHeader = 6;                       // F0 43 0n 09 20 00
constexpr size_t kDumpBytes = kHeader + kFmBankBytes + 2;  // + checksum, F7

bool is_bank_header(const uint8_t* p) {
  return p[0] == 0xF0 && p[1] == 0x43 && (p[2] & 0xF0) == 0x00 && p[3] == 0x09 && p[4] == 0x20 && p[5] == 0x00;
}

uint8_t checksum(const uint8_t* voices) {
  unsigned sum = 0;
  for (int i = 0; i < kFmBankBytes; ++i) sum += voices[i];
  return uint8_t((0x80 - (sum & 0x7F)) & 0x7F);
}

}  // namespace

bool is_fm_bank_file(const std::string& name) {
  if (name.size() < 4) return false;
  std::string ext = name.substr(name.size() - 4);
  for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".syx";
}

bool parse_fm_bank(const uint8_t* data, size_t size, std::vector<uint8_t>& bank, std::string& error,
                   std::string* warning) {
  if (warning) warning->clear();
  if (!data || size == 0) {
    error = "The file is empty";
    return false;
  }
  // A bulk dump anywhere in the file (some files hold other messages first).
  for (size_t i = 0; i + kDumpBytes <= size; ++i) {
    if (!is_bank_header(data + i)) continue;
    const uint8_t* voices = data + i + kHeader;
    if (data[i + kHeader + kFmBankBytes + 1] != 0xF7) continue;
    bank.assign(voices, voices + kFmBankBytes);
    if (warning && data[i + kHeader + kFmBankBytes] != checksum(voices))
      *warning = "The bank's checksum is wrong; loaded anyway";
    return true;
  }
  // The voices on their own, without the SysEx around them.
  if (size == size_t(kFmBankBytes) && data[0] != 0xF0) {
    bank.assign(data, data + kFmBankBytes);
    return true;
  }
  // A single voice (F0 43 0n 00 01 1B, 155 bytes, a checksum, F7) is a different format.
  if (size >= 6 && data[0] == 0xF0 && data[1] == 0x43 && data[3] == 0x00 && data[4] == 0x01 && data[5] == 0x1B)
    error = "That's a single voice; the FM engines need a 32-voice bank";
  else
    error = "Not a DX7 32-voice bank";
  return false;
}

std::string fm_bank_title(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name.erase(dot);
  return name;
}

std::string fm_voice_name(const uint8_t* bank, int index) {
  if (!bank || index < 0 || index >= kFmBankVoices) return std::string();
  std::string name;
  const uint8_t* n = bank + index * kFmVoiceBytes + 118;
  for (int i = 0; i < 10; ++i) {
    char c = char(n[i] & 0x7F);
    if (c < 32 || c >= 127) c = ' ';
    if (c == ' ' && !name.empty() && name.back() == ' ') continue;  // "PIPES   2" -> "PIPES 2"
    name += c;
  }
  while (!name.empty() && name.back() == ' ') name.pop_back();
  size_t first = name.find_first_not_of(' ');
  return first == std::string::npos ? std::string() : name.substr(first);
}

std::vector<uint8_t> make_fm_bank_sysex(const uint8_t* bank) {
  std::vector<uint8_t> out = {0xF0, 0x43, 0x00, 0x09, 0x20, 0x00};
  out.insert(out.end(), bank, bank + kFmBankBytes);
  out.push_back(checksum(bank));
  out.push_back(0xF7);
  return out;
}

}  // namespace twill
