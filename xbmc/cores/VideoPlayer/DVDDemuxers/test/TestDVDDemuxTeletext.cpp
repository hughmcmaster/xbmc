/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDDemuxers/DVDDemuxTeletext.h"
#include "cores/VideoPlayer/DVDDemuxers/DVDDemuxUtils.h"
#include "video/TeletextDefines.h"

#include <array>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace
{
using Rows = std::array<std::array<uint8_t, 40>, 24>;
using Valid = std::array<bool, 24>;

void SetRow(Rows& rows, Valid& valid, int row, const std::string& content)
{
  rows[row].fill(0x20);
  std::memcpy(rows[row].data(), content.data(), std::min<size_t>(content.size(), 40));
  valid[row] = true;
}

constexpr std::array<uint8_t, 16> HAMMING84 = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
                                               0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};

// Bytes are transmitted LSB first
uint8_t Reverse(uint8_t b)
{
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

// Build a 46 byte EN 300 472 data unit for the given magazine and packet
std::vector<uint8_t> DataUnit(int magazine, int packet, const std::vector<uint8_t>& payload)
{
  std::vector<uint8_t> unit = {0x03, 0x2C, 0xE4, 0xE4};
  const uint8_t address0 = (magazine & 7) | ((packet & 1) << 3);
  const uint8_t address1 = packet >> 1;
  unit.push_back(Reverse(HAMMING84[address0]));
  unit.push_back(Reverse(HAMMING84[address1]));
  for (uint8_t b : payload)
    unit.push_back(Reverse(b));
  unit.resize(46, Reverse(0x20));
  return unit;
}

std::vector<uint8_t> Header(int magazine, int page, bool subtitle, bool erase)
{
  std::vector<uint8_t> payload;
  payload.push_back(HAMMING84[page & 0x0F]);
  payload.push_back(HAMMING84[(page >> 4) & 0x0F]);
  payload.push_back(HAMMING84[0]); // subcode S1
  payload.push_back(HAMMING84[erase ? 0x08 : 0x00]); // S2 + C4
  payload.push_back(HAMMING84[0]); // S3
  payload.push_back(HAMMING84[subtitle ? 0x08 : 0x00]); // S4 + C5..C6
  payload.push_back(HAMMING84[0]); // C7..C10
  payload.push_back(HAMMING84[0]); // C11..C14, national option 0 (English)
  return DataUnit(magazine, 0, payload);
}

std::vector<uint8_t> TextRow(int magazine, int row, const std::string& text)
{
  return DataUnit(magazine, row, std::vector<uint8_t>(text.begin(), text.end()));
}

DemuxPacket* Packet(const std::vector<std::vector<uint8_t>>& units, double pts)
{
  std::vector<uint8_t> data = {0x10};
  for (const auto& unit : units)
    data.insert(data.end(), unit.begin(), unit.end());

  DemuxPacket* packet = CDVDDemuxUtils::AllocateDemuxPacket(static_cast<int>(data.size()));
  std::memcpy(packet->pData, data.data(), data.size());
  packet->iSize = static_cast<int>(data.size());
  packet->pts = pts;
  return packet;
}

std::string Text(DemuxPacket* packet)
{
  return std::string(reinterpret_cast<char*>(packet->pData), packet->iSize);
}
} // unnamed namespace

TEST(TestDVDDemuxTeletext, BoxedTextOnly)
{
  Rows rows{};
  Valid valid{};
  SetRow(rows, valid, 0, "not boxed");
  SetRow(rows, valid, 19,
         std::string("\x0b\x0b"
                     "Hello world  \x0a\x0a",
                     17));
  SetRow(rows, valid, 20,
         std::string("\x0b\x0b"
                     "Second line\x0a\x0a",
                     15));

  EXPECT_EQ(CDVDDemuxTeletext::PageToText(rows, valid, NAT_UK, NAT_UK), "Hello world\nSecond line");
}

TEST(TestDVDDemuxTeletext, ColourTags)
{
  Rows rows{};
  Valid valid{};
  SetRow(rows, valid, 20,
         std::string("\x0b\x0b\x03"
                     "Yellow \x07white",
                     16));

  EXPECT_EQ(CDVDDemuxTeletext::PageToText(rows, valid, NAT_UK, NAT_UK),
            "<font color=#FCC024>Yellow</font> white");
}

TEST(TestDVDDemuxTeletext, DoubleHeightSkipsNextRow)
{
  Rows rows{};
  Valid valid{};
  SetRow(rows, valid, 20,
         std::string("\x0b\x0b\x0d"
                     "Tall",
                     7));
  SetRow(rows, valid, 21,
         std::string("\x0b\x0b"
                     "Hidden",
                     8));
  SetRow(rows, valid, 22,
         std::string("\x0b\x0b"
                     "Shown",
                     7));

  EXPECT_EQ(CDVDDemuxTeletext::PageToText(rows, valid, NAT_UK, NAT_UK), "Tall\nShown");
}

TEST(TestDVDDemuxTeletext, NationalCharacters)
{
  Rows rows{};
  Valid valid{};
  // 0x5B..0x5D map to Ä Ö Ü in the German subset, 0x7F is the block graphic
  SetRow(rows, valid, 20, std::string("\x0b\x0b\x5b\x5c\x5d\x7f", 6));

  EXPECT_EQ(CDVDDemuxTeletext::PageToText(rows, valid, NAT_DE, NAT_DE), "ÄÖÜ■");
}

TEST(TestDVDDemuxTeletext, CyrillicG0Set)
{
  Rows rows{};
  Valid valid{};
  // Russian/Bulgarian option maps latin positions to cyrillic letters
  SetRow(rows, valid, 20,
         std::string("\x0b\x0b"
                     "A",
                     3));

  EXPECT_EQ(CDVDDemuxTeletext::PageToText(rows, valid, NAT_RB, NAT_RB), "А");
}

TEST(TestDVDDemuxTeletext, EscapeSwitchesSecondarySubset)
{
  Rows rows{};
  Valid valid{};
  SetRow(rows, valid, 20, std::string("\x0b\x0b\x5b\x1b\x5b", 5));

  EXPECT_EQ(CDVDDemuxTeletext::PageToText(rows, valid, NAT_DE, NAT_UK), "Ä ←");
}

TEST(TestDVDDemuxTeletext, ParseDescriptor)
{
  // magazine 8 page 88 subtitle, magazine 1 page 00 initial, magazine 7 page 77 hearing impaired
  const uint8_t extradata[] = {(0x02 << 3) | 0x00, 0x88, (0x01 << 3) | 0x01, 0x00,
                               (0x05 << 3) | 0x07, 0x77};

  const auto pages =
      CDVDDemuxTeletext::ParseDescriptor(extradata, sizeof(extradata), "eng,deu,swe");

  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0].page, 0x888);
  EXPECT_FALSE(pages[0].hearingImpaired);
  EXPECT_EQ(pages[0].language.AsIso6392B(), "eng");
  EXPECT_EQ(pages[1].page, 0x777);
  EXPECT_TRUE(pages[1].hearingImpaired);
  EXPECT_EQ(pages[1].language.AsIso6392B(), "swe");
}

TEST(TestDVDDemuxTeletext, DiscoversPageAndEmitsOnNextHeader)
{
  CDVDDemuxTeletext demux({});
  EXPECT_EQ(demux.GetNrOfStreams(), 0);

  DemuxPacket* in =
      Packet({Header(8, 0x88, true, true), TextRow(8, 20, "\x0b\x0bHello\x0a")}, 1000.0);
  EXPECT_EQ(demux.Read(in), nullptr);
  CDVDDemuxUtils::FreeDemuxPacket(in);

  ASSERT_EQ(demux.GetNrOfStreams(), 1);
  EXPECT_EQ(demux.GetStream(0x888)->type, StreamType::SUBTITLE);

  // The page is complete when the next header of the magazine arrives
  in = Packet({Header(8, 0x88, true, true)}, 2000.0);
  DemuxPacket* out = demux.Read(in);
  CDVDDemuxUtils::FreeDemuxPacket(in);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->iStreamId, 0x888);
  EXPECT_EQ(out->pts, 1000.0);
  EXPECT_EQ(Text(out), "Hello");
  CDVDDemuxUtils::FreeDemuxPacket(out);
  EXPECT_EQ(demux.Read(), nullptr);

  // An erased page without rows clears the subtitle once
  in = Packet({Header(8, 0x88, true, true)}, 3000.0);
  out = demux.Read(in);
  CDVDDemuxUtils::FreeDemuxPacket(in);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->iSize, 0);
  EXPECT_EQ(out->pts, 2000.0);
  CDVDDemuxUtils::FreeDemuxPacket(out);

  in = Packet({Header(8, 0x88, true, true)}, 4000.0);
  EXPECT_EQ(demux.Read(in), nullptr);
  CDVDDemuxUtils::FreeDemuxPacket(in);
}

TEST(TestDVDDemuxTeletext, DescriptorPagesWithoutSubtitleFlag)
{
  TeletextSubtitlePage page;
  page.page = 0x150;
  CDVDDemuxTeletext demux({page});
  ASSERT_EQ(demux.GetNrOfStreams(), 1);

  DemuxPacket* in = Packet({Header(1, 0x50, false, true), TextRow(1, 22, "\x0b\x0bText\x0a"),
                            Header(1, 0xFF, false, false)},
                           500.0);
  DemuxPacket* out = demux.Read(in);
  CDVDDemuxUtils::FreeDemuxPacket(in);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->iStreamId, 0x150);
  EXPECT_EQ(Text(out), "Text");
  CDVDDemuxUtils::FreeDemuxPacket(out);
}
