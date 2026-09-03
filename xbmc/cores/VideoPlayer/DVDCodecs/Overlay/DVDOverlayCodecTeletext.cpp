/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDOverlayCodecTeletext.h"

#include "DVDCodecs/DVDCodecs.h"
#include "DVDOverlayTeletext.h"
#include "DVDStreamInfo.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "settings/SettingsComponent.h"
#include "settings/SubtitlesSettings.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <memory>

using namespace KODI;
using namespace SUBTITLES::STYLE;

// Teletext color palette (RGB to BGR for libass)
static const uint32_t TELETEXT_COLORS[8] =
{
  0x000000, // 0: Black       -> &H000000&
  0x0000FF, // 1: Red         -> &HFF0000&
  0x00FF00, // 2: Green       -> &H00FF00&
  0x00FFFF, // 3: Yellow      -> &HFFFF00&
  0xFF0000, // 4: Blue        -> &H0000FF&
  0xFF00FF, // 5: Magenta     -> &HFF00FF&
  0xFFFF00, // 6: Cyan        -> &H00FFFF&
  0xFFFFFF  // 7: White       -> &HFFFFFF&
};

CDVDOverlayCodecTeletext::CDVDOverlayCodecTeletext()
  : CDVDOverlayCodec("DVB Teletext Subtitle Decoder"),
    m_libass(std::make_shared<CDVDSubtitlesLibass>()),
    m_pOverlay(nullptr),
    m_order(0)
{
  m_libass->Configure();
}

CDVDOverlayCodecTeletext::~CDVDOverlayCodecTeletext() = default;

bool CDVDOverlayCodecTeletext::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  if (hints.codec != AV_CODEC_ID_DVB_TELETEXT)
    return false;

  m_pOverlay.reset();
  m_order = 0;

  // Initialize libass track for teletext subtitles
  m_libass->SetSubtitleType(ADAPTED);
  if (!m_libass->CreateTrack() || !m_libass->CreateStyle())
  {
    CLog::Log(LOGERROR, "CDVDOverlayCodecTeletext::Open: Failed to initialize libass");
    return false;
  }

  CLog::Log(LOGINFO, "CDVDOverlayCodecTeletext: Opened teletext subtitle decoder");
  return true;
}

OverlayMessage CDVDOverlayCodecTeletext::Decode(DemuxPacket* pPacket)
{
  if (!pPacket)
    return OverlayMessage::OC_ERROR;

  double pts = pPacket->dts != DVD_NOPTS_VALUE ? pPacket->dts : pPacket->pts;
  // libass only has a precision of msec
  pts = round(pts / 1000) * 1000;

  uint8_t* data = pPacket->pData;
  int size = pPacket->iSize;
  double duration = pPacket->duration;
  if (duration == DVD_NOPTS_VALUE)
    duration = 5000.0; // Default 5 second duration for teletext

  if (!data || size < 2)
    return OverlayMessage::OC_ERROR;

  // Convert teletext packet to ASS format
  std::string assText = ConvertTeletextToASS(data, size);

  if (assText.empty())
    return OverlayMessage::OC_DONE; // Empty subtitle, no error

  // Add to libass track
  int ret = m_libass->AddEvent(assText.c_str(), pts, pts + duration, nullptr);
  if (ret == ASS_NO_ID)
  {
    CLog::Log(LOGDEBUG, "CDVDOverlayCodecTeletext::Decode: Failed to add event to libass");
    return OverlayMessage::OC_DONE;
  }

  return m_pOverlay ? OverlayMessage::OC_DONE : OverlayMessage::OC_OVERLAY;
}

void CDVDOverlayCodecTeletext::Reset()
{
  Flush();
}

void CDVDOverlayCodecTeletext::Flush()
{
  m_pOverlay.reset();
  m_libass->FlushEvents();
  m_order = 0;
}

std::shared_ptr<CDVDOverlay> CDVDOverlayCodecTeletext::GetOverlay()
{
  if (m_pOverlay)
  {
    std::shared_ptr<CDVDOverlay> ret = m_pOverlay;
    m_pOverlay.reset();
    return ret;
  }

  m_pOverlay = std::make_shared<CDVDOverlayTeletext>(m_libass);
  return m_pOverlay;
}

std::string CDVDOverlayCodecTeletext::ConvertTeletextToASS(const uint8_t* teletextData,
                                                           int dataSize)
{
  if (!teletextData || dataSize < 2)
    return "";

  std::string result;
  uint8_t currentColor = 7; // Default white
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool doubleHeight = false;
  bool doubleWidth = false;
  bool mosaic = false;
  bool flash = false;

  // Process teletext data bytes starting from byte 2 (skip data identifier and length)
  for (int i = 2; i < dataSize; i++)
  {
    uint8_t byte = teletextData[i];

    // Control codes are in the range 0x00-0x1F
    if (byte < 0x20)
    {
      // Teletext control codes
      switch (byte)
      {
        case 0x00: // NULL - ignore
          break;
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
          // Colors (1-7)
          ApplyTeletextAttribute(result, 0x01 + (byte - 1), false); // Close previous color
          currentColor = byte;
          ApplyTeletextAttribute(result, byte, true); // Open new color
          break;
        case 0x08: // Flash
          if (!flash)
          {
            flash = true;
            result += "{\\1a&H00&}"; // Use alpha for flash simulation
          }
          break;
        case 0x09: // Steady (end flash)
          if (flash)
          {
            flash = false;
            result += "{\\1a&HFF&}"; // Reset alpha
          }
          break;
        case 0x0A: // End box
          result += "{\\c}";
          break;
        case 0x0B: // Start box
          result += "{\\b1}";
          break;
        case 0x0C: // Normal size
          if (doubleHeight || doubleWidth)
          {
            doubleHeight = false;
            doubleWidth = false;
            // Kodi doesn't have a direct scale reset, use approximation
          }
          break;
        case 0x0D: // Double height
          doubleHeight = true;
          break;
        case 0x0E: // Double width
          doubleWidth = true;
          break;
        case 0x0F: // Double size (both)
          doubleHeight = true;
          doubleWidth = true;
          break;
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
          // Mosaic colors (0x10-0x17)
          mosaic = true;
          currentColor = byte - 0x10;
          ApplyTeletextAttribute(result, byte - 0x10, true);
          break;
        case 0x18: // Conceal
          result += "{\\1a&HFF&}"; // Make invisible with alpha
          break;
        case 0x19: // Contiguous mosaic
          mosaic = true;
          break;
        case 0x1A: // Separated mosaic
          mosaic = false;
          break;
        case 0x1B: // Escape
          // Next byte is escaped, skip for now
          if (i + 1 < dataSize)
            i++;
          break;
        case 0x1C: // Black background
          result += "{\\3c&H000000&}";
          break;
        case 0x1D: // New background
          result += "{\\3c}";
          break;
        case 0x1E: // Hold mosaic
          // Continue displaying last mosaic character
          break;
        case 0x1F: // Release mosaic
          mosaic = false;
          break;
        default:
          break;
      }
    }
    else if (byte >= 0x20 && byte < 0x7F)
    {
      // Printable character
      if (mosaic)
      {
        // Mosaic characters use special rendering
        // For now, replace with a block character
        result += "\xe2\x96\x88"; // UTF-8 encoding of \u2588 (full block)
      }
      else
      {
        // Regular ASCII character
        result += static_cast<char>(byte);
      }
    }
    // bytes >= 0x7F are typically graphic characters in teletext
  }

  // Close any open tags at end of subtitle
  if (bold)
    result += "{\\b0}";
  if (italic)
    result += "{\\i0}";
  if (underline)
    result += "{\\u0}";

  return result;
}

std::string CDVDOverlayCodecTeletext::ConvertTeletextColor(uint8_t teletextColor)
{
  if (teletextColor >= 8)
    teletextColor = 7; // Default to white

  uint32_t color = TELETEXT_COLORS[teletextColor];
  return StringUtils::Format("{{\\c&H{:06x}&}}", color);
}

void CDVDOverlayCodecTeletext::ApplyTeletextAttribute(std::string& text,
                                                      uint8_t attribute,
                                                      bool opening)
{
  switch (attribute)
  {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
      // Text colors
      text += ConvertTeletextColor(attribute);
      break;
    default:
      break;
  }
}
