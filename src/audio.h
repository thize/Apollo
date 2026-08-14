/**
 * @file src/audio.h
 * @brief Declarations for audio capture and encoding.
 */
#pragma once

// local includes
#include "config.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

#include <bitset>
#include <map>

namespace audio {
  enum stream_config_e : int {
    STEREO,  ///< Stereo
    HIGH_STEREO,  ///< High stereo
    SURROUND51,  ///< Surround 5.1
    HIGH_SURROUND51,  ///< High surround 5.1
    SURROUND71,  ///< Surround 7.1
    HIGH_SURROUND71,  ///< High surround 7.1
    MAX_STREAM_CONFIG  ///< Maximum audio stream configuration
  };

  struct opus_stream_config_t {
    std::int32_t sampleRate;
    int channelCount;
    int streams;
    int coupledStreams;
    const std::uint8_t *mapping;
    int bitrate;
  };

  struct stream_params_t {
    int channelCount;
    int streams;
    int coupledStreams;
    std::uint8_t mapping[8];
  };

  extern opus_stream_config_t stream_configs[MAX_STREAM_CONFIG];

  struct config_t {
    enum flags_e : int {
      HIGH_QUALITY,  ///< High quality audio
      HOST_AUDIO,  ///< Host audio
      CUSTOM_SURROUND_PARAMS,  ///< Custom surround parameters
      MAX_FLAGS  ///< Maximum number of flags
    };

    int packetDuration;
    int channels;
    int mask;

    stream_params_t customStreamParams;

    std::bitset<MAX_FLAGS> flags;

    // Who TF knows what Sunshine did
    // putting input_only at the end of flags will always be over written to true
    uint64_t __padding;

    bool input_only;
  };

  struct audio_ctx_t {
    // We want to change the sink for the first stream only
    std::unique_ptr<std::atomic_bool> sink_flag;

    // Likewise, only one stream at a time may feed the virtual microphone
    std::unique_ptr<std::atomic_bool> mic_flag;

    std::unique_ptr<platf::audio_control_t> control;

    bool restore_sink;
    platf::sink_t sink;
  };

  /**
   * @brief An Opus encoded microphone frame received from a client.
   */
  struct mic_packet_t {
    std::uint32_t sequenceNumber;
    std::vector<std::uint8_t> data;
  };

  /**
   * @brief Reorders and paces the microphone frames sent by a client.
   *
   * The frames travel unsequenced over the control stream, so they may arrive out of
   * order, duplicated or not at all. Holding a couple of them back gives the late ones
   * a chance to catch up, and lets the buffer depth absorb the drift between the
   * client's capture clock and our playback clock.
   */
  class mic_jitter_buffer_t {
  public:
    enum class action_e : int {
      underrun,  ///< The buffer ran dry, play silence until it has refilled
      conceal,  ///< The frame that is due was lost, let the decoder conceal it
      play  ///< The frame that is due is available
    };

    /// The number of frames buffered before playback starts.
    static constexpr std::size_t prefill_frames = 3;

    /// The number of queued frames past a missing one before it's declared lost.
    static constexpr std::size_t conceal_frames = 2;

    /// The number of frames the buffer may hold before the oldest ones are dropped.
    static constexpr std::size_t max_frames = 4;

    /**
     * @brief Queue a frame received from the client.
     * @param sequence_number The sequence number the client assigned to the frame.
     * @param data The Opus payload of the frame.
     */
    void push(std::uint32_t sequence_number, std::vector<std::uint8_t> data);

    /**
     * @brief Take the frame that is due for playback.
     * @param data Receives the Opus payload when `action_e::play` is returned.
     * @returns What the virtual microphone should be fed with.
     */
    action_e pop(std::vector<std::uint8_t> &data);

    /**
     * @brief Drop every queued frame and prefill the buffer again.
     */
    void reset();

    /**
     * @brief Get the number of frames currently held back.
     * @returns The depth of the buffer, in frames.
     */
    std::size_t depth() const {
      return _frames.size();
    }

  private:
    std::map<std::uint32_t, std::vector<std::uint8_t>> _frames;

    std::uint32_t _next_sequence_number {};
    bool _primed {false};
  };

  using buffer_t = util::buffer_t<std::uint8_t>;
  using packet_t = std::pair<void *, buffer_t>;
  using audio_ctx_ref_t = safe::shared_t<audio_ctx_t>::ptr_t;
  using mic_queue_t = safe::mail_raw_t::queue_t<mic_packet_t>;

  void capture(safe::mail_t mail, config_t config, void *channel_data);

  /**
   * @brief Play the microphone audio of a client back through the host's virtual microphone.
   * @param mail The mailbox carrying the `mail::mic_packets` queue and the shutdown event.
   * @note Only the first session to call this claims the virtual microphone. The others
   *       return immediately, since mixing several clients into one device is not supported.
   */
  void mic_passthrough(safe::mail_t mail);

  /**
   * @brief Get the reference to the audio context.
   * @returns A shared pointer reference to audio context.
   * @note Aside from the configuration purposes, it can be used to extend the
   *       audio sink lifetime to capture sink earlier and restore it later.
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * @examples_end
   */
  audio_ctx_ref_t get_audio_ctx_ref();

  /**
   * @brief Check if the audio sink held by audio context is available.
   * @returns True if available (and can probably be restored), false otherwise.
   * @note Useful for delaying the release of audio context shared pointer (which
   *       tries to restore original sink).
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * if (audio.get()) {
   *     return is_audio_ctx_sink_available(*audio.get());
   * }
   * return false;
   * @examples_end
   */
  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx);

  /**
   * @brief Check if microphone passthrough is enabled on this host.
   * @returns True if the host is configured to accept microphone audio from clients.
   */
  inline bool is_mic_enabled() {
    return config::audio.enable_mic;
  }

  /**
   * @brief Check if the virtual microphone driver is ready to play back microphone audio.
   * @returns True if a virtual microphone was found during platform initialization.
   */
  inline bool is_mic_driver_ready() {
    return platf::mic_driver_status.load() == platf::mic_driver_status_e::ok;
  }

  /**
   * @brief Check if microphone passthrough can actually be offered to a client.
   * @returns True if microphone passthrough is both enabled and backed by a working device.
   * @note This is the single condition behind the `MicrophoneCapable`/`MicrophoneDriverReady`
   *       fields of `serverinfo` and the `x-ss-general.micSupport` attribute of DESCRIBE.
   */
  inline bool is_mic_available() {
    return is_mic_enabled() && is_mic_driver_ready();
  }
}  // namespace audio
