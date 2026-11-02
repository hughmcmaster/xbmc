/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "cores/VideoPlayer/DVDDemuxers/DVDDemuxTeletext.h"
#include "cores/VideoPlayer/DVDDemuxers/DVDDemuxUtils.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "language/LanguageTag.h"
#include "video/TeletextDefines.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::LANGUAGE;

namespace
{
constexpr int ROWS = 23;
constexpr int COLUMNS = 40;
constexpr int ROW_BYTES = 42;
constexpr int DATA_UNIT_SIZE = 46;

uint8_t BitReverse(uint8_t b)
{
  b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
  b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
  b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
  return b;
}

// Hamming 8/4 encode a nibble by looking up the decoding table used by the decoder
uint8_t Hamming84(int value)
{
  for (int i = 0; i < 256; i++)
  {
    if (dehamming[i] == value)
      return static_cast<uint8_t>(i);
  }
  ADD_FAILURE() << "no hamming code for " << value;
  return 0;
}

uint8_t OddParity(uint8_t value)
{
  uint8_t parity = 0;
  for (int i = 0; i < 7; i++)
    parity ^= (value >> i) & 1;
  return static_cast<uint8_t>((value & 0x7F) | (parity ? 0 : 0x80));
}

class TeletextPes
{
public:
  // Header packet X/0 for page (magazine << 8 | bcd page), with control bits
  TeletextPes& Header(int page, bool subtitle, bool erase = false, int national = 0)
  {
    std::array<uint8_t, ROW_BYTES> row{};
    const int magazine = (page >> 8) & 0x07; // 8 is transmitted as 0
    row[0] = Hamming84(magazine);
    row[1] = Hamming84(0);
    row[2] = Hamming84(page & 0x0F);
    row[3] = Hamming84((page >> 4) & 0x0F);
    row[4] = Hamming84(0); // subcode S1
    row[5] = Hamming84(erase ? 0x08 : 0); // S2, C4
    row[6] = Hamming84(0); // S3
    row[7] = Hamming84(subtitle ? 0x08 : 0); // S4, C5, C6
    row[8] = Hamming84(0); // C7..C10
    // C12..C14 are transmitted reversed
    const int nat = ((national & 0x01) << 3) | ((national & 0x02) << 1) | ((national & 0x04) >> 1);
    row[9] = Hamming84(nat);
    for (int i = 10; i < ROW_BYTES; i++)
      row[i] = OddParity(' ');
    return Add(row);
  }

  TeletextPes& Row(int magazine, int packetNumber, const std::string& text)
  {
    std::array<uint8_t, ROW_BYTES> row{};
    row[0] = Hamming84((magazine & 0x07) | ((packetNumber & 0x01) << 3));
    row[1] = Hamming84(packetNumber >> 1);
    for (int i = 0; i < COLUMNS; i++)
      row[2 + i] = OddParity(i < static_cast<int>(text.size()) ? text[i] : ' ');
    return Add(row);
  }

  std::vector<uint8_t>& Data() { return m_data; }

private:
  TeletextPes& Add(const std::array<uint8_t, ROW_BYTES>& row)
  {
    if (m_data.empty())
      m_data.push_back(0x10); // data_identifier
    m_data.push_back(0x03); // data_unit_id subtitle
    m_data.push_back(0x2C); // data_unit_length
    m_data.push_back(0xC0); // field parity / line offset
    m_data.push_back(0xE4); // framing code
    for (uint8_t b : row)
      m_data.push_back(BitReverse(b));
    return *this;
  }

  std::vector<uint8_t> m_data;
};

struct Subtitle
{
  std::string text;
  double pts;
};

std::vector<Subtitle> Feed(CDVDDemuxTeletext& demuxer, TeletextPes& pes, double pts)
{
  DemuxPacket* packet = CDVDDemuxUtils::AllocateDemuxPacket(pes.Data().size());
  std::memcpy(packet->pData, pes.Data().data(), pes.Data().size());
  packet->iSize = static_cast<int>(pes.Data().size());
  packet->pts = pts;
  packet->dts = pts;

  std::vector<Subtitle> texts;
  DemuxPacket* out = demuxer.Read(packet);
  while (out)
  {
    texts.push_back({std::string(reinterpret_cast<const char*>(out->pData), out->iSize), out->pts});
    EXPECT_EQ(out->dts, out->pts);
    EXPECT_EQ(out->iStreamId, 0x888);
    CDVDDemuxUtils::FreeDemuxPacket(out);
    out = demuxer.Read(nullptr);
  }
  CDVDDemuxUtils::FreeDemuxPacket(packet);
  return texts;
}

std::array<uint8_t, ROWS * COLUMNS> MakePage(const std::vector<std::pair<int, std::string>>& rows)
{
  std::array<uint8_t, ROWS * COLUMNS> page;
  page.fill(' ');
  for (const auto& [row, text] : rows)
  {
    for (size_t i = 0; i < text.size() && i < COLUMNS; i++)
      page[(row - 1) * COLUMNS + i] = static_cast<uint8_t>(text[i]);
  }
  return page;
}
} // namespace

TEST(TestDVDDemuxTeletext, PageToTextKeepsOnlyBoxedText)
{
  const auto page = MakePage({{1, "P888 broadcaster header"},
                              {20, "\x0B\x0B Hello \x0A\x0A"},
                              {22, "\x0B\x0B World!\x0A unboxed"}});
  EXPECT_EQ(CDVDDemuxTeletext::PageToText(page.data(), NAT_DEFAULT, NAT_DEFAULT), "Hello\nWorld!");
}

TEST(TestDVDDemuxTeletext, PageToTextEmitsColourTags)
{
  const auto page = MakePage({{20, "\x0B\x0B\x03Yellow\x07 white"}});
  EXPECT_EQ(CDVDDemuxTeletext::PageToText(page.data(), NAT_DEFAULT, NAT_DEFAULT),
            "<font color=\"#ffff00\">Yellow</font> white");
  EXPECT_EQ(CDVDDemuxTeletext::PageToText(page.data(), NAT_DEFAULT, NAT_DEFAULT, false),
            "Yellow white");
}

TEST(TestDVDDemuxTeletext, PageToTextSkipsMosaicsAndConcealed)
{
  const auto page = MakePage({{20, "\x0B\x0B\x17!#$\x07text\x18 hidden"}});
  EXPECT_EQ(CDVDDemuxTeletext::PageToText(page.data(), NAT_DEFAULT, NAT_DEFAULT, false), "text");
}

TEST(TestDVDDemuxTeletext, PageToTextUsesNationalSubset)
{
  // 0x5B, 0x5C, 0x5D are [ \ ] in the default set and Ä Ö Ü in the German set
  const auto page = MakePage({{20, "\x0B\x0B[\\]\x0A"}});
  EXPECT_EQ(CDVDDemuxTeletext::PageToText(page.data(), NAT_DEFAULT, NAT_DEFAULT), "[\\]");
  EXPECT_EQ(CDVDDemuxTeletext::PageToText(page.data(), NAT_DE, NAT_DE), "ÄÖÜ");
  // ESC switches to the second G0 set; like every spacing attribute it is displayed as a space
  const auto escPage = MakePage({{20, "\x0B\x0B[\x1B[\x0A"}});
  EXPECT_EQ(CDVDDemuxTeletext::PageToText(escPage.data(), NAT_DEFAULT, NAT_DE), "[ Ä");
}

TEST(TestDVDDemuxTeletext, StreamsFromDescriptor)
{
  // deu 888 subtitles, eng 889 hearing impaired, deu 100 initial page (not a subtitle)
  const uint8_t descriptor[] = {'d', 'e', 'u', 0x02 << 3 | 0, 0x88,
                                'e', 'n', 'g', 0x05 << 3 | 0, 0x89,
                                'd', 'e', 'u', 0x01 << 3 | 1, 0x00};
  CDVDDemuxTeletext demuxer(descriptor, sizeof(descriptor), CLanguageTag::Parse("fra"));

  ASSERT_EQ(demuxer.GetNrOfStreams(), 2);
  CDemuxStream* first = demuxer.GetStream(0x888);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->type, StreamType::SUBTITLE);
  EXPECT_EQ(first->codec, AV_CODEC_ID_TEXT);
  EXPECT_EQ(first->source, STREAM_SOURCE_TELETEXTMUX);
  EXPECT_EQ(first->language.AsBcp47(), "de");
  EXPECT_EQ(first->flags, StreamFlags::FLAG_NONE);

  CDemuxStream* second = demuxer.GetStream(0x889);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->language.AsBcp47(), "en");
  EXPECT_EQ(second->flags, StreamFlags::FLAG_HEARING_IMPAIRED);

  EXPECT_EQ(demuxer.GetStream(0x100), nullptr);
}

TEST(TestDVDDemuxTeletext, IgnoresMalformedDescriptor)
{
  const uint8_t descriptor[] = {0x02 << 3 | 0, 0x88}; // FFmpeg's raw 2 byte form
  CDVDDemuxTeletext demuxer(descriptor, sizeof(descriptor), CLanguageTag::Undetermined());
  EXPECT_EQ(demuxer.GetNrOfStreams(), 0);
}

TEST(TestDVDDemuxTeletext, DiscoversSubtitlePageAndEmitsText)
{
  CDVDDemuxTeletext demuxer(nullptr, 0, CLanguageTag::Parse("eng"));
  EXPECT_EQ(demuxer.GetNrOfStreams(), 0);

  TeletextPes first;
  first.Header(0x888, true, true).Row(8, 20, "\x0B\x0BHello\x0A");
  EXPECT_TRUE(Feed(demuxer, first, 1000.0).empty()); // page is delivered by the next header

  ASSERT_EQ(demuxer.GetNrOfStreams(), 1);
  CDemuxStream* stream = demuxer.GetStream(0x888);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->language.AsBcp47(), "en");

  // the next header of the magazine completes the page, which is timed by its own header
  TeletextPes second;
  second.Header(0x888, true, true).Row(8, 20, "\x0B\x0BHello\x0A");
  std::vector<Subtitle> texts = Feed(demuxer, second, 2000.0);
  ASSERT_EQ(texts.size(), 1U);
  EXPECT_EQ(texts[0].text, "Hello");
  EXPECT_EQ(texts[0].pts, 1000.0);

  // identical content is not repeated
  TeletextPes third;
  third.Header(0x888, true, true).Row(8, 20, "\x0B\x0BWorld\x0A");
  EXPECT_TRUE(Feed(demuxer, third, 3000.0).empty());

  // changed content is delivered, a blank page clears the subtitle
  TeletextPes fourth;
  fourth.Header(0x888, true, true);
  texts = Feed(demuxer, fourth, 4000.0);
  ASSERT_EQ(texts.size(), 1U);
  EXPECT_EQ(texts[0].text, "World");
  EXPECT_EQ(texts[0].pts, 3000.0);

  TeletextPes fifth;
  fifth.Header(0x888, true, true);
  texts = Feed(demuxer, fifth, 5000.0);
  ASSERT_EQ(texts.size(), 1U);
  EXPECT_TRUE(texts[0].text.empty());
  EXPECT_EQ(texts[0].pts, 4000.0);

  // a blank page is not reported twice
  TeletextPes sixth;
  sixth.Header(0x888, true, true);
  EXPECT_TRUE(Feed(demuxer, sixth, 6000.0).empty());
}

TEST(TestDVDDemuxTeletext, NonSubtitlePagesAreIgnored)
{
  CDVDDemuxTeletext demuxer(nullptr, 0, CLanguageTag::Undetermined());

  TeletextPes pes;
  pes.Header(0x100, false, true).Row(1, 5, "\x0B\x0BNews\x0A").Header(0x100, false, true);
  EXPECT_TRUE(Feed(demuxer, pes, 1000.0).empty());
  EXPECT_EQ(demuxer.GetNrOfStreams(), 0);
}

TEST(TestDVDDemuxTeletext, HeaderNationalBitsSelectCharset)
{
  CDVDDemuxTeletext demuxer(nullptr, 0, CLanguageTag::Undetermined());

  // national option 1 (German) in the header, see CountryConversionTable
  TeletextPes pes;
  pes.Header(0x888, true, true, 1).Row(8, 20, "\x0B\x0B[\\]\x0A").Header(0x888, true, true, 1);
  std::vector<Subtitle> texts = Feed(demuxer, pes, 1000.0);
  ASSERT_EQ(texts.size(), 1U);
  EXPECT_EQ(texts[0].text, "ÄÖÜ");
}

TEST(TestDVDDemuxTeletext, FlushDropsPendingState)
{
  CDVDDemuxTeletext demuxer(nullptr, 0, CLanguageTag::Undetermined());

  TeletextPes pes;
  pes.Header(0x888, true, true).Row(8, 20, "\x0B\x0BHello\x0A");
  Feed(demuxer, pes, 1000.0);
  demuxer.Flush();
  EXPECT_EQ(demuxer.GetNrOfStreams(), 1); // streams survive a seek

  // the row received before the flush must not show up after it
  TeletextPes header;
  header.Header(0x888, true, false);
  EXPECT_TRUE(Feed(demuxer, header, 2000.0).empty());
}
