/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerTeletext.h"

#include "DVDStreamInfo.h"
#include "DVDSubtitles/DVBTeletextParser.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <mutex>

using namespace std::chrono_literals;

const uint8_t rev_lut[32] =
{
  0x00,0x08,0x04,0x0c, /*  upper nibble */
  0x02,0x0a,0x06,0x0e,
  0x01,0x09,0x05,0x0d,
  0x03,0x0b,0x07,0x0f,
  0x00,0x80,0x40,0xc0, /*  lower nibble */
  0x20,0xa0,0x60,0xe0,
  0x10,0x90,0x50,0xd0,
  0x30,0xb0,0x70,0xf0
};

void CDVDTeletextTools::NextDec(int *i) /* skip to next decimal */
{
  (*i)++;

  if ((*i & 0x0F) > 0x09)
    *i += 0x06;

  if ((*i & 0xF0) > 0x90)
    *i += 0x60;

  if (*i > 0x899)
    *i = 0x100;
}

void CDVDTeletextTools::PrevDec(int *i)           /* counting down */
{
  (*i)--;

  if ((*i & 0x0F) > 0x09)
    *i -= 0x06;

  if ((*i & 0xF0) > 0x90)
    *i -= 0x60;

  if (*i < 0x100)
    *i = 0x899;
}

/* print hex-number into string, s points to last digit, caller has to provide enough space, no termination */
void CDVDTeletextTools::Hex2Str(char *s, unsigned int n)
{
  do {
    char c = (n & 0xF);
    *s-- = number2char(c);
    n >>= 4;
  } while (n);
}

signed int CDVDTeletextTools::deh24(unsigned char *p)
{
  int e = hamm24par[0][p[0]]
    ^ hamm24par[1][p[1]]
    ^ hamm24par[2][p[2]];

  int x = hamm24val[p[0]]
    + (p[1] & 127) * 16
    + (p[2] & 127) * 2048;

  return (x ^ hamm24cor[e]) | hamm24err[e];
}


CDVDTeletextData::CDVDTeletextData(CProcessInfo &processInfo)
: CThread("DVDTeletextData")
, IDVDStreamPlayer(processInfo)
, m_messageQueue("teletext")
, m_parser(std::make_shared<CDVBTeletextParser>())
{
  m_speed = DVD_PLAYSPEED_NORMAL;

  m_messageQueue.SetMaxDataSize(40 * 256 * 1024);

  m_TXTCache = m_parser->GetCache();
}

CDVDTeletextData::~CDVDTeletextData()
{
  StopThread();
  if (m_parser)
    m_parser->Reset();
}

bool CDVDTeletextData::CheckStream(CDVDStreamInfo &hints)
{
  if (hints.codec == AV_CODEC_ID_DVB_TELETEXT)
    return true;

  return false;
}

bool CDVDTeletextData::OpenStream(CDVDStreamInfo hints)
{
  CloseStream(true);

  m_messageQueue.Init();

  if (hints.codec == AV_CODEC_ID_DVB_TELETEXT)
  {
    CLog::Log(LOGINFO, "Creating teletext data thread");
    Create();
    return true;
  }

  return false;
}

void CDVDTeletextData::CloseStream(bool bWaitForBuffers)
{
  m_messageQueue.Abort();

  // wait for decode_video thread to end
  CLog::Log(LOGINFO, "waiting for teletext data thread to exit");

  StopThread(); // will set this->m_bStop to true

  m_messageQueue.End();
  if (m_parser)
    m_parser->Reset();
}

void CDVDTeletextData::ResetTeletextCache()
{
  if (m_parser)
    m_parser->Reset();
}

void CDVDTeletextData::Process()
{
  CLog::Log(LOGINFO, "running thread: CDVDTeletextData");

  while (!m_bStop)
  {
    std::shared_ptr<CDVDMsg> pMsg;
    int iPriority = (m_speed == DVD_PLAYSPEED_PAUSE) ? 1 : 0;
    MsgQueueReturnCode ret = m_messageQueue.Get(pMsg, 2s, iPriority);

    if (ret == MSGQ_TIMEOUT)
    {
      /* Timeout for Teletext is not a bad thing, so we continue without error */
      continue;
    }

    if (MSGQ_IS_ERROR(ret))
    {
      if (!m_messageQueue.ReceivedAbortRequest())
        CLog::Log(LOGERROR, "MSGQ_IS_ERROR returned true ({})", ret);

      break;
    }

    if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacket();
      
      if (m_parser)
        m_parser->Parse(pPacket);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH)
          || pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if (m_parser)
        m_parser->Reset();
    }
  }
}

void CDVDTeletextData::OnExit()
{
  CLog::Log(LOGINFO, "thread end: data_thread");
}

void CDVDTeletextData::Flush()
{
  if(!m_messageQueue.IsInited())
    return;
  /* flush using message as this get's called from VideoPlayer thread */
  /* and any demux packet that has been taken out of queue need to */
  /* be disposed of before we flush */
  m_messageQueue.Flush();
  m_messageQueue.Put(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_FLUSH));
}

void CDVDTeletextData::Decode_p2829(unsigned char *vtxt_row, TextExtData_t **ptExtData)
{
  unsigned int bitsleft, colorindex;
  unsigned char *p;
  int t1 = CDVDTeletextTools::deh24(&vtxt_row[7-4]);
  int t2 = CDVDTeletextTools::deh24(&vtxt_row[10-4]);

  if (t1 < 0 || t2 < 0)
    return;

  if (!(*ptExtData))
    (*ptExtData) = (TextExtData_t*) calloc(1, sizeof(TextExtData_t));
  if (!(*ptExtData))
    return;

  (*ptExtData)->p28Received = 1;
  (*ptExtData)->DefaultCharset = (t1>>7) & 0x7f;
  (*ptExtData)->SecondCharset = ((t1>>14) & 0x0f) | ((t2<<4) & 0x70);
  (*ptExtData)->LSP = !!(t2 & 0x08);
  (*ptExtData)->RSP = !!(t2 & 0x10);
  (*ptExtData)->SPL25 = !!(t2 & 0x20);
  (*ptExtData)->LSPColumns = (t2>>6) & 0x0f;

  bitsleft = 8; /* # of bits not evaluated in val */
  t2 >>= 10; /* current data */
  p = &vtxt_row[13-4];  /* pointer to next data triplet */
  for (colorindex = 0; colorindex < 16; colorindex++)
  {
    if (bitsleft < 12)
    {
      t2 |= CDVDTeletextTools::deh24(p) << bitsleft;
      if (t2 < 0)  /* hamming error */
        break;
      p += 3;
      bitsleft += 18;
    }
    (*ptExtData)->bgr[colorindex] = t2 & 0x0fff;
    bitsleft -= 12;
    t2 >>= 12;
  }
  if (t2 < 0 || bitsleft != 14)
  {
    (*ptExtData)->p28Received = 0;
    return;
  }
  (*ptExtData)->DefScreenColor = t2 & 0x1f;
  t2 >>= 5;
  (*ptExtData)->DefRowColor = t2 & 0x1f;
  (*ptExtData)->BlackBgSubst = !!(t2 & 0x20);
  t2 >>= 6;
  (*ptExtData)->ColorTableRemapping = t2 & 0x07;
}

void CDVDTeletextData::SavePage(int p, int sp, unsigned char* buffer)
{
  if (m_parser)
    m_parser->SavePage(p, sp, buffer);
}

void CDVDTeletextData::LoadPage(int p, int sp, unsigned char* buffer)
{
  if (m_parser)
    m_parser->LoadPage(p, sp, buffer);
}

void CDVDTeletextData::ErasePage(int magazine)
{
  std::unique_lock lock(m_TXTCache->m_critSection);
  TextCachedPage_t* pg = m_TXTCache->astCachetable[m_TXTCache->CurrentPage[magazine]][m_TXTCache->CurrentSubPage[magazine]];
  if (pg)
  {
    memset(&(pg->pageinfo), 0, sizeof(TextPageinfo_t));  /* struct pageinfo */
    memset(pg->p0, ' ', 24);
    memset(pg->data, ' ', 23*40);
  }
}

void CDVDTeletextData::AllocateCache(int magazine)
{
  /* check cachetable and allocate memory if needed */
  if (m_TXTCache->astCachetable[m_TXTCache->CurrentPage[magazine]][m_TXTCache->CurrentSubPage[magazine]] == 0)
  {
    m_TXTCache->astCachetable[m_TXTCache->CurrentPage[magazine]][m_TXTCache->CurrentSubPage[magazine]] = new TextCachedPage_t;
    if (m_TXTCache->astCachetable[m_TXTCache->CurrentPage[magazine]][m_TXTCache->CurrentSubPage[magazine]] )
    {
      ErasePage(magazine);
      m_TXTCache->CachedPages++;
    }
  }
}
