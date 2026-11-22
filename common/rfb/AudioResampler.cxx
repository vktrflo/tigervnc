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

#include <stdexcept>

#include <rfb/AudioResampler.h>
#include <rfb/qemuTypes.h>

using namespace rfb;

AudioResampler::AudioResampler()
  : inRate(48000), inChannels(2), outRate(48000), outChannels(2)
{
  reset();
}

void AudioResampler::setFormat(int inRate_, int inChannels_,
                               int outRate_, int outChannels_)
{
  if (inChannels_ < 1 || inChannels_ > 2 ||
      outChannels_ < 1 || outChannels_ > 2)
    throw std::invalid_argument("Unsupported number of audio channels");
  if (inRate_ < 1 || inRate_ > 384000 || outRate_ < 1 || outRate_ > 384000)
    throw std::invalid_argument("Unsupported audio sample rate");

  inRate = inRate_;
  inChannels = inChannels_;
  outRate = outRate_;
  outChannels = outChannels_;
  reset();
}

void AudioResampler::reset()
{
  pos = 0;
  havePrev = false;
  prev[0] = prev[1] = 0;
}

// Fetches input frame 'index' (-1 meaning the saved previous frame)
// converted to the output channel layout.
void AudioResampler::frameAt(const int16_t* in, long index,
                             int16_t* frame) const
{
  const int16_t* src;
  int16_t tmp[2];

  if (index < 0) {
    tmp[0] = prev[0];
    tmp[1] = prev[1];
    src = tmp;
  } else {
    src = in + index * inChannels;
  }

  if (inChannels == outChannels) {
    frame[0] = src[0];
    if (outChannels == 2)
      frame[1] = src[1];
  } else if (inChannels == 2) {
    frame[0] = (int16_t)(((int)src[0] + (int)src[1]) / 2);
  } else {
    frame[0] = frame[1] = src[0];
  }
}

void AudioResampler::process(const int16_t* in, size_t frames,
                             std::vector<int16_t>* out)
{
  if (frames == 0)
    return;

  if (!havePrev) {
    // Start the stream on the first frame
    prev[0] = in[0];
    prev[1] = inChannels == 2 ? in[1] : in[0];
    havePrev = true;
    pos = -1;
  }

  if (inRate == outRate) {
    // Fast path, only channel conversion
    int16_t frame[2];
    out->reserve(out->size() + frames * outChannels);
    for (size_t i = 0; i < frames; i++) {
      frameAt(in, (long)i, frame);
      out->push_back(frame[0]);
      if (outChannels == 2)
        out->push_back(frame[1]);
    }
  } else {
    double step = (double)inRate / outRate;
    int16_t a[2], b[2];

    out->reserve(out->size() +
                 (size_t)(frames / step + 2) * outChannels);

    while (true) {
      long i = (long)floor(pos);
      if (i + 1 >= (long)frames)
        break;
      double frac = pos - i;
      frameAt(in, i, a);
      frameAt(in, i + 1, b);
      for (int c = 0; c < outChannels; c++) {
        double v = a[c] + (b[c] - a[c]) * frac;
        out->push_back((int16_t)lrint(v));
      }
      pos += step;
    }

    pos -= frames;
  }

  // Remember the last input frame, in the input layout
  const int16_t* last = in + (frames - 1) * inChannels;
  prev[0] = last[0];
  prev[1] = inChannels == 2 ? last[1] : last[0];
  if (inRate == outRate)
    pos = 0;
}

int AudioResampler::bytesPerSample(int format)
{
  switch (format) {
  case qemuAudioFormatU8:
  case qemuAudioFormatS8:
    return 1;
  case qemuAudioFormatU16:
  case qemuAudioFormatS16:
    return 2;
  case qemuAudioFormatU32:
  case qemuAudioFormatS32:
    return 4;
  }
  return 0;
}

void AudioResampler::convertSamples(const int16_t* in, size_t samples,
                                    int format, std::vector<uint8_t>* out)
{
  size_t base = out->size();
  int bps = bytesPerSample(format);

  if (bps == 0)
    throw std::invalid_argument("Unsupported audio sample format");

  out->resize(base + samples * bps);
  uint8_t* p = out->data() + base;

  for (size_t i = 0; i < samples; i++) {
    int s = in[i];
    uint32_t v;
    switch (format) {
    case qemuAudioFormatU8:
      *p++ = (uint8_t)((s >> 8) + 128);
      break;
    case qemuAudioFormatS8:
      *p++ = (uint8_t)(int8_t)(s >> 8);
      break;
    case qemuAudioFormatU16:
      v = (uint32_t)(s + 32768);
      *p++ = v & 0xff;
      *p++ = (v >> 8) & 0xff;
      break;
    case qemuAudioFormatS16:
      v = (uint16_t)s;
      *p++ = v & 0xff;
      *p++ = (v >> 8) & 0xff;
      break;
    case qemuAudioFormatU32:
      v = (uint32_t)(s + 32768) << 16;
      *p++ = v & 0xff;
      *p++ = (v >> 8) & 0xff;
      *p++ = (v >> 16) & 0xff;
      *p++ = (v >> 24) & 0xff;
      break;
    case qemuAudioFormatS32:
      v = (uint32_t)s << 16;
      *p++ = v & 0xff;
      *p++ = (v >> 8) & 0xff;
      *p++ = (v >> 16) & 0xff;
      *p++ = (v >> 24) & 0xff;
      break;
    }
  }
}
