/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "video/Teletext.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace
{
TextPageAttr_t VisibleAttr(enumTeletextColor fg = TXT_ColorWhite,
                           enumTeletextColor bg = TXT_ColorBlack)
{
  TextPageAttr_t attr{};
  attr.fg = fg;
  attr.bg = bg;
  attr.charset = C_G0P;
  attr.setG0G2 = 0x3f;
  return attr;
}
} // namespace

TEST(TestTeletext, GetSubtitlePagesReturnsCachedPages)
{
  auto cache = std::make_shared<TextCacheStruct_t>();
  cache->SubtitlePages[0] = {0x888, NAT_UK};
  cache->SubtitlePages[1] = {0, NAT_DEFAULT};
  cache->SubtitlePages[2] = {0x889, NAT_DE};

  const auto pages = CTeletextDecoder::GetSubtitlePages(cache);

  ASSERT_EQ(2u, pages.size());
  EXPECT_EQ(0x888, pages[0].page);
  EXPECT_EQ(NAT_UK, pages[0].language);
  EXPECT_EQ(0x889, pages[1].page);
  EXPECT_EQ(NAT_DE, pages[1].language);
}

TEST(TestTeletext, ConvertSubtitlePageToASSEscapesAndConcatenatesRows)
{
  unsigned char pageChar[TELETEXT_PAGE_SIZE];
  TextPageAttr_t pageAtrb[TELETEXT_PAGE_SIZE];
  std::fill_n(pageChar, TELETEXT_PAGE_SIZE, ' ');
  std::fill_n(pageAtrb, TELETEXT_PAGE_SIZE, VisibleAttr(TXT_ColorTransp, TXT_ColorTransp));

  pageChar[5 * 40 + 2] = 'A';
  pageChar[5 * 40 + 3] = '{';
  pageChar[5 * 40 + 4] = '\\';
  pageChar[5 * 40 + 5] = '}';
  pageChar[6 * 40 + 2] = 'B';

  for (int col = 2; col <= 5; ++col)
    pageAtrb[5 * 40 + col] = VisibleAttr();
  pageAtrb[6 * 40 + 2] = VisibleAttr();

  const std::string assText = CTeletextDecoder::ConvertSubtitlePageToASS(pageChar, pageAtrb);

  EXPECT_EQ(0u, assText.rfind("{\\an2\\pos(960,972)\\q2\\fnmonospace\\fs43}", 0));
  EXPECT_NE(std::string::npos, assText.find("A\\{\\\\\\}"));
  EXPECT_EQ(std::string::npos, assText.find("\\N"));
  EXPECT_NE(std::string::npos, assText.find("\\h\\hB"));
}

TEST(TestTeletext, ConvertSubtitlePageToASSPreservesColorsAndDoubleHeight)
{
  unsigned char pageChar[TELETEXT_PAGE_SIZE];
  TextPageAttr_t pageAtrb[TELETEXT_PAGE_SIZE];
  std::fill_n(pageChar, TELETEXT_PAGE_SIZE, ' ');
  std::fill_n(pageAtrb, TELETEXT_PAGE_SIZE, VisibleAttr(TXT_ColorTransp, TXT_ColorTransp));

  pageChar[10 * 40 + 8] = 'R';
  pageChar[10 * 40 + 9] = 'G';
  pageChar[10 * 40 + 10] = 'H';
  pageAtrb[10 * 40 + 8] = VisibleAttr(TXT_ColorRed);
  pageAtrb[10 * 40 + 9] = VisibleAttr(TXT_ColorGreen);
  pageAtrb[10 * 40 + 10] = VisibleAttr();
  pageAtrb[10 * 40 + 10].doubleh = 1;

  const std::string assText = CTeletextDecoder::ConvertSubtitlePageToASS(pageChar, pageAtrb);

  EXPECT_NE(std::string::npos, assText.find("\\c&H1414FC&"));
  EXPECT_NE(std::string::npos, assText.find("\\c&H24FC24&"));
  EXPECT_NE(std::string::npos, assText.find("\\fscy200"));
}

TEST(TestTeletext, ConvertSubtitlePageToASSHidesConcealedCharacters)
{
  unsigned char pageChar[TELETEXT_PAGE_SIZE];
  TextPageAttr_t pageAtrb[TELETEXT_PAGE_SIZE];
  std::fill_n(pageChar, TELETEXT_PAGE_SIZE, ' ');
  std::fill_n(pageAtrb, TELETEXT_PAGE_SIZE, VisibleAttr(TXT_ColorTransp, TXT_ColorTransp));

  pageChar[8 * 40 + 4] = 'A';
  pageChar[8 * 40 + 5] = 'X';
  pageChar[8 * 40 + 6] = 'B';
  pageAtrb[8 * 40 + 4] = VisibleAttr();
  pageAtrb[8 * 40 + 5] = VisibleAttr(TXT_ColorBlack, TXT_ColorBlack);
  pageAtrb[8 * 40 + 6] = VisibleAttr();

  const std::string assText = CTeletextDecoder::ConvertSubtitlePageToASS(pageChar, pageAtrb);

  EXPECT_NE(std::string::npos, assText.find("A\\h"));
  EXPECT_EQ(std::string::npos, assText.find("X"));
  EXPECT_NE(std::string::npos, assText.find("B"));
}

TEST(TestTeletext, ConvertSubtitlePageToASSAppliesNationalSubsetAndDiacritics)
{
  unsigned char pageChar[TELETEXT_PAGE_SIZE];
  TextPageAttr_t pageAtrb[TELETEXT_PAGE_SIZE];
  std::fill_n(pageChar, TELETEXT_PAGE_SIZE, ' ');
  std::fill_n(pageAtrb, TELETEXT_PAGE_SIZE, VisibleAttr(TXT_ColorTransp, TXT_ColorTransp));

  pageChar[4 * 40 + 4] = '[';
  pageChar[4 * 40 + 5] = 'A';
  pageAtrb[4 * 40 + 4] = VisibleAttr();
  pageAtrb[4 * 40 + 5] = VisibleAttr();
  pageAtrb[4 * 40 + 5].diacrit = 2;

  const std::string assText =
      CTeletextDecoder::ConvertSubtitlePageToASS(pageChar, pageAtrb, NAT_DE, NAT_DEFAULT);

  EXPECT_NE(std::string::npos, assText.find("Ä"));
  EXPECT_NE(std::string::npos, assText.find("Á"));
}
