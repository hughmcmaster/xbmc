/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDDemuxTeletext.h"

#include "DVDDemuxUtils.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "video/TeletextCharset.h"
#include "video/TeletextDefines.h"

#include <algorithm>
#include <cctype>
#include <cstring>

using namespace KODI::LANGUAGE;

namespace
{
// EN 300 472: a teletext data unit is 46 bytes (data_unit_id, length, field/line, framing code, 42 data bytes)
constexpr int DATA_UNIT_SIZE = 46;
constexpr int DATA_UNIT_ID_NON_SUBTITLE = 0x02;
constexpr int DATA_UNIT_ID_SUBTITLE = 0x03;
constexpr int DATA_UNIT_LENGTH = 0x2C;

// EN 300 468 6.2.43 teletext_descriptor teletext_type values
constexpr uint8_t TELETEXT_TYPE_SUBTITLE = 0x02;
constexpr uint8_t TELETEXT_TYPE_SUBTITLE_HEARING_IMPAIRED = 0x05;
constexpr size_t TELETEXT_DESCRIPTOR_ENTRY_SIZE = 5;

// ETS 300 706 12.2 spacing attributes
constexpr uint8_t ATTR_ALPHA_COLOUR_MAX = 0x07;
constexpr uint8_t ATTR_END_BOX = 0x0A;
constexpr uint8_t ATTR_START_BOX = 0x0B;
constexpr uint8_t ATTR_MOSAIC_COLOUR_MIN = 0x10;
constexpr uint8_t ATTR_MOSAIC_COLOUR_MAX = 0x17;
constexpr uint8_t ATTR_CONCEAL = 0x18;
constexpr uint8_t ATTR_ESC = 0x1B;
constexpr uint8_t ATTR_MAX = 0x1F;

constexpr int COLOUR_WHITE = 7;

constexpr const char* COLOUR_TAGS[8] = {
    // black is rendered grey so it stays visible on a dark background
    "<font color=\"#888888\">", "<font color=\"#ff0000\">", "<font color=\"#00ff00\">",
    "<font color=\"#ffff00\">", "<font color=\"#0000ff\">", "<font color=\"#ff00ff\">",
    "<font color=\"#00ffff\">", "<font color=\"#ffffff\">",
};
constexpr const char* COLOUR_TAG_CLOSE = "</font>";

uint8_t BitReverse(uint8_t b)
{
  b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
  b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
  b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
  return b;
}

int PageFromDescriptor(uint8_t typeMagazine, uint8_t pageBcd)
{
  int magazine = typeMagazine & 0x07;
  if (magazine == 0)
    magazine = 8;
  return magazine << 8 | pageBcd;
}

CLanguageTag LanguageFromDescriptor(const uint8_t* entry, const CLanguageTag& fallback)
{
  std::string code(reinterpret_cast<const char*>(entry), 3);
  if (std::ranges::all_of(code, [](char c) { return std::isalpha(static_cast<unsigned char>(c)); }))
    return CLanguageTag::Parse(code);
  return fallback;
}
} // namespace

CDVDDemuxTeletext::CDVDDemuxTeletext(const uint8_t* descriptor,
                                     size_t size,
                                     const CLanguageTag& language)
  : m_language(language.IsEmpty() ? CLanguageTag::Undetermined() : language)
{
  ResetMagazines();

  // anything that is not a sequence of 5 byte entries (e.g. the raw 2 byte entries FFmpeg
  // produces) cannot be interpreted; subtitle pages will then be discovered at runtime
  if (!descriptor || size % TELETEXT_DESCRIPTOR_ENTRY_SIZE != 0)
    return;

  for (size_t off = 0; off + TELETEXT_DESCRIPTOR_ENTRY_SIZE <= size;
       off += TELETEXT_DESCRIPTOR_ENTRY_SIZE)
  {
    const uint8_t* entry = descriptor + off;
    const uint8_t type = entry[3] >> 3;
    if (type != TELETEXT_TYPE_SUBTITLE && type != TELETEXT_TYPE_SUBTITLE_HEARING_IMPAIRED)
      continue;

    const int page = PageFromDescriptor(entry[3], entry[4]);
    if (!IsDec(page) || FindPage(page))
      continue;

    const StreamFlags flags = type == TELETEXT_TYPE_SUBTITLE_HEARING_IMPAIRED
                                  ? StreamFlags::FLAG_HEARING_IMPAIRED
                                  : StreamFlags::FLAG_NONE;
    AddPage(page, LanguageFromDescriptor(entry, m_language), flags);
  }
}

CDVDDemuxTeletext::~CDVDDemuxTeletext()
{
  Flush();
}

bool CDVDDemuxTeletext::Reset()
{
  Flush();
  return true;
}

void CDVDDemuxTeletext::Flush()
{
  for (DemuxPacket* packet : m_pending)
    CDVDDemuxUtils::FreeDemuxPacket(packet);
  m_pending.clear();

  ResetMagazines();

  // the next text shown after a flush must not be suppressed as a duplicate
  for (auto& page : m_pages)
  {
    page->lastText.clear();
    page->blank = true;
  }
}

void CDVDDemuxTeletext::ResetMagazines()
{
  for (Magazine& mag : m_magazines)
  {
    mag = Magazine();
    mag.data.fill(' ');
  }
  m_magCharsetValid.fill(false);
  m_magDefaultCharset.fill(0);
  m_magSecondCharset.fill(0);
}

DemuxPacket* CDVDDemuxTeletext::Read()
{
  return Read(nullptr);
}

CDemuxStream* CDVDDemuxTeletext::GetStream(int iStreamId) const
{
  Page* page = FindPage(iStreamId);
  return page ? &page->stream : nullptr;
}

std::vector<CDemuxStream*> CDVDDemuxTeletext::GetStreams() const
{
  std::vector<CDemuxStream*> streams;
  streams.reserve(m_pages.size());
  for (const auto& page : m_pages)
    streams.push_back(&page->stream);
  return streams;
}

int CDVDDemuxTeletext::GetNrOfStreams() const
{
  return static_cast<int>(m_pages.size());
}

CDVDDemuxTeletext::Page* CDVDDemuxTeletext::FindPage(int page) const
{
  const auto it = std::ranges::find_if(m_pages, [page](const auto& p) { return p->page == page; });
  return it != m_pages.end() ? it->get() : nullptr;
}

CDVDDemuxTeletext::Page& CDVDDemuxTeletext::AddPage(int page,
                                                    const CLanguageTag& language,
                                                    StreamFlags flags)
{
  auto entry = std::make_unique<Page>();
  entry->page = page;
  entry->stream.uniqueId = page;
  entry->stream.source = STREAM_SOURCE_TELETEXTMUX;
  entry->stream.codec = AV_CODEC_ID_TEXT;
  entry->stream.language = language;
  entry->stream.flags = flags;
  entry->stream.name = StringUtils::Format(
      "{} {:03X}", CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(10550),
      page); // "Teletext 888"

  CLog::Log(LOGINFO, "CDVDDemuxTeletext: found teletext subtitle page {:03X} ({})", page,
            language.AsBcp47());

  m_pages.push_back(std::move(entry));
  return *m_pages.back();
}

DemuxPacket* CDVDDemuxTeletext::Read(DemuxPacket* packet)
{
  if (packet && packet->pData && packet->iSize > 1)
  {
    const uint8_t* data = packet->pData;
    const int size = packet->iSize;

    // ITU-R System B teletext in a PES packet according to EN 300 472: data_identifier 0x10..0x1F
    if (data[0] >= 0x10 && data[0] <= 0x1F)
    {
      double pts = packet->pts;
      if (pts == DVD_NOPTS_VALUE)
        pts = packet->dts;
      const bool ptsValid = pts != DVD_NOPTS_VALUE;

      uint8_t row[42];
      for (int off = 1; off + DATA_UNIT_SIZE <= size; off += DATA_UNIT_SIZE)
      {
        const uint8_t* unit = data + off;
        if ((unit[0] != DATA_UNIT_ID_NON_SUBTITLE && unit[0] != DATA_UNIT_ID_SUBTITLE) ||
            unit[1] != DATA_UNIT_LENGTH)
          continue;

        // skip field/line and framing code, bytes are transmitted LSB first
        for (int i = 0; i < 42; i++)
          row[i] = BitReverse(unit[4 + i]);

        DecodeLine(row, pts, ptsValid);
      }
    }
  }

  if (m_pending.empty())
    return nullptr;

  DemuxPacket* result = m_pending.front();
  m_pending.pop_front();
  return result;
}

void CDVDDemuxTeletext::DecodeLine(const uint8_t* row, double pts, bool ptsValid)
{
  const int b1 = dehamming[row[0]];
  const int b2 = dehamming[row[1]];
  if (b1 == 0xFF || b2 == 0xFF)
    return;

  const int magIdx = b1 & 0x07;
  const int packetNumber = (b1 >> 3) | (b2 << 1);
  Magazine& mag = m_magazines[magIdx];

  if (packetNumber == 0)
  {
    // a new header terminates the page previously received in this magazine
    DeliverPage(mag);

    const int units = dehamming[row[2]];
    const int tens = dehamming[row[3]];
    if (units == 0xFF || tens == 0xFF || (units == 0x0F && tens == 0x0F))
    {
      mag.page = -1;
      return;
    }

    const int magazine = magIdx == 0 ? 8 : magIdx;
    const int page = magazine << 8 | tens << 4 | units;

    const int c4 = dehamming[row[5]];
    const int c6 = dehamming[row[7]];
    const int national = dehamming[row[9]];
    if (c4 == 0xFF || c6 == 0xFF || national == 0xFF)
    {
      mag.page = -1;
      return;
    }

    if (page != mag.page)
    {
      mag.data.fill(' ');
      mag.charsetValid = false;
    }
    else if (c4 & 0x08) // C4: erase page
    {
      mag.data.fill(' ');
    }

    mag.page = page;
    mag.subtitle = (c6 & 0x08) != 0; // C6: subtitle
    // C12..C14 national option bits are transmitted in reverse order (ETS 300 706 table 32)
    mag.national = ((national >> 3) & 0x01) | ((national >> 1) & 0x02) | ((national << 1) & 0x04);
    mag.pts = pts;
    mag.ptsValid = ptsValid;

    if (mag.subtitle && IsDec(page) && !FindPage(page))
      AddPage(page, m_language, StreamFlags::FLAG_NONE);
  }
  else if (mag.page < 0)
  {
    return;
  }
  else if (packetNumber <= ROWS)
  {
    uint8_t* dst = mag.data.data() + (packetNumber - 1) * COLUMNS;
    for (int i = 0; i < COLUMNS; i++)
      dst[i] = row[2 + i] & 0x7F; // tolerate parity errors like the page renderer does
  }
  else if (packetNumber == 28 || packetNumber == 29)
  {
    if (dehamming[row[2]] != 0) // designation code must be 0 (basic level 1 page / magazine)
      return;

    const int t1 = CDVDTeletextTools::deh24(const_cast<uint8_t*>(row + 3));
    const int t2 = CDVDTeletextTools::deh24(const_cast<uint8_t*>(row + 6));
    if (t1 < 0 || t2 < 0)
      return;

    const uint8_t defaultCharset = (t1 >> 7) & 0x7F;
    const uint8_t secondCharset = ((t1 >> 14) & 0x0F) | ((t2 << 4) & 0x70);
    if (packetNumber == 28)
    {
      mag.charsetValid = true;
      mag.defaultCharset = defaultCharset;
      mag.secondCharset = secondCharset;
    }
    else
    {
      m_magCharsetValid[magIdx] = true;
      m_magDefaultCharset[magIdx] = defaultCharset;
      m_magSecondCharset[magIdx] = secondCharset;
    }
  }
}

void CDVDDemuxTeletext::DeliverPage(Magazine& mag)
{
  if (mag.page < 0 || !mag.ptsValid)
    return;

  Page* page = FindPage(mag.page);
  if (!page)
    return;

  const int magIdx = mag.page >> 8 & 0x07;
  int national;
  int secondNational;
  if (mag.charsetValid || m_magCharsetValid[magIdx])
  {
    // X/28 or M/29 select the G0 set, the header bits select the national option within it
    const uint8_t defaultCharset =
        mag.charsetValid ? mag.defaultCharset : m_magDefaultCharset[magIdx];
    const uint8_t secondCharset = mag.charsetValid ? mag.secondCharset : m_magSecondCharset[magIdx];
    national = CTeletextCharset::NationalFromDesignation((defaultCharset & ~0x07) | mag.national);
    secondNational = CTeletextCharset::NationalFromDesignation(secondCharset);
  }
  else
  {
    national = CountryConversionTable[mag.national];
    secondNational = national;
  }

  const std::string text = PageToText(mag.data.data(), national, secondNational);

  // avoid repeating blank subtitles, and identical text
  if (text.empty() && page->blank)
    return;
  if (!text.empty() && text == page->lastText)
    return;

  page->blank = text.empty();
  page->lastText = text;

  DemuxPacket* packet = CDVDDemuxUtils::AllocateDemuxPacket(static_cast<int>(text.size()));
  if (!packet)
    return;

  packet->iSize = static_cast<int>(text.size());
  if (packet->iSize)
    std::memcpy(packet->pData, text.data(), text.size());
  packet->iStreamId = page->page;
  packet->pts = mag.pts;
  packet->dts = mag.pts;
  packet->duration = 0;
  m_pending.push_back(packet);
}

std::string CDVDDemuxTeletext::PageToText(const uint8_t* page,
                                          int nationalSubset,
                                          int secondNationalSubset,
                                          bool withColours)
{
  std::string text;

  for (int row = 0; row < ROWS; row++)
  {
    const uint8_t* line = page + row * COLUMNS;
    std::string rowText;
    bool box = false;
    bool mosaic = false;
    bool conceal = false;
    bool pendingSpace = false;
    int national = nationalSubset;
    int colour = COLOUR_WHITE; // colour requested by the last colour attribute
    int activeColour = COLOUR_WHITE; // colour of the currently open font tag

    for (int col = 0; col < COLUMNS; col++)
    {
      const uint8_t ch = line[col];

      if (ch <= ATTR_MAX)
      {
        if (ch <= ATTR_ALPHA_COLOUR_MAX ||
            (ch >= ATTR_MOSAIC_COLOUR_MIN && ch <= ATTR_MOSAIC_COLOUR_MAX))
        {
          mosaic = ch >= ATTR_MOSAIC_COLOUR_MIN;
          conceal = false;
          colour = ch & 0x07;
        }
        else if (ch == ATTR_START_BOX)
          box = true;
        else if (ch == ATTR_END_BOX)
          box = false;
        else if (ch == ATTR_CONCEAL)
          conceal = true;
        else if (ch == ATTR_ESC)
          national = national == nationalSubset ? secondNationalSubset : nationalSubset;

        // a spacing attribute occupies a character position and is displayed as a space
        pendingSpace = pendingSpace || !rowText.empty();
        continue;
      }

      if (!box || conceal)
        continue;

      // mosaic graphics (bit 6 clear) have no textual representation
      if (mosaic && !(ch & 0x40))
        continue;

      if (ch == ' ')
      {
        pendingSpace = pendingSpace || !rowText.empty();
        continue;
      }

      const uint16_t ucs2 = CTeletextCharset::G0ToUcs2(national, ch);
      if (ucs2 == 0)
        continue;

      if (withColours && colour != activeColour)
      {
        if (activeColour != COLOUR_WHITE)
          rowText += COLOUR_TAG_CLOSE;
        if (pendingSpace)
        {
          rowText.push_back(' ');
          pendingSpace = false;
        }
        if (colour != COLOUR_WHITE)
          rowText += COLOUR_TAGS[colour];
        activeColour = colour;
      }

      if (pendingSpace)
      {
        rowText.push_back(' ');
        pendingSpace = false;
      }

      CTeletextCharset::AppendUtf8(rowText, ucs2);
    }

    if (rowText.empty())
      continue;

    if (activeColour != COLOUR_WHITE)
      rowText += COLOUR_TAG_CLOSE;

    if (!text.empty())
      text.push_back('\n');
    text += rowText;
  }

  return text;
}
