/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDDemux.h"

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/*
 * Extracts DVB teletext subtitle pages (ETSI EN 300 706) from a teletext
 * elementary stream and exposes each page as a text subtitle stream.
 * The page text is emitted as UTF-8 with the SubRip-style tags understood
 * by CDVDSubtitleTagSami.
 */
class CDVDDemuxTeletext : public CDVDDemux
{
public:
  explicit CDVDDemuxTeletext(const std::vector<TeletextSubtitlePage>& pages);
  ~CDVDDemuxTeletext() override;

  bool Reset() override { return true; }
  void Flush() override;
  DemuxPacket* Read() override;
  bool SeekTime(double time, bool backwards = false, double* startpts = nullptr) override
  {
    return true;
  }
  CDemuxStream* GetStream(int iStreamId) const override;
  std::vector<CDemuxStream*> GetStreams() const override;
  int GetNrOfStreams() const override;

  /*
   * Feed a teletext PES packet (EN 300 472 data units). Returns the first
   * subtitle packet produced, further packets are returned by Read().
   */
  DemuxPacket* Read(DemuxPacket* packet);

  /*
   * Parse the teletext descriptor stored as codec extradata by ffmpeg, together
   * with the matching comma separated ISO 639-2 language list.
   */
  static std::vector<TeletextSubtitlePage> ParseDescriptor(const uint8_t* data,
                                                           size_t size,
                                                           std::string_view languages);

  /*
   * Convert a received page to subtitle text. Exposed for unit testing.
   */
  static std::string PageToText(const std::array<std::array<uint8_t, 40>, 24>& rows,
                                const std::array<bool, 24>& rowValid,
                                int primarySubset,
                                int secondarySubset);

private:
  struct Stream
  {
    std::unique_ptr<CDemuxStreamSubtitle> stream;
    std::string lastText;
  };

  struct Magazine
  {
    int page{-1}; // hex page currently being received, -1 when none
    bool subtitle{false}; // header C6 bit
    double pts{0.0};
    int headerNational{0};
    bool hasX28{false};
    int x28Default{0};
    int x28Second{0};
    bool hasM29{false};
    int m29Default{0};
    int m29Second{0};
    std::array<std::array<uint8_t, 40>, 24> rows{};
    std::array<bool, 24> rowValid{};
    bool dirty{false};
  };

  Stream* FindStream(int page);
  Stream& AddStream(const TeletextSubtitlePage& page);
  void ProcessRow(const uint8_t* row, double pts);
  void FinishPage(Magazine& mag);
  void EmitPacket(Stream& stream, const std::string& text, double pts);

  std::vector<Stream> m_streams;
  std::array<Magazine, 9> m_magazines; // index 1..8
  std::deque<DemuxPacket*> m_packets;
  bool m_serial{false}; // header C11, page completes on any magazine's header
};
