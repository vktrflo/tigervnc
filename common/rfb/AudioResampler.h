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

// AudioResampler converts a stream of interleaved signed 16-bit PCM
// between sample rates (linear interpolation) and between mono and
// stereo. It keeps state between calls so that chunk boundaries are
// seamless.

#ifndef __RFB_AUDIORESAMPLER_H__
#define __RFB_AUDIORESAMPLER_H__

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace rfb {

  class AudioResampler {
  public:
    AudioResampler();

    // Channels must be 1 or 2, rates between 1 and 384000. Resets the
    // stream state.
    void setFormat(int inRate, int inChannels,
                   int outRate, int outChannels);

    void reset();

    // Appends the converted frames to out.
    void process(const int16_t* in, size_t frames,
                 std::vector<int16_t>* out);

    int inputRate() const { return inRate; }
    int inputChannels() const { return inChannels; }
    int outputRate() const { return outRate; }
    int outputChannels() const { return outChannels; }

    // Converts S16 samples to one of the QEMU audio sample formats
    // (qemuAudioFormat*), little-endian, appending to out.
    static void convertSamples(const int16_t* in, size_t samples,
                               int format, std::vector<uint8_t>* out);
    static int bytesPerSample(int format);

  private:
    void frameAt(const int16_t* in, long index, int16_t* frame) const;

    int inRate, inChannels, outRate, outChannels;
    // Position of the next output frame, in input frames, relative to
    // the first frame of the next input chunk. -1 is the last frame of
    // the previous chunk.
    double pos;
    bool havePrev;
    int16_t prev[2];
  };

}

#endif
