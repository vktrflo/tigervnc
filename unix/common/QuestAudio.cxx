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

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <algorithm>

#include <core/Configuration.h>
#include <core/LogWriter.h>
#include <core/time.h>

#include <rfb/VNCServer.h>

#include <QuestAudio.h>

extern char** environ;

static core::LogWriter vlog("QuestAudio");

static core::EnumParameter audioCapture(
  "AudioCapture",
  "How to capture sound for clients that can play it: auto (parec, "
  "then pw-record), parec, pw-record, tone (a test beep) or none",
  {"auto", "parec", "pw-record", "tone", "none"}, "auto");

static core::EnumParameter micSource(
  "MicSource",
  "Where headset microphone audio from clients goes: auto (a "
  "PulseAudio source named QuestVNC-Mic, created while clients are "
  "connected) or none",
  {"auto", "none"}, "none");

static const int rate = rfb::VNCServer::audioRate;
static const int channels = rfb::VNCServer::audioChannels;
static const size_t frameBytes = channels * 2;

// Timer period, and the largest chunk handed to the server
static const int tickMs = 10;
static const size_t maxChunkFrames = rate / 50;   // 20 ms
// Audio older than this when we finally get to read it is dropped
static const size_t maxBufferedFrames = rate / 25; // 40 ms

static const int micRate = 48000;

// Starts argv with stdout on a new pipe. Returns the read end of the
// pipe, or -1.
static int spawnPipe(const std::vector<std::string>& args, pid_t* pid)
{
  int fds[2];
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attr;
  sigset_t mask;
  std::vector<char*> argv;
  int ret;

  if (pipe(fds) < 0)
    return -1;
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);

  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addclose(&actions, fds[1]);

  // The X server ignores and blocks some signals; give the child
  // ordinary ones so it dies when we go away
  posix_spawnattr_init(&attr);
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  sigemptyset(&mask);
  sigaddset(&mask, SIGPIPE);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGHUP);
  sigaddset(&mask, SIGCHLD);
  posix_spawnattr_setsigdefault(&attr, &mask);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK |
                                  POSIX_SPAWN_SETSIGDEF);

  for (const std::string& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  ret = posix_spawnp(pid, argv[0], &actions, &attr, argv.data(), environ);

  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  ::close(fds[1]);

  if (ret != 0) {
    ::close(fds[0]);
    errno = ret;
    return -1;
  }

  return fds[0];
}

// Runs a short command and returns its exit status (or -1) and output.
static int runCommand(const std::vector<std::string>& args,
                      std::string* output, int timeoutMs)
{
  pid_t pid;
  int fd, status;
  struct timeval start;

  fd = spawnPipe(args, &pid);
  if (fd < 0)
    return -1;

  gettimeofday(&start, nullptr);
  output->clear();
  while (true) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int left = timeoutMs - (int)core::msSince(&start);
    if (left <= 0) {
      vlog.error("%s timed out", args[0].c_str());
      kill(pid, SIGKILL);
      break;
    }
    if (poll(&pfd, 1, left) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (pfd.revents == 0)
      continue;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    output->append(buf, n);
  }
  ::close(fd);

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }
  if (!WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}

//
// AudioCapture
//

AudioCapture::AudioCapture()
  : server(nullptr), timer(this), tone(false), command(0), failures(0),
    child(-1), fd(-1), childGotData(false), toneFrames(0)
{
  nextSpawn.tv_sec = nextSpawn.tv_usec = 0;
}

AudioCapture::~AudioCapture()
{
  stop();
}

void AudioCapture::start(rfb::VNCServer* server_)
{
  stop();

  server = server_;
  commands.clear();
  command = 0;
  failures = 0;
  tone = false;

  if (audioCapture == "none")
    return;

  if (audioCapture == "tone") {
    vlog.info("Sending a test tone as sound");
    tone = true;
    gettimeofday(&toneStart, nullptr);
    toneFrames = 0;
    server->setAudioAvailable(true);
    timer.start(tickMs);
    return;
  }

  if (audioCapture == "auto" || audioCapture == "parec") {
    // Name the monitor explicitly. PulseAudio's module-switch-on-connect
    // moves recordings that follow the default source (which can be
    // this monitor on a headless system) to a newly added source, such
    // as our microphone, unless they asked for a specific device.
    std::string device = "@DEFAULT_MONITOR@";
    std::string sink;
    if (runCommand({"pactl", "get-default-sink"}, &sink, 3000) == 0) {
      while (!sink.empty() && isspace((unsigned char)sink.back()))
        sink.pop_back();
      if (!sink.empty() && sink.find_first_of(" \t\n") == std::string::npos)
        device = sink + ".monitor";
    }
    commands.push_back({"parec", "--device=" + device,
                        "--format=s16le", "--rate=48000", "--channels=2",
                        "--latency-msec=20", "--raw"});
  }
  if (audioCapture == "auto" || audioCapture == "pw-record")
    commands.push_back({"pw-record", "-P", "{ stream.capture.sink=true }",
                        "--format=s16", "--rate=48000", "--channels=2",
                        "-"});

  nextSpawn.tv_sec = nextSpawn.tv_usec = 0;
  spawnChild();
  timer.start(tickMs);
}

void AudioCapture::stop()
{
  timer.stop();
  stopChild(true);
  if (server != nullptr)
    server->setAudioAvailable(false);
  server = nullptr;
  buffer.clear();
}

void AudioCapture::spawnChild()
{
  if (commands.empty())
    return;

  const std::vector<std::string>& args = commands[command];

  childGotData = false;
  fd = spawnPipe(args, &child);
  if (fd < 0) {
    vlog.debug("Could not start %s: %s", args[0].c_str(), strerror(errno));
    child = -1;
    stopChild(false);
    return;
  }

  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  gettimeofday(&spawnTime, nullptr);
  childGotData = false;
  buffer.clear();
  vlog.debug("Started %s for sound capture", args[0].c_str());
}

// Cleans up after a child that ended (or that we end now), and picks
// when and with what to try again.
void AudioCapture::stopChild(bool killChild)
{
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }

  if (child > 0) {
    if (killChild)
      kill(child, SIGTERM);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR)
      ;
    child = -1;
  }

  if (killChild || commands.empty())
    return;

  // Move on to the next way of capturing unless this one worked
  if (!childGotData) {
    command = (command + 1) % commands.size();
    if (command == 0) {
      failures++;
      if (failures == 1)
        vlog.info("Sound capture is not available, will keep trying");
    }
  } else {
    vlog.info("Sound capture stopped, restarting");
  }

  int delayMs = command == 0 ? (failures > 3 ? 10000 : 2000) : 0;
  gettimeofday(&nextSpawn, nullptr);
  nextSpawn.tv_usec += (delayMs % 1000) * 1000;
  nextSpawn.tv_sec += delayMs / 1000 + nextSpawn.tv_usec / 1000000;
  nextSpawn.tv_usec %= 1000000;
}

void AudioCapture::handleTimeout(core::Timer* t)
{
  if (t != &timer)
    return;

  if (tone) {
    generateTone();
    timer.repeat();
    return;
  }

  if (fd < 0) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    if (now.tv_sec > nextSpawn.tv_sec ||
        (now.tv_sec == nextSpawn.tv_sec && now.tv_usec >= nextSpawn.tv_usec))
      spawnChild();
  }

  if (fd >= 0)
    readChild();

  timer.repeat();
}

void AudioCapture::readChild()
{
  uint8_t buf[16384];
  bool ended = false;

  while (true) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        ended = true;
      break;
    }
    if (n == 0) {
      ended = true;
      break;
    }
    buffer.insert(buffer.end(), buf, buf + n);
  }

  // A monitor of an idle sink may deliver nothing at all, so a
  // recorder that keeps running counts as working too
  if (!childGotData &&
      (!buffer.empty() || (!ended && core::msSince(&spawnTime) > 1000)))
    childWorks();

  sendBuffered();

  if (ended)
    stopChild(false);
}

void AudioCapture::childWorks()
{
  childGotData = true;
  failures = 0;
  vlog.info("Capturing sound with %s", commands[command][0].c_str());
  server->setAudioAvailable(true);
}

void AudioCapture::sendBuffered()
{
  size_t frames = buffer.size() / frameBytes;
  size_t first = 0;

  // If we fell behind (the server was busy), keep only the newest audio
  if (frames > maxBufferedFrames)
    first = frames - maxBufferedFrames;

  for (size_t f = first; f < frames; f += maxChunkFrames) {
    size_t count = std::min(maxChunkFrames, frames - f);
    const uint8_t* p = buffer.data() + f * frameBytes;

    pcm.resize(count * channels);
    for (size_t i = 0; i < count * channels; i++)
      pcm[i] = (int16_t)(p[2 * i] | (p[2 * i + 1] << 8));

    server->sendAudio(pcm.data(), count);
  }

  buffer.erase(buffer.begin(), buffer.begin() + frames * frameBytes);
}

void AudioCapture::generateTone()
{
  // A 440 Hz beep for a quarter of every second
  uint64_t due = (uint64_t)core::msSince(&toneStart) * rate / 1000;

  if (due - toneFrames > maxBufferedFrames)
    toneFrames = due - maxBufferedFrames;

  while (toneFrames < due) {
    size_t count = std::min((uint64_t)maxChunkFrames, due - toneFrames);
    pcm.resize(count * channels);
    for (size_t i = 0; i < count; i++) {
      uint64_t frame = toneFrames + i;
      int16_t v = 0;
      if (frame % rate < (uint64_t)rate / 4)
        v = (int16_t)(sin(2 * M_PI * 440.0 * frame / rate) * 6000);
      pcm[2 * i] = pcm[2 * i + 1] = v;
    }
    server->sendAudio(pcm.data(), count);
    toneFrames += count;
  }
}

//
// MicSink
//

MicSink::MicSink()
  : moduleIndex(-1), fd(-1), started(false)
{
}

MicSink::~MicSink()
{
  close();
}

void MicSink::open()
{
  std::string output;
  const char* dir;
  int status;

  if (fd >= 0 || micSource != "auto")
    return;

  dir = getenv("XDG_RUNTIME_DIR");
  if (dir == nullptr || dir[0] == '\0')
    dir = "/tmp";
  path = std::string(dir) + "/questvnc-mic-" + std::to_string(getpid());

  // module-pipe-source creates the FIFO and reads it at real-time rate
  status = runCommand({"pactl", "load-module", "module-pipe-source",
                       "source_name=questvnc_mic", "file=" + path,
                       "format=s16le", "rate=" + std::to_string(micRate),
                       "channels=1",
                       "source_properties=device.description=QuestVNC-Mic"},
                      &output, 5000);
  if (status != 0) {
    vlog.error("Could not create the QuestVNC-Mic source with pactl");
    return;
  }

  moduleIndex = atoi(output.c_str());

  fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    vlog.error("Could not open %s: %s", path.c_str(), strerror(errno));
    close();
    return;
  }

  vlog.info("Headset microphone available as the QuestVNC-Mic source");
}

void MicSink::close()
{
  std::string output;

  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }

  if (moduleIndex >= 0) {
    runCommand({"pactl", "unload-module", std::to_string(moduleIndex)},
               &output, 5000);
    moduleIndex = -1;
  }

  started = false;
}

void MicSink::start(int channels_, uint32_t sampleRate)
{
  if (fd < 0)
    return;

  try {
    resampler.setFormat(sampleRate, channels_, micRate, 1);
  } catch (std::exception& e) {
    vlog.error("Microphone: %s", e.what());
    return;
  }

  pending.clear();
  started = true;
}

void MicSink::data(const uint8_t* data, size_t len)
{
  if (fd < 0 || !started)
    return;

  pending.insert(pending.end(), data, data + len);

  size_t frameLen = resampler.inputChannels() * 2;
  size_t frames = pending.size() / frameLen;
  if (frames == 0)
    return;

  in.resize(frames * resampler.inputChannels());
  for (size_t i = 0; i < in.size(); i++)
    in[i] = (int16_t)(pending[2 * i] | (pending[2 * i + 1] << 8));
  pending.erase(pending.begin(), pending.begin() + frames * frameLen);

  out.clear();
  resampler.process(in.data(), frames, &out);

  bytes.resize(out.size() * 2);
  for (size_t i = 0; i < out.size(); i++) {
    bytes[2 * i] = out[i] & 0xff;
    bytes[2 * i + 1] = (out[i] >> 8) & 0xff;
  }

  // Writes of up to PIPE_BUF bytes are atomic, so a full pipe never
  // leaves half a sample behind. If PulseAudio is not keeping up, the
  // audio is dropped.
  for (size_t off = 0; off < bytes.size(); ) {
    size_t n = std::min(bytes.size() - off, (size_t)PIPE_BUF & ~(size_t)1);
    ssize_t w = write(fd, bytes.data() + off, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        vlog.error("Microphone write failed: %s", strerror(errno));
      break;
    }
    off += w;
  }
}

void MicSink::stop()
{
  started = false;
  pending.clear();
}
