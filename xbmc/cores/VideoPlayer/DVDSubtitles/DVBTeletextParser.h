/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "video/TeletextDefines.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct DemuxPacket;

struct DVBTeletextPage
{
  int service{-1};   // magazine, 1..8
  int page{-1};      // e.g. 0x801 or 0x888
  int subPage{-1};   // -1 means current/automatic
  int language{NAT_DEFAULT};
};

class CDVBTeletextParser
{
public:
  CDVBTeletextParser();
  ~CDVBTeletextParser();

  void Reset();
  void Parse(DemuxPacket* packet);
  void GetCache();
  void LoadPage(int p, int sp, unsigned char* buffer);
  void SavePage(int p, int sp, unsigned char* buffer);

  std::vector<DVBTeletextPage> GetSubtitlePages() const;

  bool GetPageRows(int page,
                   int subPage,
                   std::vector<uint8_t>& rows,
                   DVBTeletextPage& metadata) const;

private:
  mutable std::mutex m_mutex;
  std::shared_ptr<TextCacheStruct_t> m_cache;
};
