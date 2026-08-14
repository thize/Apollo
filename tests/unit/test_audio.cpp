/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */
#include "../tests_common.h"

#include <src/audio.h>

using namespace audio;

struct AudioTest: PlatformTestSuite, testing::WithParamInterface<std::tuple<std::basic_string_view<char>, config_t>> {
  void SetUp() override {
    m_config = std::get<1>(GetParam());
    m_mail = std::make_shared<safe::mail_raw_t>();
  }

  config_t m_config;
  safe::mail_t m_mail;
};

constexpr std::bitset<config_t::MAX_FLAGS> config_flags(const int flag = -1) {
  std::bitset<3> result = std::bitset<config_t::MAX_FLAGS>();
  if (flag >= 0) {
    result.set(flag);
  }
  return result;
}

INSTANTIATE_TEST_SUITE_P(
  Configurations,
  AudioTest,
  testing::Values(
    std::make_tuple("HIGH_STEREO", config_t {5, 2, 0x3, {0}, config_flags(config_t::HIGH_QUALITY)}),
    std::make_tuple("SURROUND51", config_t {5, 6, 0x3F, {0}, config_flags()}),
    std::make_tuple("SURROUND71", config_t {5, 8, 0x63F, {0}, config_flags()}),
    std::make_tuple("SURROUND51_CUSTOM", config_t {5, 6, 0x3F, {6, 4, 2, {0, 1, 4, 5, 2, 3}}, config_flags(config_t::CUSTOM_SURROUND_PARAMS)})
  ),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

struct MicJitterBufferTest: testing::Test {
  /**
   * @brief Build a frame whose payload identifies the sequence number it belongs to.
   */
  static std::vector<std::uint8_t> frame_of(std::uint32_t sequence_number) {
    return {(std::uint8_t) sequence_number};
  }

  /**
   * @brief Fill the buffer up to the point where it starts playing back.
   */
  void prefill(std::uint32_t first_sequence_number = 0) {
    for (std::uint32_t i = 0; i < mic_jitter_buffer_t::prefill_frames; ++i) {
      m_buffer.push(first_sequence_number + i, frame_of(first_sequence_number + i));
    }
  }

  mic_jitter_buffer_t m_buffer;
  std::vector<std::uint8_t> m_frame;
};

TEST_F(MicJitterBufferTest, UnderrunsUntilPrefilled) {
  for (std::uint32_t i = 0; i < mic_jitter_buffer_t::prefill_frames; ++i) {
    ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::underrun);
    m_buffer.push(i, frame_of(i));
  }

  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  ASSERT_EQ(m_frame, frame_of(0));
}

TEST_F(MicJitterBufferTest, ReordersFrames) {
  m_buffer.push(2, frame_of(2));
  m_buffer.push(0, frame_of(0));
  m_buffer.push(1, frame_of(1));

  for (std::uint32_t i = 0; i < 3; ++i) {
    ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
    ASSERT_EQ(m_frame, frame_of(i));
  }
}

TEST_F(MicJitterBufferTest, ConcealsLostFrame) {
  m_buffer.push(0, frame_of(0));
  m_buffer.push(2, frame_of(2));
  m_buffer.push(3, frame_of(3));

  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  ASSERT_EQ(m_frame, frame_of(0));

  // Sequence number 1 never arrived, but the ones after it did
  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::conceal);

  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  ASSERT_EQ(m_frame, frame_of(2));
}

TEST_F(MicJitterBufferTest, DropsOldestFramesWhenOverfilled) {
  constexpr std::uint32_t overfill = 2;

  for (std::uint32_t i = 0; i < mic_jitter_buffer_t::max_frames + overfill; ++i) {
    m_buffer.push(i, frame_of(i));
  }

  ASSERT_EQ(m_buffer.depth(), mic_jitter_buffer_t::max_frames);

  // The frames that were dropped to bound the latency are not concealed
  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  ASSERT_EQ(m_frame, frame_of(overfill));
}

TEST_F(MicJitterBufferTest, RefillsAfterRunningDry) {
  prefill();

  for (std::uint32_t i = 0; i < mic_jitter_buffer_t::prefill_frames; ++i) {
    ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  }

  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::underrun);

  // Playback only resumes once the buffer has been filled up again
  prefill(mic_jitter_buffer_t::prefill_frames);
  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  ASSERT_EQ(m_frame, frame_of(mic_jitter_buffer_t::prefill_frames));
}

TEST_F(MicJitterBufferTest, IgnoresFramesThatArriveTooLate) {
  prefill();

  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);

  auto depth = m_buffer.depth();
  m_buffer.push(0, frame_of(0));
  ASSERT_EQ(m_buffer.depth(), depth);
}

TEST_F(MicJitterBufferTest, ResyncsInsteadOfConcealingABurst) {
  constexpr std::uint32_t resumed_at = 100;

  prefill();
  for (std::uint32_t i = 0; i < mic_jitter_buffer_t::prefill_frames; ++i) {
    ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  }

  // Nothing arrived for a while, so the gap is far too wide to conceal
  m_buffer.push(resumed_at, frame_of(resumed_at));

  ASSERT_EQ(m_buffer.pop(m_frame), mic_jitter_buffer_t::action_e::play);
  ASSERT_EQ(m_frame, frame_of(resumed_at));
}

TEST_P(AudioTest, TestEncode) {
  std::thread timer([&] {
    // Terminate the audio capture after 100 ms
    std::this_thread::sleep_for(100ms);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    const auto audio_packets = m_mail->queue<packet_t>(mail::audio_packets);
    shutdown_event->raise(true);
    audio_packets->stop();
  });
  std::thread capture([&] {
    const auto packets = m_mail->queue<packet_t>(mail::audio_packets);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    while (const auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }
      if (auto packet_data = packet->second; packet_data.size() == 0) {
        FAIL() << "Empty packet data";
      }
    }
  });
  audio::capture(m_mail, m_config, nullptr);

  timer.join();
  capture.join();
}
