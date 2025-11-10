/**
 * @file src/platform/linux/picamera_capture.cpp
 * @brief Raspberry Pi camera capture backend definitions.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// lib includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

// local includes
#include "src/logging.h"
#include "src/platform/linux/picamera_capture.h"

using namespace std::literals;

namespace platf::picamera {

	namespace {
		constexpr auto DEFAULT_DEVICE = "/dev/video0"sv;

		void log_ffmpeg_error(const char *fn, int err) {
			char err_buf[AV_ERROR_MAX_STRING_SIZE] {};
			av_make_error_string(err_buf, AV_ERROR_MAX_STRING_SIZE, err);
			BOOST_LOG(error) << "PiCamera: " << fn << " failed: " << err_buf;
		}

		struct picamera_frame_t: public img_t {
			picamera_frame_t() {
				pixel_pitch = 4;
			}

			~picamera_frame_t() override {
				delete[] data;
				data = nullptr;
			}
		};

		class format_context_t {
		public:
			~format_context_t() {
				close();
			}

			bool open(const std::string &device, const video::config_t &cfg) {
				AVDictionary *options = nullptr;

				if (cfg.width > 0 && cfg.height > 0) {
					std::string size = std::to_string(cfg.width) + "x" + std::to_string(cfg.height);
					av_dict_set(&options, "video_size", size.c_str(), 0);
				}

				if (cfg.framerate > 0) {
					av_dict_set(&options, "framerate", std::to_string(cfg.framerate).c_str(), 0);
				}

				auto input_fmt = av_find_input_format("v4l2");
				if (!input_fmt) {
					BOOST_LOG(error) << "PiCamera: v4l2 input format not found";
					return false;
				}

				int result = avformat_open_input(&ctx, device.c_str(), input_fmt, &options);
				av_dict_free(&options);
				if (result < 0) {
					log_ffmpeg_error("avformat_open_input", result);
					return false;
				}

				result = avformat_find_stream_info(ctx, nullptr);
				if (result < 0) {
					log_ffmpeg_error("avformat_find_stream_info", result);
					return false;
				}

				stream_index = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
				if (stream_index < 0) {
					BOOST_LOG(error) << "PiCamera: no video stream found";
					return false;
				}

				return true;
			}

			void close() {
				if (ctx) {
					avformat_close_input(&ctx);
					ctx = nullptr;
					stream_index = -1;
				}
			}

			AVStream *stream() {
				if (!ctx || stream_index < 0) {
					return nullptr;
				}
				return ctx->streams[stream_index];
			}

			AVFormatContext *get() {
				return ctx;
			}

			int index() const {
				return stream_index;
			}

		private:
			AVFormatContext *ctx {nullptr};
			int stream_index {-1};
		};

		class codec_context_t {
		public:
			~codec_context_t() {
				if (ctx) {
					avcodec_free_context(&ctx);
				}
			}

			bool open(AVCodecParameters *params) {
				if (!params) {
					return false;
				}

				auto decoder = avcodec_find_decoder(params->codec_id);
				if (!decoder) {
					BOOST_LOG(error) << "PiCamera: decoder not available";
					return false;
				}

				ctx = avcodec_alloc_context3(decoder);
				if (!ctx) {
					BOOST_LOG(error) << "PiCamera: failed to allocate decoder context";
					return false;
				}

				if (avcodec_parameters_to_context(ctx, params) < 0) {
					BOOST_LOG(error) << "PiCamera: failed to initialise decoder context";
					return false;
				}

				if (avcodec_open2(ctx, decoder, nullptr) < 0) {
					BOOST_LOG(error) << "PiCamera: failed to open decoder";
					return false;
				}

				return true;
			}

			AVCodecContext *get() {
				return ctx;
			}

		private:
			AVCodecContext *ctx {nullptr};
		};

		class scale_context_t {
		public:
			~scale_context_t() {
				if (ctx) {
					sws_freeContext(ctx);
				}
			}

			bool configure(int src_w, int src_h, AVPixelFormat src_fmt, int dst_w, int dst_h) {
				ctx = sws_getCachedContext(ctx, src_w, src_h, src_fmt, dst_w, dst_h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
				if (!ctx) {
					BOOST_LOG(error) << "PiCamera: failed to configure scaler";
					return false;
				}
				return true;
			}

			SwsContext *get() {
				return ctx;
			}

		private:
			SwsContext *ctx {nullptr};
		};

		class picamera_display_t: public platf::display_t {
		public:
			explicit picamera_display_t(std::string device_path):
					device(std::move(device_path)) {
				width = 1280;
				height = 720;
			}

			~picamera_display_t() override {
				format.close();
			}

					bool init(const video::config_t &cfg) {
						delay = std::chrono::nanoseconds {1s} / std::max(1, cfg.framerate);

						if (!format.open(device, cfg)) {
					return false;
				}

				auto stream = format.stream();
				if (!stream) {
					return false;
				}

				width = stream->codecpar->width;
				height = stream->codecpar->height;
				env_width = width;
				env_height = height;

				if (!codec.open(stream->codecpar)) {
					return false;
				}

				BOOST_LOG(info) << "PiCamera: capturing from " << device;
				return true;
			}

			capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
				(void) cursor;

				auto next_frame = std::chrono::steady_clock::now();
				sleep_overshoot_logger.reset();

				while (true) {
					auto now = std::chrono::steady_clock::now();
					if (next_frame > now) {
						std::this_thread::sleep_for(next_frame - now);
						sleep_overshoot_logger.first_point(next_frame);
						sleep_overshoot_logger.second_point_now_and_log();
					}
					next_frame += delay;
					if (next_frame < now) {
						next_frame = now + delay;
					}

					std::shared_ptr<platf::img_t> img_out;
					if (!pull_free_image_cb(img_out)) {
						return capture_e::interrupted;
					}

					auto status = read_frame(*img_out);
					if (status == capture_e::ok) {
						if (!push_captured_image_cb(std::move(img_out), true)) {
							return capture_e::ok;
						}
					} else if (status == capture_e::timeout) {
						if (!push_captured_image_cb(std::move(img_out), false)) {
							return capture_e::ok;
						}
					} else {
						return status;
					}
				}
			}

			std::shared_ptr<img_t> alloc_img() override {
				auto image = std::make_shared<picamera_frame_t>();
				image->row_pitch = width * 4;
				image->height = height;
				image->data = new std::uint8_t[static_cast<size_t>(image->row_pitch) * image->height];
				return image;
			}

			int dummy_img(img_t *img) override {
				if (!img) {
					return -1;
				}
				if (!img->data) {
					img->row_pitch = width * 4;
					img->height = height;
					img->data = new std::uint8_t[static_cast<size_t>(img->row_pitch) * img->height];
				}
				std::memset(img->data, 0, static_cast<size_t>(img->row_pitch) * img->height);
				return 0;
			}

			std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override {
				(void) pix_fmt;
				return std::make_unique<avcodec_encode_device_t>();
			}

			bool is_codec_supported(std::string_view, const video::config_t &) override {
				return true;
			}

		private:
			capture_e read_frame(platf::img_t &img) {
				AVPacket packet;
				av_init_packet(&packet);
				packet.data = nullptr;
				packet.size = 0;

				int result = av_read_frame(format.get(), &packet);
				if (result < 0) {
					if (result == AVERROR(EAGAIN)) {
						return capture_e::timeout;
					}
					log_ffmpeg_error("av_read_frame", result);
					return capture_e::error;
				}

				if (packet.stream_index != format.index()) {
					av_packet_unref(&packet);
					return capture_e::timeout;
				}

				result = avcodec_send_packet(codec.get(), &packet);
				av_packet_unref(&packet);
				if (result < 0) {
					log_ffmpeg_error("avcodec_send_packet", result);
					return capture_e::error;
				}

				std::unique_ptr<AVFrame, decltype(&av_frame_free)> frame(av_frame_alloc(), &av_frame_free);
				if (!frame) {
					BOOST_LOG(error) << "PiCamera: failed to allocate frame";
					return capture_e::error;
				}

				result = avcodec_receive_frame(codec.get(), frame.get());
				if (result == AVERROR(EAGAIN)) {
					return capture_e::timeout;
				}
				if (result < 0) {
					log_ffmpeg_error("avcodec_receive_frame", result);
					return capture_e::error;
				}

				width = frame->width;
				height = frame->height;
				env_width = width;
				env_height = height;

				if (!scaler.configure(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height)) {
					return capture_e::error;
				}

				if (!img.data || img.row_pitch != frame->width * 4 || img.height != frame->height) {
					delete[] img.data;
					img.row_pitch = frame->width * 4;
					img.height = frame->height;
					img.data = new std::uint8_t[static_cast<size_t>(img.row_pitch) * img.height];
				}

				img.width = frame->width;
				img.height = frame->height;
				img.pixel_pitch = 4;

				uint8_t *dst_data[4] {img.data, nullptr, nullptr, nullptr};
				int dst_linesize[4] {img.row_pitch, 0, 0, 0};

				if (sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize) <= 0) {
					BOOST_LOG(error) << "PiCamera: sws_scale failed";
					return capture_e::error;
				}

				img.frame_timestamp = std::chrono::steady_clock::now();
				return capture_e::ok;
			}

			std::string device;
			format_context_t format;
			codec_context_t codec;
			scale_context_t scaler;
			std::chrono::nanoseconds delay {std::chrono::nanoseconds {1s} / 30};
		};
	}  // namespace

	bool initialize() {
		return std::filesystem::exists(DEFAULT_DEVICE.data());
	}

	std::vector<std::string> display_names() {
		std::vector<std::string> devices;
		for (int idx = 0; idx < 8; ++idx) {
			auto path = "/dev/video" + std::to_string(idx);
			if (std::filesystem::exists(path)) {
				devices.emplace_back(path);
			}
		}
		if (devices.empty()) {
			devices.emplace_back(std::string {DEFAULT_DEVICE});
		}
		return devices;
	}

	std::shared_ptr<platf::display_t> create_display(const std::string &device, const video::config_t &config) {
		auto resolved = device.empty() ? std::string {DEFAULT_DEVICE} : device;
		auto display = std::make_shared<picamera_display_t>(resolved);
		if (!display->init(config)) {
			BOOST_LOG(error) << "PiCamera: failed to initialise capture";
			return nullptr;
		}
		return display;
	}

}  // namespace platf::picamera

