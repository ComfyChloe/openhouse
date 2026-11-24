// settings.json "doormanHomeChannels" (runtime/doorman_home_channels.hpp).
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "runtime/doorman_home_channels.hpp"

namespace {
using Homes = std::map<std::string, uint16_t>;

Homes Parse(const char* text, std::vector<std::string>* problems) {
  return lh::runtime::DoormanHomeChannels(nlohmann::json::parse(text), problems);
}

TEST(DoormanHomeChannels, AbsentMeansNone) {
  std::vector<std::string> problems;
  EXPECT_EQ(Parse(R"({"hideLostDevices": true})", &problems), Homes{});
  EXPECT_TRUE(problems.empty());
}

TEST(DoormanHomeChannels, HexStringsOrNumbersByUsbSerial) {
  std::vector<std::string> problems;
  const Homes homes = Parse(R"({"doormanHomeChannels": {
      "0123456789ABCDEF": "0x00FF", "fedcba9876543210": "0xff00", "OHDG-00000000000000AA": 4095, "BB": "0XFFFF"}})",
                            &problems);
  EXPECT_EQ(homes, (Homes{{"0123456789ABCDEF", 0x00FF}, {"FEDCBA9876543210", 0xFF00}, {"00000000000000AA", 0x0FFF},
                          {"BB", 0xFFFF}}));
  EXPECT_TRUE(problems.empty());
}

// A wrong entry is left out (and said); the others still count. How many channels a mask has is
// checked where it's sent.
TEST(DoormanHomeChannels, WhatIsNotAMaskIsLeftOut) {
  std::vector<std::string> problems;
  const Homes homes = Parse(R"({"doormanHomeChannels": {
      "A1": "0x1FFFF", "A2": "zz", "A3": 70000, "A4": -1, "A5": true, "A6": "", "A7": "0x", "A8": 255.5,
      "A9": "0x000F", "OHDG-": "0x00FF", "A10": "00FF", "A11": "255"}})",
                            &problems);
  EXPECT_EQ(homes, (Homes{{"A9", 0x000F}}));
  EXPECT_EQ(problems.size(), 11u);
  ASSERT_FALSE(problems.empty());
  EXPECT_EQ(problems[0], "doormanHomeChannels: A1: \"0x1FFFF\" isn't a channel mask (\"0x0000\" to \"0xFFFF\"); left out");
}

// Two keys for one dongle (case, or the OHDG- prefix): the first is kept and the other is said, not
// silently dropped.
TEST(DoormanHomeChannels, TwoKeysForOneDongleKeepOne) {
  std::vector<std::string> problems;
  const Homes homes = Parse(R"({"doormanHomeChannels": {"ABC": "0x00FF", "OHDG-abc": "0xFF00"}})", &problems);
  EXPECT_EQ(homes, (Homes{{"ABC", 0x00FF}}));
  ASSERT_EQ(problems.size(), 1u);
  EXPECT_EQ(problems[0], "doormanHomeChannels: OHDG-abc: another key names the same dongle; left out");
}

TEST(DoormanHomeChannels, NotAnObject) {
  std::vector<std::string> problems;
  EXPECT_EQ(Parse(R"({"doormanHomeChannels": "0x00FF"})", &problems), Homes{});
  EXPECT_EQ(problems.size(), 1u);
}

}  // namespace
