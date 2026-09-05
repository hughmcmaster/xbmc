// This is a partial file showing the fix location around line 1865-1870
// In DVDDemuxFFmpeg::CreateStreams() method, AVMEDIA_TYPE_SUBTITLE case:

      case AVMEDIA_TYPE_SUBTITLE:
      {
        if (pStream->codecpar->codec_id == AV_CODEC_ID_DVB_TELETEXT && CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_TELETEXTENABLED))
        {
          CDemuxStreamTeletext* st = new CDemuxStreamTeletext();
          stream = st;
          stream->type = StreamType::TELETEXT;
          stream->codec = pStream->codecpar->codec_id;  // ← ADD THIS LINE
          break;
        }
        else
        {
          CDemuxStreamSubtitleFFmpeg* st = new CDemuxStreamSubtitleFFmpeg(pStream);
          stream = st;

          if (pStream->codecpar->codec_id == AV_CODEC_ID_WEBVTT)
          {
            stream->flags = static_cast<StreamFlags>(static_cast<int>(stream->flags) |
                                                     FLAG_WEBVTT_DATA_PACKETS);
          }

          if (av_dict_get(pStream->metadata, "title", NULL, 0))
            st->m_description = av_dict_get(pStream->metadata, "title", NULL, 0)->value;
          break;
        }
      }