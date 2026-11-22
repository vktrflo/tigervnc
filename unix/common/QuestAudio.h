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

// Sound and headset microphone support for the Unix servers.
//
// AudioCapture records what the desktop is playing (the default
// PulseAudio/PipeWire output's monitor, via parec or pw-record) and
// hands it to the VNC server, which sends it to clients using the QEMU
// audio extension.
//
// MicSink plays headset microphone audio from a client (Quest VNC
// extension) into a virtual microphone, a PulseAudio pipe source that
// is created while clients are connected.
//
// Both run from timers on the server's main loop, so no locking is
// needed.

#ifndef __UNIX_QUESTAUDIO_H__
#define __UNIX_QUESTAUDIO_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

#include <string>
#include <vector>

#include <core/Timer.h>

#include <rfb/AudioResampler.h>

namespace rfb { class VNCServer; }

class AudioCapture : public core::Timer::Callback {
public:
  AudioCapture();
  ~AudioCapture();

  // start() begins capturing for the given server, according to the
  // AudioCapture parameter. stop() ends it.
  void start(rfb::VNCServer* server);
  void stop();

protected:
  void handleTimeout(core::Timer* t) override;

private:
  void spawnChild();
  void stopChild(bool kill);
  void readChild();
  void childWorks();
  void sendBuffered();
  void generateTone();

  rfb::VNCServer* server;
  core::Timer timer;

  bool tone;
  std::vector<std::vector<std::string>> commands;
  size_t command;
  int failures;

  pid_t child;
  int fd;
  bool childGotData;
  struct timeval spawnTime;
  struct timeval nextSpawn;

  std::vector<uint8_t> buffer;
  std::vector<int16_t> pcm;

  struct timeval toneStart;
  uint64_t toneFrames;
};

class MicSink {
public:
  MicSink();
  ~MicSink();

  // open() creates the virtual microphone, according to the
  // MicSource parameter. close() removes it.
  void open();
  void close();

  bool available() const { return fd >= 0; }

  void start(int channels, uint32_t sampleRate);
  void data(const uint8_t* data, size_t len);
  void stop();

private:
  int moduleIndex;
  std::string path;
  int fd;
  bool started;
  rfb::AudioResampler resampler;
  std::vector<int16_t> in, out;
  std::vector<uint8_t> pending;
  std::vector<uint8_t> bytes;
};

#endif
