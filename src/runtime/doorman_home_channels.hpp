// settings.json "doormanHomeChannels": the hop channels each Doorman Dongle may use (dongle cmd 0x36),
// by the dongle's USB serial: {"0123456789ABCDEF": "0x00FF", "FEDCBA9876543210": "0xFF00"}. The dongle
// keeps them in RAM only; the transport sends them each time it opens the dongle
// (DoormanDongle::SetHomeChannels).
#pragma once

#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lh::runtime {

// The masks by USB serial in upper case ("OHDG-" in front is allowed: the receiver name). A mask is a
// string "0x" + 1-4 hex digits ("0x00FF"; JSON has no hex numbers, and "255" would be ambiguous) or a
// number 0-65535. An entry that isn't one, or a second key for the same dongle (serials differing only
// in case), is left out with a line in *problems. How many channels a mask has is checked where it's
// sent (fewer than 8: not sent).
inline std::map<std::string, uint16_t> DoormanHomeChannels(const nlohmann::json& settings,
                                                           std::vector<std::string>* problems) {
  std::map<std::string, uint16_t> out;
  const auto it = settings.find("doormanHomeChannels");
  if (it == settings.end()) return out;
  if (!it->is_object()) {
    problems->push_back("doormanHomeChannels must be an object: {\"<dongle serial>\": \"0x00FF\"}");
    return out;
  }
  for (const auto& [key, value] : it->items()) {
    std::string serial = key;
    for (char& c : serial) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (serial.compare(0, 5, "OHDG-") == 0) serial.erase(0, 5);
    std::optional<uint32_t> mask;
    if (value.is_number_unsigned()) {
      mask = value.get<uint64_t>() <= 0xFFFF ? std::optional<uint32_t>(uint32_t(value.get<uint64_t>())) : std::nullopt;
    } else if (value.is_string()) {
      const std::string& text = value.get_ref<const std::string&>();
      const std::string digits = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')
                                     ? text.substr(2) : std::string();
      if (!digits.empty() && digits.size() <= 4 &&
          digits.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        mask = uint32_t(std::stoul(digits, nullptr, 16));
    }
    if (serial.empty() || !mask) {
      problems->push_back("doormanHomeChannels: " + key + ": " + value.dump() +
                          " isn't a channel mask (\"0x0000\" to \"0xFFFF\"); left out");
      continue;
    }
    if (!out.emplace(serial, uint16_t(*mask)).second) {
      problems->push_back("doormanHomeChannels: " + key + ": another key names the same dongle; left out");
      continue;
    }
  }
  return out;
}

}  // namespace lh::runtime
