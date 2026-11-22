Quest VNC fork of TigerVNC
==========================

This fork adds the Quest VNC headset extensions to TigerVNC's servers
(Xvnc and x0vncserver), so the Quest VNC headset app gets the desktop's
sound and can send its microphone over the same VNC connection. Xvnc is
the X server itself, so it knows exactly what changed on screen and needs
no capture-and-re-encode layer.

Everything here stays under the GNU General Public License, version 2 or
later, like the rest of TigerVNC (see LICENCE.TXT).

These changes are not intended for upstream TigerVNC, whose AI policy
(AI_POLICY.md) does not accept significant LLM-generated contributions.

Sound (QEMU audio extension)
----------------------------

 * A client that lists pseudo-encoding -259 gets a -259 pseudo-rectangle
   in its next framebuffer update once the server has sound.
 * The client sends QEMU client messages (type 255, submessage 1):
   operation 2 sets the format (`u8 format, u8 channels, u32 frequency`),
   0 enables and 1 disables. All six QEMU sample formats, 1 or 2 channels
   and 8000-96000 Hz are supported; the default is S16, 2 channels,
   48000 Hz.
 * The server answers enable with "stream begins" (operation 1), then sends
   10-20 ms chunks of little-endian PCM (operation 2), and "stream ends"
   (operation 0) on disable.
 * If a client stops taking data for more than 250 ms, its sound is dropped
   rather than queued.

Capture is set with `-AudioCapture`: `auto` (default; `parec` on the default
output's monitor, then `pw-record`), `parec`, `pw-record`, `tone` (test beep)
or `none`. It runs only while clients are connected.

Headset microphone (Quest VNC extension)
----------------------------------------

 * A client that lists pseudo-encoding -2703 gets a -2703 pseudo-rectangle
   carrying one byte, 1, if the server has a microphone sink.
 * Then, and only then, the client may send message type 248:
   `u8 248, u8 1, u8 channels, u32 rate` starts, `u8 248, u8 2, u32 length,
   <length bytes S16LE>` carries audio (at most 65536 bytes) and
   `u8 248, u8 3` stops.
 * Type 248 is ClientFence in the standard protocol. The server therefore
   only offers the microphone to clients that do not list the fence
   pseudo-encoding (-312), and ignores fence support from a client once the
   microphone has been offered. The Quest VNC app does not use fences.
 * One client feeds the microphone at a time; the latest to start wins.
   View-only clients are not offered it.

The sink is set with `-MicSource`: `auto` creates a PulseAudio pipe source
named QuestVNC-Mic (with `pactl load-module module-pipe-source`) while
clients are connected and unloads it when the last one leaves; `none`
(default) turns the microphone off. Audio is resampled to 48 kHz mono.
PulseAudio may make QuestVNC-Mic the default input while it exists.

Example
-------

    Xvnc :1 -geometry 1920x1080 -SecurityTypes VncAuth -PasswordFile ~/.vnc/passwd \
         -AudioCapture auto -MicSource auto

Code
----

 * `common/rfb/AudioResampler.*`: rate, channel and sample format conversion.
 * `common/rfb/SMsgReader.cxx`, `SMsgWriter.cxx`, `SConnection.cxx`,
   `VNCSConnectionST.cxx`, `VNCServerST.cxx`: the protocol.
 * `unix/common/QuestAudio.*`: capture and the PulseAudio microphone source.
 * `unix/xserver/hw/vnc/XserverDesktop.cc`, `unix/x0vncserver/XDesktop.cxx`:
   hooks into Xvnc and x0vncserver.
 * `tests/unit/audioresampler.cxx`: unit tests.
