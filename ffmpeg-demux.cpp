#include <stdint.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/mem.h"
}

const auto rtp_buff_size = 1500;
const auto static_session_description =
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "c=IN IP4 127.0.0.1\r\n"
    "m=video 5000 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96\r\n"
    "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f'"
    "\r\n";

std::vector<std::string> rtp_file_list;

void populate_rtp_buffer_list() {
  for (const auto &entry :
       std::filesystem::directory_iterator("/Users/sean/rtp-pkts")) {
    rtp_file_list.push_back(entry.path());
  }
  std::sort(rtp_file_list.begin(), rtp_file_list.end());
}

std::string print_av_error(int err) {
  char buff[500] = {0};
  av_strerror(err, buff, 500);
  return std::string(buff);
}

AVIOContext *create_session_description_avio_context(bool *have_read) {
  auto avio_context = avio_alloc_context(
      reinterpret_cast<unsigned char *>(av_strdup(static_session_description)),
      strlen(static_session_description), 0, have_read,
      [](void *opaque, uint8_t *buf, int buf_size) -> int {
        auto have_read = static_cast<bool *>(opaque);
        if (*have_read == true) {
          return AVERROR_EOF;
        }

        strncpy(reinterpret_cast<char *>(buf), static_session_description,
                buf_size);
        *have_read = true;
        return strlen(static_session_description);
      },
      NULL, NULL);

  if (avio_context == nullptr) {
    throw std::runtime_error("Failed to create avio_context");
  }

  return avio_context;
}

AVIOContext *create_rtp_avio_context(int *rtp_file_index) {
  auto avio_context = avio_alloc_context(
      static_cast<unsigned char *>(av_malloc(rtp_buff_size)), rtp_buff_size, 1,
      rtp_file_index,
      [](void *opaque, uint8_t *buf, int buf_size) -> int {
        auto rtp_file_index = static_cast<int *>(opaque);

        if (*rtp_file_index >= rtp_file_list.size()) {
          return AVERROR_EOF;
        }

        auto fp = fopen(rtp_file_list[*rtp_file_index].c_str(), "rb");
        auto amount_read = fread(buf, 1, rtp_buff_size, fp);
        fclose(fp);

        (*rtp_file_index)++;

        return amount_read;
      },
      // Ignore RTCP Packets. Must be set
      [](void *, uint8_t *, int buf_size) -> int { return buf_size; }, NULL);

  if (avio_context == nullptr) {
    throw std::runtime_error("Failed to create avio_context");
  }

  return avio_context;
}

int main() {
  populate_rtp_buffer_list();
  bool have_read = false;
  int rtp_file_index = 0;
  auto session_description_avio_context =
      create_session_description_avio_context(&have_read);
  auto rtp_avio_context = create_rtp_avio_context(&rtp_file_index);

  auto avformat_context = avformat_alloc_context();
  if (avformat_context == nullptr) {
    throw std::runtime_error("Failed to create avformat_context");
  }
  avformat_context->pb = session_description_avio_context;

  AVDictionary *avformat_open_input_options = nullptr;
  av_dict_set(&avformat_open_input_options, "sdp_flags", "custom_io", 0);
  av_dict_set_int(&avformat_open_input_options, "reorder_queue_size", 0, 0);

  int status = 0;
  if ((status = avformat_open_input(&avformat_context, "", nullptr,
                                    &avformat_open_input_options)) != 0) {
    throw std::runtime_error("Failed to avformat_open_input " +
                             print_av_error(status));
  }
  avformat_context->pb = rtp_avio_context;

  if ((status = avformat_find_stream_info(avformat_context, nullptr)) != 0) {
    throw std::runtime_error("Failed to avformat_find_stream_info " +
                             print_av_error(status));
  }

  AVCodecContext *av_codec_context_audio = nullptr;
  AVCodecContext *av_codec_context_video = nullptr;

  for (int i = 0; i < avformat_context->nb_streams; i++) {
    auto avstream = avformat_context->streams[i];
    auto decoder = avcodec_find_decoder(avstream->codecpar->codec_id);
    auto avcodec_context = av_codec_context_audio =
        avcodec_alloc_context3(decoder);

    if ((status = avcodec_open2(avcodec_context, decoder, NULL)) != 0) {
      throw std::runtime_error("Failed to avcodec_open2 " +
                               print_av_error(status));
    }

    if (avstream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      av_codec_context_audio = avcodec_context;
    } else if (avstream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      av_codec_context_video = avcodec_context;
    }
  }

  AVPacket packet;
  AVFrame *frame = av_frame_alloc();
  while (av_read_frame(avformat_context, &packet) >= 0) {
    AVCodecContext *av_codec_context = NULL;
    auto *avstream = avformat_context->streams[packet.stream_index];
    auto is_audio = avstream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;

    if (is_audio) {
      av_codec_context = av_codec_context_audio;
    } else {
      av_codec_context = av_codec_context_video;
    }

    status = avcodec_send_packet(av_codec_context, &packet);
    if (status != AVERROR_INVALIDDATA && status != 0) {
      throw std::runtime_error("Failed to avcodec_send_packet " +
                               print_av_error(status));
    }

    status = avcodec_receive_frame(av_codec_context, frame);
    if (status != AVERROR(EAGAIN) && status != 0) {
      throw std::runtime_error("Failed to avcodec_receive_frame " +
                               print_av_error(status));
    }

    av_packet_unref(&packet);
  }

  avformat_free_context(avformat_context);
  avio_context_free(&session_description_avio_context);

  return 0;
}

