/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <chrono>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

#ifdef _WIN32
  // The virtual microphone is driven through WASAPI, see mic_passthrough()
  #include <objbase.h>
#endif

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using opus_decoder_t = util::safe_ptr<OpusDecoder, opus_decoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // Microphone audio is always sent as 10 ms frames of mono 48 kHz Opus
  constexpr auto MIC_SAMPLE_RATE = 48000;
  constexpr auto MIC_CHANNELS = 1;
  constexpr auto MIC_FRAME_DURATION = 10ms;
  constexpr auto MIC_FRAME_SIZE = MIC_SAMPLE_RATE / 100;

  // The largest frame Opus can produce at 48 kHz is 120 ms long
  constexpr auto MIC_MAX_FRAME_SIZE = 5760;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream || config.input_only) {
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
      return;
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
      }
    }

    BOOST_LOG(info) << "Selected audio sink: "sv << *sink;

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread {encodeThread, samples, config, channel_data};

    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          if (config::audio.auto_capture) {
            BOOST_LOG(info) << "Reinitializing audio capture"sv;
            mic.reset();
            do {
              mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
              if (!mic) {
                BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
              }
            } while (!mic && !shutdown_event->view(5s));
          }

          continue;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
  }

  void mic_jitter_buffer_t::push(std::uint32_t sequence_number, std::vector<std::uint8_t> data) {
    // A frame whose turn has already passed is of no use to us anymore
    if (_primed && (std::int32_t) (sequence_number - _next_sequence_number) < 0) {
      return;
    }

    _frames.insert_or_assign(sequence_number, std::move(data));

    // The client is producing frames faster than we play them back. Drop the oldest ones
    // rather than letting the latency grow, and resume playback from what's left.
    if (_frames.size() > max_frames) {
      do {
        _frames.erase(std::begin(_frames));
      } while (_frames.size() > max_frames);

      _next_sequence_number = std::begin(_frames)->first;
    }
  }

  mic_jitter_buffer_t::action_e mic_jitter_buffer_t::pop(std::vector<std::uint8_t> &data) {
    if (!_primed) {
      if (_frames.size() < prefill_frames) {
        return action_e::underrun;
      }

      _primed = true;
      _next_sequence_number = std::begin(_frames)->first;
    }

    // Concealment only makes sense for the odd lost frame. Once the client has run
    // further ahead than the buffer is deep, skip to what we actually have.
    if (!_frames.empty() && (std::int32_t) (std::begin(_frames)->first - _next_sequence_number) > (std::int32_t) max_frames) {
      _next_sequence_number = std::begin(_frames)->first;
    }

    auto frame = _frames.find(_next_sequence_number);
    if (frame != std::end(_frames)) {
      data = std::move(frame->second);
      _frames.erase(frame);

      ++_next_sequence_number;
      return action_e::play;
    }

    // Enough later frames have arrived to consider this one lost for good
    if (_frames.size() >= conceal_frames) {
      ++_next_sequence_number;
      return action_e::conceal;
    }

    _primed = false;
    return action_e::underrun;
  }

  void mic_jitter_buffer_t::reset() {
    _frames.clear();
    _primed = false;
  }

  void mic_passthrough(safe::mail_t mail) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto packets = mail->queue<mic_packet_t>(mail::mic_packets);

    // Once we're gone there is nobody left to drain the queue
    auto packets_fg = util::fail_guard([&packets]() {
      packets->stop();
    });

#ifdef _WIN32
    // The virtual microphone is a WASAPI endpoint, so this thread has to join an apartment
    // before it may touch the sink or the device enumerator it was created from.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    auto co_uninit_fg = util::fail_guard([]() {
      CoUninitialize();
    });
#endif

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    // Reuse the control interface of the audio context. Creating another one is not free of
    // side effects: on Windows it installs the Steam audio drivers.
    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Only the first session to get here may write to the virtual microphone. Mixing
    // several clients into a single capture device is not something we can do sensibly.
    if (ref->mic_flag->exchange(true, std::memory_order_acquire)) {
      BOOST_LOG(info) << "Another session is already using the virtual microphone"sv;
      return;
    }

    auto mic_flag_fg = util::fail_guard([&ref]() {
      ref->mic_flag->store(false, std::memory_order_release);
    });

    auto sink = control->mic_sink(MIC_SAMPLE_RATE, MIC_CHANNELS);
    if (!sink) {
      BOOST_LOG(error) << "Unable to open the virtual microphone. The client's microphone will not be audible."sv;
      return;
    }

    int err;
    opus_decoder_t opus {opus_decoder_create(MIC_SAMPLE_RATE, MIC_CHANNELS, &err)};
    if (!opus) {
      BOOST_LOG(error) << "Couldn't create the microphone decoder: "sv << opus_strerror(err);
      return;
    }

    BOOST_LOG(info) << "Microphone passthrough initialized: "sv << MIC_SAMPLE_RATE / 1000 << " kHz, "sv
                    << MIC_CHANNELS << " channel, "sv << MIC_FRAME_DURATION.count() << " ms frames"sv;

    // Playback takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    mic_jitter_buffer_t jitter;
    std::vector<std::uint8_t> frame;
    std::vector<float> samples(MIC_MAX_FRAME_SIZE * MIC_CHANNELS);

    auto next_frame = std::chrono::steady_clock::now();
    while (!shutdown_event->peek()) {
      // Take in everything that arrived while we were waiting for this frame to come due
      while (packets->peek()) {
        auto packet = packets->pop();
        if (!packet) {
          break;
        }

        jitter.push(packet->sequenceNumber, std::move(packet->data));
      }

      int decoded = 0;
      switch (jitter.pop(frame)) {
        case mic_jitter_buffer_t::action_e::play:
          decoded = opus_decode_float(opus.get(), frame.data(), (opus_int32) frame.size(), samples.data(), MIC_MAX_FRAME_SIZE, 0);
          break;
        case mic_jitter_buffer_t::action_e::conceal:
          // The frame never made it, so let Opus interpolate over the gap
          decoded = opus_decode_float(opus.get(), nullptr, 0, samples.data(), MIC_FRAME_SIZE, 0);
          break;
        case mic_jitter_buffer_t::action_e::underrun:
          // There's nothing to play back yet, the buffer is still filling up
          break;
      }

      if (decoded < 0) {
        BOOST_LOG(error) << "Couldn't decode microphone audio: "sv << opus_strerror(decoded);
        decoded = 0;
      }

      // Keeping the endpoint fed with silence is cheaper than letting it starve
      auto written = decoded > 0 ? sink->write(samples.data(), decoded) : sink->write(nullptr, MIC_FRAME_SIZE);
      if (written < 0) {
        BOOST_LOG(warning) << "Reinitializing the virtual microphone"sv;

        sink.reset();
        jitter.reset();

        do {
          sink = control->mic_sink(MIC_SAMPLE_RATE, MIC_CHANNELS);
          if (!sink) {
            BOOST_LOG(warning) << "Couldn't re-initialize the virtual microphone"sv;
          }
        } while (!sink && !shutdown_event->view(5s));

        if (!sink) {
          return;
        }

        next_frame = std::chrono::steady_clock::now();
      }

      // Playback is paced by our own clock. Whatever drift there is against the client's
      // capture clock shows up as the depth of the jitter buffer, which drops frames when
      // it grows too deep and inserts silence when it runs dry.
      next_frame += MIC_FRAME_DURATION;

      auto now = std::chrono::steady_clock::now();
      if (next_frame < now) {
        // We fell behind, don't try to make up for it in one burst
        next_frame = now;
      }

      std::this_thread::sleep_until(next_frame);
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);
    ctx.mic_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping;
  }
}  // namespace audio
