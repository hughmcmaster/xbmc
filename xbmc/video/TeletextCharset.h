/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <string>

/*
 * Character set tables defined in ETS 300 706, shared between the teletext
 * page renderer (CTeletextDecoder) and the teletext subtitle extractor
 * (CDVDDemuxTeletext).
 */

/* G2 supplementary sets: 0 Latin, 1 Cyrillic, 2 Greek, 3 Arabic, 4 Hebrew (diacritics) */
extern const unsigned short int G2table[5][6 * 16];
/* Full G0 remaps: 0 Serbian/Croatian, 1 Russian/Bulgarian, 2 Ukrainian, 3 Greek, 4 Hebrew, 5 Arabic */
extern const unsigned short int G0table[6][6 * 16];
/* Latin G0 national option subsets (NAT_DEFAULT .. NAT_TR), indexed by code position */
extern const unsigned short int nationaltable23[14][2];
extern const unsigned short int nationaltable40[14];
extern const unsigned short int nationaltable5b[14][6];
extern const unsigned short int nationaltable7b[14][4];

class CTeletextCharset
{
public:
  /*!
   * \brief Map a G0/G2 character set designation (packet X/28 or M/29, 7 bits) to a NAT_* subset.
   * \param designation default/second G0 charset + national option (ETS 300 706 table 32)
   * \return one of the NAT_* enum values from TeletextDefines.h
   */
  static int NationalFromDesignation(unsigned char designation);

  /*!
   * \brief Translate a Latin/Cyrillic/Greek/Hebrew/Arabic G0 character to Unicode.
   * \param nationalSubset one of the NAT_* enum values
   * \param ch character code 0x20..0x7F
   * \return UCS-2 code point, or 0 for a character that has no textual representation
   */
  static uint16_t G0ToUcs2(int nationalSubset, unsigned char ch);

  /*!
   * \brief Translate a G2 supplementary set character to Unicode.
   */
  static uint16_t G2ToUcs2(int nationalSubset, unsigned char ch);

  /*!
   * \brief Append a UCS-2 code point to a UTF-8 string. Code point 0 is skipped.
   */
  static void AppendUtf8(std::string& out, uint16_t ucs2);
};
