/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDDemux.h"
#include "language/LanguageTag.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

/*!
 * \brief Pseudo demuxer that turns DVB teletext subtitle pages into text subtitle streams.
 *
 * Teletext packets (EN 300 472 PES payload, codec AV_CODEC_ID_DVB_TELETEXT) are fed in with
 * Read(DemuxPacket*). Every teletext page flagged as a subtitle page (header control bit C6, or
 * announced in the DVB teletext_descriptor) becomes a CDemuxStreamSubtitle with codec
 * AV_CODEC_ID_TEXT. Whenever the content of such a page changes, a packet holding the boxed text
 * of the page as UTF-8 (with optional font colour tags) is produced, in the same way tvheadend
 * exposes "TEXTSUB" streams. An empty packet is produced when the page is cleared.
 */
class CDVDDemuxTeletext : public CDVDDemux
{
public:
  /*!
   * \param descriptor Payload of a DVB teletext_descriptor (EN 300 468 6.2.43): 5 bytes per entry
   *        (ISO 639 language, teletext_type/magazine, page number). May be empty.
   * \param size Size of descriptor in bytes.
   * \param language Fallback language for pages that are not listed in the descriptor.
   */
  CDVDDemuxTeletext(const uint8_t* descriptor,
                    size_t size,
                    const KODI::LANGUAGE::CLanguageTag& language);
  ~CDVDDemuxTeletext() override;

  bool Reset() override;
  void Flush() override;
  DemuxPacket* Read() override;
  bool SeekTime(double time, bool backwards = false, double* startpts = nullptr) override
  {
    return true;
  }
  CDemuxStream* GetStream(int iStreamId) const override;
  std::vector<CDemuxStream*> GetStreams() const override;
  int GetNrOfStreams() const override;

  /*!
   * \brief Feed a teletext PES packet and return the first produced subtitle packet, if any.
   *        Call again with nullptr until nullptr is returned to drain remaining packets.
   * \param packet The teletext packet. Ownership stays with the caller.
   */
  DemuxPacket* Read(DemuxPacket* packet);

  /*!
   * \brief Convert a page buffer (23 rows x 40 columns, packets X/1..X/23) to subtitle text.
   * \param page The page buffer, 7-bit character codes with spacing attributes.
   * \param nationalSubset NAT_* value for the primary G0 set.
   * \param secondNationalSubset NAT_* value for the G0 set selected by ESC (0x1B).
   * \param withColours Emit font colour tags for non-white text.
   * \return UTF-8 text, rows separated by '\n'. Only boxed text (between 0x0B and 0x0A) is kept.
   */
  static std::string PageToText(const uint8_t* page,
                                int nationalSubset,
                                int secondNationalSubset,
                                bool withColours = true);

private:
  static constexpr int ROWS = 23;
  static constexpr int COLUMNS = 40;

  struct Magazine
  {
    int page{-1}; // page currently being received, -1 if none / not decodable
    bool subtitle{false}; // header control bit C6
    int national{0}; // header control bits C12..C14
    bool charsetValid{false}; // X/28/0 received for this page
    uint8_t defaultCharset{0};
    uint8_t secondCharset{0};
    double pts{0};
    bool ptsValid{false};
    std::array<uint8_t, ROWS * COLUMNS> data{};
  };

  struct Page
  {
    int page{0};
    CDemuxStreamSubtitle stream;
    std::string lastText;
    bool blank{true};
  };

  void DecodeLine(const uint8_t* row, double pts, bool ptsValid);
  void DeliverPage(Magazine& mag);
  Page* FindPage(int page) const;
  Page& AddPage(int page, const KODI::LANGUAGE::CLanguageTag& language, StreamFlags flags);
  void ResetMagazines();

  std::array<Magazine, 8> m_magazines;
  std::array<bool, 8> m_magCharsetValid{}; // M/29/0 received
  std::array<uint8_t, 8> m_magDefaultCharset{};
  std::array<uint8_t, 8> m_magSecondCharset{};
  std::vector<std::unique_ptr<Page>> m_pages;
  std::deque<DemuxPacket*> m_pending;
  KODI::LANGUAGE::CLanguageTag m_language;
};
