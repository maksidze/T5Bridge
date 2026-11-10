#include "audiodecode.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

namespace TES5Viewer
{
    namespace
    {
        constexpr std::size_t maxAudioBytes = 64 * 1024 * 1024;

        void check(int result, const char* message)
        {
            if (result >= 0)
                return;
            std::array<char, AV_ERROR_MAX_STRING_SIZE> detail{};
            av_strerror(result, detail.data(), detail.size());
            throw std::runtime_error(std::string(message) + ": " + detail.data());
        }

        bool tag(std::span<const std::byte> bytes, std::size_t offset, const char* expected)
        {
            return offset <= bytes.size() && bytes.size() - offset >= 4
                && std::memcmp(bytes.data() + offset, expected, 4) == 0;
        }

        std::uint32_t le32(std::span<const std::byte> bytes, std::size_t offset)
        {
            if (offset > bytes.size() || bytes.size() - offset < 4)
                throw std::runtime_error("Truncated game audio header");
            std::uint32_t result = 0;
            for (unsigned i = 0; i < 4; ++i)
                result |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8);
            return result;
        }

        struct MemoryInput
        {
            std::span<const std::byte> bytes;
            std::size_t position = 0;

            static int read(void* opaque, std::uint8_t* output, int requested)
            {
                auto& self = *static_cast<MemoryInput*>(opaque);
                if (requested <= 0 || self.position == self.bytes.size())
                    return AVERROR_EOF;
                const auto count = std::min(static_cast<std::size_t>(requested), self.bytes.size() - self.position);
                std::memcpy(output, self.bytes.data() + self.position, count);
                self.position += count;
                return static_cast<int>(count);
            }

            static std::int64_t seek(void* opaque, std::int64_t offset, int origin)
            {
                auto& self = *static_cast<MemoryInput*>(opaque);
                const auto size = static_cast<std::int64_t>(self.bytes.size());
                origin &= ~AVSEEK_FORCE;
                if (origin == AVSEEK_SIZE)
                    return size;
                if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)
                    return AVERROR(EINVAL);
                const auto base = origin == SEEK_SET ? 0 : origin == SEEK_END ? size : static_cast<std::int64_t>(self.position);
                if (offset < -base || offset > size - base)
                    return AVERROR(EINVAL);
                self.position = static_cast<std::size_t>(base + offset);
                return static_cast<std::int64_t>(self.position);
            }
        };

        std::optional<AudioLoop> waveLoop(std::span<const std::byte> bytes)
        {
            for (std::size_t position = 12; position <= bytes.size() && bytes.size() - position >= 8;)
            {
                const auto size = le32(bytes, position + 4);
                if (size > bytes.size() - position - 8)
                    break;
                if (tag(bytes, position, "smpl") && size >= 60)
                {
                    const auto chunk = bytes.subspan(position + 8, size);
                    if (le32(chunk, 28) && le32(chunk, 40) == 0)
                    {
                        const auto first = le32(chunk, 44);
                        const auto last = le32(chunk, 48);
                        if (first <= last && last != UINT32_MAX)
                            return AudioLoop{first, last + 1};
                    }
                }
                position += 8 + static_cast<std::size_t>(size) + (size & 1);
            }
            return {};
        }

        struct AudioIo
        {
            AVIOContext* value;
            ~AudioIo()
            {
                av_freep(&value->buffer);
                avio_context_free(&value);
            }
        };

        struct AudioFormat
        {
            AVFormatContext* value;
            ~AudioFormat() { avformat_close_input(&value); }
        };

        template <class T, void (*Release)(T**)>
        struct AudioDeleter
        {
            void operator()(T* value) const { Release(&value); }
        };
    }

    DecodedAudio decodeAudio(std::span<const std::byte> data)
    {
        if (data.empty() || data.size() > maxAudioBytes)
            throw std::runtime_error("Invalid game audio size");
        if (tag(data, 0, "FUZE"))
        {
            if (le32(data, 4) != 1)
                throw std::runtime_error("Unsupported FUZ version");
            const auto lipSize = le32(data, 8);
            if (lipSize > data.size() - 12)
                throw std::runtime_error("Truncated FUZ lip section");
            data = data.subspan(12 + lipSize);
        }
        const char* format = nullptr;
        if (tag(data, 0, "RIFF") && tag(data, 8, "XWMA"))
            format = "xwma";
        else if (tag(data, 0, "RIFF") && tag(data, 8, "WAVE"))
            format = "wav";
        else if (tag(data, 0, "OggS"))
            format = "ogg";
        const auto* demuxer = format ? av_find_input_format(format) : nullptr;
        if (!demuxer)
            throw std::runtime_error("Unsupported game audio container");

        MemoryInput input{ data };
        constexpr int ioBufferSize = 4096;
        auto* ioBuffer = static_cast<std::uint8_t*>(av_malloc(ioBufferSize));
        if (!ioBuffer)
            throw std::bad_alloc();
        auto* ioContext = avio_alloc_context(ioBuffer, ioBufferSize, 0, &input, MemoryInput::read, nullptr, MemoryInput::seek);
        if (!ioContext)
        {
            av_free(ioBuffer);
            throw std::bad_alloc();
        }
        const AudioIo io{ ioContext };
        AudioFormat source{ avformat_alloc_context() };
        if (!source.value)
            throw std::bad_alloc();
        source.value->pb = io.value;
        source.value->flags |= AVFMT_FLAG_CUSTOM_IO;
        check(avformat_open_input(&source.value, nullptr,
#if LIBAVFORMAT_VERSION_MAJOR >= 59
                  demuxer,
#else
                  const_cast<AVInputFormat*>(demuxer),
#endif
                  nullptr),
            "Cannot open game audio");
        check(avformat_find_stream_info(source.value, nullptr), "Cannot read game audio stream");
        const int stream = av_find_best_stream(source.value, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        check(stream, "No audio stream");
        const auto* parameters = source.value->streams[stream]->codecpar;
        const auto* decoder = avcodec_find_decoder(parameters->codec_id);
        if (!decoder)
            throw std::runtime_error("No decoder for game audio codec");
        std::unique_ptr<AVCodecContext, AudioDeleter<AVCodecContext, avcodec_free_context>> codec(avcodec_alloc_context3(decoder));
        std::unique_ptr<AVPacket, AudioDeleter<AVPacket, av_packet_free>> packet(av_packet_alloc());
        std::unique_ptr<AVFrame, AudioDeleter<AVFrame, av_frame_free>> frame(av_frame_alloc());
        if (!codec || !packet || !frame)
            throw std::bad_alloc();
        check(avcodec_parameters_to_context(codec.get(), parameters), "Cannot copy game audio parameters");
        check(avcodec_open2(codec.get(), decoder, nullptr), "Cannot initialize game audio decoder");
#if LIBAVUTIL_VERSION_MAJOR >= 57
        const int sourceChannels = codec->ch_layout.nb_channels;
#else
        const int sourceChannels = codec->channels;
#endif
        if (sourceChannels <= 0 || sourceChannels > 8 || codec->sample_rate < 8000 || codec->sample_rate > 192000)
            throw std::runtime_error("Invalid game audio sample rate or channel count");
        DecodedAudio result{ codec->sample_rate, std::min(sourceChannels, 2), {},
            tag(data, 8, "WAVE") ? waveLoop(data) : std::nullopt };
        SwrContext* resampler = nullptr;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        AVChannelLayout output{};
        av_channel_layout_default(&output, result.channels);
        if (codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&codec->ch_layout, sourceChannels);
        const int configured = swr_alloc_set_opts2(&resampler, &output, AV_SAMPLE_FMT_S16, result.sampleRate,
            &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&output);
#else
        resampler = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(result.channels), AV_SAMPLE_FMT_S16,
            result.sampleRate, codec->channel_layout ? codec->channel_layout : av_get_default_channel_layout(sourceChannels),
            codec->sample_fmt, codec->sample_rate, 0, nullptr);
        const int configured = resampler ? 0 : AVERROR(ENOMEM);
#endif
        const std::unique_ptr<SwrContext, AudioDeleter<SwrContext, swr_free>> resample(resampler);
        check(configured, "Cannot configure PCM conversion");
        check(swr_init(resample.get()), "Cannot initialize PCM conversion");

        const auto frameBytes = static_cast<std::size_t>(result.channels) * sizeof(std::int16_t);
        const auto convert = [&](const std::uint8_t** samples, int count) {
            const int capacity = swr_get_out_samples(resample.get(), count);
            check(capacity, "Cannot bound converted audio samples");
            if (static_cast<std::size_t>(capacity) > (maxAudioBytes - result.pcm.size()) / frameBytes)
                throw std::runtime_error("Decoded game audio exceeds the PCM size limit");
            if (!capacity)
                return 0;
            const auto start = result.pcm.size();
            result.pcm.resize(start + capacity * frameBytes);
            auto* destination = reinterpret_cast<std::uint8_t*>(result.pcm.data() + start);
            const int written = swr_convert(resample.get(), &destination, capacity, samples, count);
            check(written, "Cannot convert game audio to PCM");
            result.pcm.resize(start + written * frameBytes);
            return written;
        };
        const auto receive = [&] {
            for (;;)
            {
                const int status = avcodec_receive_frame(codec.get(), frame.get());
                if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                    return;
                check(status, "Cannot decode game audio frame");
                convert(const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
                av_frame_unref(frame.get());
            }
        };
        int status;
        while ((status = av_read_frame(source.value, packet.get())) >= 0)
        {
            if (packet->stream_index == stream)
            {
                check(avcodec_send_packet(codec.get(), packet.get()), "Cannot submit game audio packet");
                receive();
            }
            av_packet_unref(packet.get());
        }
        if (status != AVERROR_EOF)
            check(status, "Cannot read game audio packet");
        check(avcodec_send_packet(codec.get(), nullptr), "Cannot flush game audio decoder");
        receive();
        while (convert(nullptr, 0) > 0)
        {
        }
        if (result.pcm.empty())
            throw std::runtime_error("Decoded game audio contains no samples");
        if (result.loop && result.loop->end > result.pcm.size() / frameBytes)
            result.loop.reset();
        return result;
    }
}
