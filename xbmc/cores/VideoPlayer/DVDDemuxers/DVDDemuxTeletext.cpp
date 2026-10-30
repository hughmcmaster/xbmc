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
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "language/LanguageTag.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "utils/CharsetConverter.h"
#include "utils/StringUtils.h"
#include "video/TeletextDefines.h"

#include <algorithm>
#include <utility>

using namespace KODI::LANGUAGE;

namespace
{
constexpr int TELETEXT_TYPE_SUBTITLE = 0x02;
constexpr int TELETEXT_TYPE_SUBTITLE_HEARING_IMPAIRED = 0x05;

// EN 300 706 default CLUT 0, matching the colours used by the teletext window
constexpr std::array<const char*, 8> COLOURS = {"000000", "FC1414", "24FC24", "FCC024",
                                                "0000FC", "B000FC", "00FCFC", "FCFCFC"};
constexpr int COLOUR_WHITE = 7;

int NationalSubset(int code)
{
  switch (code)
  {
    case 0x08:
      return NAT_PL;
    case 0x16:
    case 0x36:
      return NAT_TR;
    case 0x1d:
      return NAT_SR;
    case 0x20:
      return NAT_SC;
    case 0x24:
      return NAT_RB;
    case 0x25:
      return NAT_UA;
    case 0x22:
      return NAT_ET;
    case 0x23:
      return NAT_LV;
    case 0x37:
      return NAT_GR;
    case 0x55:
      return NAT_HB;
    case 0x47:
    case 0x57:
      return NAT_AR;
    default:
      return CountryConversionTable[code & 0x07];
  }
}

char32_t MapChar(uint8_t ch, int subset)
{
  if (subset > NAT_MAX_FROM_HEADER)
    return G0table[subset - NAT_SC][ch - 0x20];

  switch (ch)
  {
    case 0x23:
    case 0x24:
      return nationaltable23[subset][ch - 0x23];
    case 0x40:
      return nationaltable40[subset];
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F:
    case 0x60:
      return nationaltable5b[subset][ch - 0x5B];
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
      return nationaltable7b[subset][ch - 0x7B];
    case 0x7F:
      return U'\u25A0';
    default:
      return ch;
  }
}

int DecodeRow(const uint8_t* vtxRow, std::array<uint8_t, 42>& row)
{
  for (int i = 0; i < 42; i++)
  {
    uint8_t byte = vtxRow[i];
    row[i] = rev_lut[(byte >> 4) & 0xf] | rev_lut[(byte & 0xf) + 16];
  }
  uint8_t b1 = dehamming[row[0]];
  uint8_t b2 = dehamming[row[1]];
  if (b1 == 0xFF || b2 == 0xFF)
    return -1;
  return ((b1 & 8) >> 3) | (b2 << 1);
}
} // unnamed namespace

CDVDDemuxTeletext::CDVDDemuxTeletext(const std::vector<TeletextSubtitlePage>& pages)
{
  for (const TeletextSubtitlePage& page : pages)
  {
    if (!FindStream(page.page))
      AddStream(page);
  }
}

CDVDDemuxTeletext::~CDVDDemuxTeletext()
{
  Flush();
}

void CDVDDemuxTeletext::Flush()
{
  for (DemuxPacket* packet : m_packets)
    CDVDDemuxUtils::FreeDemuxPacket(packet);
  m_packets.clear();

  for (Magazine& mag : m_magazines)
    mag = Magazine{};

  for (Stream& stream : m_streams)
    stream.lastText.clear();
}

DemuxPacket* CDVDDemuxTeletext::Read()
{
  if (m_packets.empty())
    return nullptr;

  DemuxPacket* packet = m_packets.front();
  m_packets.pop_front();
  return packet;
}

CDemuxStream* CDVDDemuxTeletext::GetStream(int iStreamId) const
{
  for (const Stream& stream : m_streams)
  {
    if (stream.stream->uniqueId == iStreamId)
      return stream.stream.get();
  }
  return nullptr;
}

std::vector<CDemuxStream*> CDVDDemuxTeletext::GetStreams() const
{
  std::vector<CDemuxStream*> streams;
  streams.reserve(m_streams.size());
  for (const Stream& stream : m_streams)
    streams.push_back(stream.stream.get());
  return streams;
}

int CDVDDemuxTeletext::GetNrOfStreams() const
{
  return static_cast<int>(m_streams.size());
}

CDVDDemuxTeletext::Stream* CDVDDemuxTeletext::FindStream(int page)
{
  for (Stream& stream : m_streams)
  {
    if (stream.stream->uniqueId == page)
      return &stream;
  }
  return nullptr;
}

CDVDDemuxTeletext::Stream& CDVDDemuxTeletext::AddStream(const TeletextSubtitlePage& page)
{
  auto stream = std::make_unique<CDemuxStreamSubtitle>();
  stream->source = STREAM_SOURCE_TELETEXT;
  stream->codec = AV_CODEC_ID_TEXT;
  stream->uniqueId = page.page;
  stream->name = StringUtils::Format(
      "{} {:03X}", CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(10550),
      page.page);
  stream->language = page.language.IsEmpty() ? CLanguageTag::Undetermined() : page.language;
  if (page.hearingImpaired)
    stream->flags = FLAG_HEARING_IMPAIRED;

  m_streams.push_back({std::move(stream), {}});
  return m_streams.back();
}

std::vector<TeletextSubtitlePage> CDVDDemuxTeletext::ParseDescriptor(const uint8_t* data,
                                                                     size_t size,
                                                                     std::string_view languages)
{
  std::vector<TeletextSubtitlePage> pages;
  std::vector<std::string> langs = StringUtils::Split(languages, ',');

  for (size_t i = 0; i + 1 < size; i += 2)
  {
    const int type = data[i] >> 3;
    if (type != TELETEXT_TYPE_SUBTITLE && type != TELETEXT_TYPE_SUBTITLE_HEARING_IMPAIRED)
      continue;

    int magazine = data[i] & 0x07;
    if (magazine == 0)
      magazine = 8;

    TeletextSubtitlePage page;
    page.page = (magazine << 8) | data[i + 1];
    page.hearingImpaired = type == TELETEXT_TYPE_SUBTITLE_HEARING_IMPAIRED;

    const size_t langIdx = i / 2;
    if (langIdx < langs.size() && !langs[langIdx].empty())
      page.language = CLanguageTag::Parse(langs[langIdx]);

    pages.push_back(std::move(page));
  }
  return pages;
}

DemuxPacket* CDVDDemuxTeletext::Read(DemuxPacket* packet)
{
  if (!packet)
    return Read();

  const uint8_t* data = packet->pData;
  const int size = packet->iSize;

  // EN 300 472: data_identifier followed by 46 byte data units
  if (size < 1 || data[0] < 0x10 || data[0] > 0x1F)
    return Read();

  const int units = (size - 1) / 46;
  for (int unit = 0; unit < units; unit++)
  {
    const uint8_t* du = &data[1 + unit * 46];
    // data_unit_id: EBU teletext non-subtitle (0x02) or subtitle (0x03)
    if ((du[0] == 0x02 || du[0] == 0x03) && du[1] == 0x2C)
      ProcessRow(&du[4], packet->pts);
  }

  return Read();
}

void CDVDDemuxTeletext::ProcessRow(const uint8_t* vtxRow, double pts)
{
  std::array<uint8_t, 42> row;
  const int packetNumber = DecodeRow(vtxRow, row);
  if (packetNumber < 0)
    return;

  int magazineNumber = dehamming[row[0]] & 7;
  if (magazineNumber == 0)
    magazineNumber = 8;
  Magazine& mag = m_magazines[magazineNumber];

  if (packetNumber == 0)
  {
    const uint8_t units = dehamming[row[2]];
    const uint8_t tens = dehamming[row[3]];
    const uint8_t c4 = dehamming[row[5]];
    const uint8_t c5c8 = dehamming[row[7]];
    const uint8_t c9c14 = dehamming[row[9]];
    if (units == 0xFF || tens == 0xFF || c4 == 0xFF || c5c8 == 0xFF || c9c14 == 0xFF)
      return;

    // A page header terminates the page in progress on this magazine, or on all
    // magazines in serial transmission mode
    if (m_serial)
    {
      for (Magazine& other : m_magazines)
        FinishPage(other);
    }
    else
    {
      FinishPage(mag);
    }

    m_serial = (c9c14 & 0x01) != 0;

    const int page = (magazineNumber << 8) | (tens << 4) | units;
    // Page 0xFF of a magazine is a filler used to time out the previous page
    if ((page & 0xFF) == 0xFF)
    {
      mag.page = -1;
      return;
    }

    const bool erase = (c4 & 0x08) != 0;
    if (mag.page != page || erase)
    {
      mag.rows = {};
      mag.rowValid = {};
      mag.hasX28 = false;
      mag.dirty = true;
    }

    mag.page = page;
    mag.pts = pts;
    mag.subtitle = (c5c8 & 0x08) != 0;
    mag.headerNational = rev_lut[c9c14] & 0x07;

    if (mag.subtitle && !FindStream(page))
    {
      TeletextSubtitlePage subPage;
      subPage.page = page;
      AddStream(subPage);
    }
    return;
  }

  if (mag.page < 0)
    return;

  if (packetNumber <= 23)
  {
    const int rowIdx = packetNumber - 1;
    for (int i = 0; i < 40; i++)
      mag.rows[rowIdx][i] = row[2 + i] & 0x7F;
    mag.rowValid[rowIdx] = true;
    mag.dirty = true;
  }
  else if (packetNumber == 28 || packetNumber == 29)
  {
    const uint8_t designation = dehamming[row[2]];
    if (designation != 0)
      return;

    const int t1 = CDVDTeletextTools::deh24(&row[3]);
    const int t2 = CDVDTeletextTools::deh24(&row[6]);
    if (t1 < 0 || t2 < 0)
      return;

    const int defaultCharset = (t1 >> 7) & 0x7F;
    const int secondCharset = ((t1 >> 14) & 0x0F) | ((t2 << 4) & 0x70);
    if (packetNumber == 28)
    {
      mag.hasX28 = true;
      mag.x28Default = defaultCharset;
      mag.x28Second = secondCharset;
    }
    else
    {
      mag.hasM29 = true;
      mag.m29Default = defaultCharset;
      mag.m29Second = secondCharset;
    }
    mag.dirty = true;
  }
}

void CDVDDemuxTeletext::FinishPage(Magazine& mag)
{
  if (mag.page < 0 || !mag.dirty)
    return;
  mag.dirty = false;

  Stream* stream = FindStream(mag.page);
  if (!stream)
    return;

  int primary;
  int secondary;
  if (mag.hasX28)
  {
    primary = NationalSubset(mag.x28Default);
    secondary = NationalSubset(mag.x28Second);
  }
  else if (mag.hasM29)
  {
    primary = NationalSubset(mag.m29Default);
    secondary = NationalSubset(mag.m29Second);
  }
  else
  {
    primary = CountryConversionTable[mag.headerNational];
    secondary = primary;
  }

  const std::string text = PageToText(mag.rows, mag.rowValid, primary, secondary);

  // A repeated text extends the subtitle, a cleared page ends it once
  if (text.empty() && stream->lastText.empty())
    return;

  EmitPacket(*stream, text, mag.pts);
  stream->lastText = text;
}

void CDVDDemuxTeletext::EmitPacket(Stream& stream, const std::string& text, double pts)
{
  DemuxPacket* packet = CDVDDemuxUtils::AllocateDemuxPacket(static_cast<int>(text.size()));
  if (!packet)
    return;

  packet->iSize = static_cast<int>(text.size());
  if (packet->iSize)
    std::copy(text.begin(), text.end(), packet->pData);
  packet->iStreamId = stream.stream->uniqueId;
  packet->pts = pts;
  packet->dts = pts;
  packet->duration = 0;
  m_packets.push_back(packet);
}

std::string CDVDDemuxTeletext::PageToText(const std::array<std::array<uint8_t, 40>, 24>& rows,
                                          const std::array<bool, 24>& rowValid,
                                          int primarySubset,
                                          int secondarySubset)
{
  std::string text;
  bool skipRow = false;

  for (int rowIdx = 0; rowIdx < 24; rowIdx++)
  {
    if (skipRow)
    {
      skipRow = false;
      continue;
    }
    if (!rowValid[rowIdx])
      continue;

    const std::array<uint8_t, 40>& row = rows[rowIdx];
    std::u32string line;
    int colour = COLOUR_WHITE;
    int lineColour = COLOUR_WHITE; // colour of the currently open font tag
    bool boxed = false;
    bool mosaic = false;
    bool concealed = false;
    bool doubleHeight = false;
    int subset = primarySubset;
    bool pendingSpace = false;

    for (uint8_t ch : row)
    {
      if (ch < 0x20)
      {
        switch (ch)
        {
          case 0x00:
          case 0x01:
          case 0x02:
          case 0x03:
          case 0x04:
          case 0x05:
          case 0x06:
          case 0x07:
            colour = ch;
            mosaic = false;
            concealed = false;
            break;
          case 0x0A:
            boxed = false;
            break;
          case 0x0B:
            boxed = true;
            break;
          case 0x0D:
          case 0x0F:
            doubleHeight = true;
            break;
          case 0x10:
          case 0x11:
          case 0x12:
          case 0x13:
          case 0x14:
          case 0x15:
          case 0x16:
          case 0x17:
            colour = ch - 0x10;
            mosaic = true;
            concealed = false;
            break;
          case 0x18:
            concealed = true;
            break;
          case 0x1B:
            subset = (subset == primarySubset) ? secondarySubset : primarySubset;
            break;
          default:
            break;
        }
        if (boxed && !line.empty())
          pendingSpace = true;
        continue;
      }

      if (!boxed || concealed)
        continue;

      // Block mosaics share the code range of G0 characters
      const bool isMosaic = mosaic && (ch & 0xA0) == 0x20;
      if (ch == 0x20 || isMosaic)
      {
        if (!line.empty())
          pendingSpace = true;
        continue;
      }

      if (colour != lineColour && lineColour != COLOUR_WHITE)
        line.append(U"</font>");

      if (pendingSpace)
      {
        line.push_back(U' ');
        pendingSpace = false;
      }

      if (colour != lineColour)
      {
        if (colour != COLOUR_WHITE)
        {
          line.append(U"<font color=#");
          for (const char* c = COLOURS[colour]; *c; c++)
            line.push_back(static_cast<char32_t>(*c));
          line.push_back(U'>');
        }
        lineColour = colour;
      }

      line.push_back(MapChar(ch, subset));
    }

    if (lineColour != COLOUR_WHITE)
      line.append(U"</font>");

    skipRow = doubleHeight;

    if (line.empty())
      continue;

    if (!text.empty())
      text.push_back('\n');
    text.append(CCharsetConverter::utf32ToUtf8(line));
  }

  return text;
}
