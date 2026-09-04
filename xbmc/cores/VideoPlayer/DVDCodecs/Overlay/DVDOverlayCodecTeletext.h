/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDOverlayCodec.h"
#include "DVDOverlayLibass.h"
#include "cores/VideoPlayer/DVDSubtitles/DVDSubtitlesLibass.h"

#include <memory>
#include <string>

class CDVDOverlayTeletext;

class CDVDOverlayCodecTeletext : public CDVDOverlayCodec
{
public:
  CDVDOverlayCodecTeletext();
  ~CDVDOverlayCodecTeletext() override;

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  OverlayMessage Decode(DemuxPacket* pPacket) override;
  void Reset() override;
  void Flush() override;
  std::shared_ptr<CDVDOverlay> GetOverlay() override;

private:
  /*!
   * \brief Convert DVB Teletext raw text to libass format
   * \param teletextData Raw teletext packet data
   * \param dataSize Size of packet data
   * \return ASS formatted text with styling tags
   */
  std::string ConvertTeletextToASS(const uint8_t* teletextData, int dataSize);

  /*!
   * \brief Convert teletext color codes to ASS color format (BGR)
   * \param teletextColor Teletext color index (0-7)
   * \return ASS color string (e.g., "&H00FFFF&" for yellow)
   */
  std::string ConvertTeletextColor(uint8_t teletextColor);

  /*!
   * \brief Apply teletext spacing attributes to text
   * \param text Reference to text being built
   * \param attribute Teletext control code
   * \param opening True if opening tag, false if closing
   */
  void ApplyTeletextAttribute(std::string& text, uint8_t attribute, bool opening);

  std::shared_ptr<CDVDSubtitlesLibass> m_libass;
  std::shared_ptr<CDVDOverlayTeletext> m_pOverlay;
  int m_order;
};
