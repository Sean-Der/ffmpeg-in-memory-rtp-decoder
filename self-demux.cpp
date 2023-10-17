#include <stdint.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <rtc/rtc.hpp>
#include <string>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/mem.h"
}

const auto buff_size = 1024 * 32;
const auto naluTypeBitmask = std::byte(0x1F);
const auto naluTypeSTAPA = std::byte(24);
const auto naluTypeFUA = std::byte(28);
const auto stapaHeaderSize = 1;
const auto fuaHeaderSize = 2;
const auto rtpHeaderSize = 12;

const auto fuaEndBitmask = std::byte(0x40);
const auto naluRefIdcBitmask = std::byte(0x60);

std::vector<std::vector<std::byte>> h264_frames;
std::vector<std::vector<std::byte>> rtp_pkts;
std::vector<std::byte> fua_buffer;

std::vector<std::byte> h264_nalu_header()
{
	return std::vector<std::byte>{std::byte(0), std::byte(0), std::byte(0),
				      std::byte(1)};
}

void depacketize_h264() {
	auto pkt = rtp_pkts[0];
	auto pktParsed = reinterpret_cast<const rtc::RtpHeader *>(pkt.data());
	auto headerSize = rtpHeaderSize + pktParsed->csrcCount() +
			  pktParsed->getExtensionHeaderSize();
	auto naluType = pkt.at(headerSize) & naluTypeBitmask;

	if (naluType > std::byte(0) && naluType < std::byte(24)) {
		auto h264_nalu = h264_nalu_header();
		std::copy(pkt.begin() + headerSize, pkt.end(),
			  std::back_inserter(h264_nalu));
		h264_frames.push_back(h264_nalu);
	} else if (naluType == naluTypeSTAPA) {
		auto currOffset = stapaHeaderSize + headerSize;
		while (currOffset < pkt.size()) {
			auto h264_nalu = h264_nalu_header();

			auto naluSize = uint16_t(pkt.at(currOffset)) << 8 | uint8_t(pkt.at(currOffset + 1));

			currOffset += 2;

			if (pkt.size() < currOffset + naluSize) {
				throw std::runtime_error(
					"STAP-A declared size is larger then buffer");
			}

			std::copy(pkt.begin() + currOffset,
				  pkt.begin() + currOffset + naluSize,
				  std::back_inserter(h264_nalu));
			currOffset += naluSize;

			h264_frames.push_back(h264_nalu);
		}
	} else if (naluType == naluTypeFUA) {
		if (fua_buffer.size() == 0) {
			fua_buffer = h264_nalu_header();
			fua_buffer.push_back(std::byte(0));
		}

		std::copy(pkt.begin() + headerSize + fuaHeaderSize, pkt.end(),
			  std::back_inserter(fua_buffer));

		if ((pkt.at(headerSize + 1) & fuaEndBitmask) != std::byte(0)) {
			auto naluRefIdc = pkt.at(headerSize) &
					  naluRefIdcBitmask;
			auto fragmentedNaluType = pkt.at(headerSize + 1) &
						  std::byte(naluTypeBitmask);

			fua_buffer[4] = naluRefIdc | fragmentedNaluType;

			h264_frames.push_back(fua_buffer);
			fua_buffer = std::vector<std::byte>{};
		}
	} else {
		throw std::runtime_error("Unknown H264 RTP Packetization");
	}
}

void populate_buffer_list() {
  char buffer[1500];

  for (const auto &entry :
       std::filesystem::directory_iterator("/Users/sean/rtp-pkts")) {
    auto fd = fopen(entry.path().c_str(), "rb");
    auto n = fread(buffer, sizeof(char), 1500, fd);
    fclose(fd);

    rtp_pkts.push_back(std::vector<std::byte>(reinterpret_cast<std::byte*>(buffer), reinterpret_cast<std::byte*>(buffer + n)));
  }

   std::sort(rtp_pkts.begin(), rtp_pkts.end(),
             [](auto const &pr1, auto const &pr2) {
               auto pkt1 = reinterpret_cast<const rtc::RtpHeader *>(pr1.data());
               auto pkt2 = reinterpret_cast<const rtc::RtpHeader *>(pr2.data());
               return pkt2->seqNumber() > pkt1->seqNumber();
             });

  while (true) {
    uint32_t currentTimestamp = 0;
    size_t i = 0;

    for (const auto &pkt : rtp_pkts) {
      auto p = reinterpret_cast<const rtc::RtpHeader *>(pkt.data());

      if (i == 0) {
        currentTimestamp = p->timestamp();
      }

      if (currentTimestamp != p->timestamp()) {
        break;
      }

      i++;
    }

    if (i == rtp_pkts.size()) {
      break;
    } else {
      for (auto j = 0; j < i; j++) {
        depacketize_h264();
        rtp_pkts.erase(rtp_pkts.begin());
      }
    }
  }
}

std::string print_av_error(int err) {
  char buff[500] = {0};
  av_strerror(err, buff, 500);
  return std::string(buff);
}

AVIOContext *create_avio_context(int *h264_frame_index) {
  auto avio_context = avio_alloc_context(
      static_cast<unsigned char *>(av_malloc(buff_size)), buff_size, 0,
      h264_frame_index,
      [](void *opaque, uint8_t *buf, int buf_size) -> int {
        auto h264_frame_index = static_cast<int *>(opaque);

        if (*h264_frame_index >= h264_frames.size()) {
          return AVERROR_EOF;
        }

        std::memcpy(buf, h264_frames[*h264_frame_index].data(),
                    h264_frames[*h264_frame_index].size());

        (*h264_frame_index)++;

        return h264_frames[*h264_frame_index].size();
      },
      NULL, NULL);

  if (avio_context == nullptr) {
    throw std::runtime_error("Failed to create avio_context");
  }

  return avio_context;
}

int main() {
  populate_buffer_list();

  int h264_frame_index = 0;
  auto avio_context = create_avio_context(&h264_frame_index);

  auto avformat_context = avformat_alloc_context();
  if (avformat_context == nullptr) {
    throw std::runtime_error("Failed to create avformat_context");
  }
  avformat_context->pb = avio_context;

  int status = 0;
  if ((status = avformat_open_input(&avformat_context, "", nullptr, nullptr)) !=
      0) {
    throw std::runtime_error("Failed to avformat_open_input " +
                             print_av_error(status));
  }

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
  avio_context_free(&avio_context);

  return 0;
}
