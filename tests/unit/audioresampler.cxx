/* Copyright 2026 Quest VNC contributors
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <math.h>

#include <vector>

#include <gtest/gtest.h>

#include <rfb/AudioResampler.h>
#include <rfb/qemuTypes.h>

using rfb::AudioResampler;

TEST(AudioResampler, passthrough)
{
  AudioResampler r;
  std::vector<int16_t> out;
  int16_t in[] = { 1, -1, 1000, -1000, 32767, -32768 };

  r.setFormat(48000, 2, 48000, 2);
  r.process(in, 3, &out);
  ASSERT_EQ(out.size(), 6u);
  for (int i = 0; i < 6; i++)
    EXPECT_EQ(out[i], in[i]);
}

TEST(AudioResampler, channels)
{
  AudioResampler r;
  std::vector<int16_t> out;
  int16_t stereo[] = { 100, 300, -100, -300 };
  int16_t mono[] = { 7, -7 };

  r.setFormat(48000, 2, 48000, 1);
  r.process(stereo, 2, &out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], 200);
  EXPECT_EQ(out[1], -200);

  out.clear();
  r.setFormat(48000, 1, 48000, 2);
  r.process(mono, 2, &out);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0], 7);
  EXPECT_EQ(out[1], 7);
  EXPECT_EQ(out[2], -7);
  EXPECT_EQ(out[3], -7);
}

// The output length tracks the rate ratio over many small chunks
static void checkRate(int inRate, int outRate)
{
  AudioResampler r;
  std::vector<int16_t> in(inRate / 100), out;

  r.setFormat(inRate, 1, outRate, 1);
  for (int chunk = 0; chunk < 100; chunk++) {
    for (size_t i = 0; i < in.size(); i++)
      in[i] = (int16_t)(10000 * sin(2 * M_PI * 440 * (chunk * in.size() + i) / inRate));
    r.process(in.data(), in.size(), &out);
  }

  EXPECT_NEAR((double)out.size(), (double)outRate, 2.0);

  // And the signal is still a smooth sine at the new rate (the first
  // input frame is repeated, so skip the start)
  double maxErr = 0;
  for (size_t i = 4; i < out.size(); i++) {
    double expected = 10000 * sin(2 * M_PI * 440 * ((double)i * inRate / outRate - 1) / inRate);
    maxErr = std::max(maxErr, fabs(out[i] - expected));
  }
  EXPECT_LT(maxErr, 400.0);
}

TEST(AudioResampler, rates)
{
  checkRate(48000, 44100);
  checkRate(48000, 22050);
  checkRate(48000, 96000);
  checkRate(16000, 48000);
  checkRate(44100, 48000);
}

TEST(AudioResampler, formats)
{
  int16_t in[] = { 0, 32767, -32768, 256 };
  std::vector<uint8_t> out;

  AudioResampler::convertSamples(in, 4, rfb::qemuAudioFormatS16, &out);
  std::vector<uint8_t> s16 = { 0x00, 0x00, 0xff, 0x7f, 0x00, 0x80, 0x00, 0x01 };
  EXPECT_EQ(out, s16);

  out.clear();
  AudioResampler::convertSamples(in, 4, rfb::qemuAudioFormatU8, &out);
  std::vector<uint8_t> u8 = { 0x80, 0xff, 0x00, 0x81 };
  EXPECT_EQ(out, u8);

  out.clear();
  AudioResampler::convertSamples(in, 4, rfb::qemuAudioFormatS8, &out);
  std::vector<uint8_t> s8 = { 0x00, 0x7f, 0x80, 0x01 };
  EXPECT_EQ(out, s8);

  out.clear();
  AudioResampler::convertSamples(in, 1, rfb::qemuAudioFormatU16, &out);
  std::vector<uint8_t> u16 = { 0x00, 0x80 };
  EXPECT_EQ(out, u16);

  out.clear();
  AudioResampler::convertSamples(in + 1, 1, rfb::qemuAudioFormatS32, &out);
  std::vector<uint8_t> s32 = { 0x00, 0x00, 0xff, 0x7f };
  EXPECT_EQ(out, s32);

  EXPECT_EQ(AudioResampler::bytesPerSample(99), 0);
}
