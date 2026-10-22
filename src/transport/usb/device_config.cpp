#include "device_config.hpp"

#include <zlib.h>
#include <array>
#include <vector>

namespace lh::usb {

bool ReadDeviceConfig(const FeatureReader& read, std::string* json, std::string* error) {
  const auto fail = [&](const char* text) { if (error) *error = text; return false; };
  if (!read || !json) return fail("invalid config reader/output");
  std::array<uint8_t, 64> report{};
  report[0] = 0x10;  // start/reset the configuration stream (GET_FEATURE)
  int n = read(report.data(), report.size());
  if (n < 3 || n > int(report.size()) || report[0] != 0x10) return fail("config readmode failed");
  const size_t expected = size_t(report[1]) * 256 + report[2];
  std::vector<uint8_t> compressed;
  for (int chunks = 0; chunks < 2048; ++chunks) {
    report.fill(0); report[0] = 0x11;
    n = read(report.data(), report.size());
    if (n < 2 || n > int(report.size()) || report[0] != 0x11 || report[1] > n - 2)
      return fail("invalid config chunk");
    const size_t length = report[1];
    if (length == 0) {
      if (compressed.empty() || compressed.size() < expected) return fail("truncated config stream");
      std::string output(65536, '\0');
      uLongf size = output.size();
      const int result = uncompress(reinterpret_cast<Bytef*>(output.data()), &size,
                                    compressed.data(), compressed.size());
      if (result != Z_OK) return fail("invalid/oversized compressed config");
      output.resize(size);
      const auto begin = output.find_first_not_of(" \t\r\n");
      if (begin == std::string::npos || output[begin] != '{') return fail("config is not a JSON object");
      *json = std::move(output);
      if (error) error->clear();
      return true;
    }
    if (compressed.size() + length > 65536) return fail("config stream too large");
    compressed.insert(compressed.end(), report.begin() + 2, report.begin() + 2 + length);
  }
  return fail("config stream did not terminate");
}

}  // namespace lh::usb
