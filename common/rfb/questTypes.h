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

// Quest VNC private protocol extensions (see Quest-VNC server/SPEC.md).

#ifndef __RFB_QUESTTYPES_H__
#define __RFB_QUESTTYPES_H__

namespace rfb {

  // In-band headset microphone.
  //
  // The server announces support with a pseudo-rectangle of encoding
  // pseudoEncodingQuestMic (-2703) carrying one byte, questMicCapability.
  // After that, and only then, the client may send message type 248
  // with one of the operations below. Message type 248 is also
  // ClientFence in the standard protocol, so the server only announces
  // this extension to clients that do not use fences.
  const int msgTypeQuestMic = 248;

  const int questMicCapability = 1;

  const int questMicStart = 1;  // u8 channels, u32 sampleRate
  const int questMicData = 2;   // u32 length, length bytes S16LE PCM
  const int questMicStop = 3;

  const unsigned questMicMaxData = 65536;
}

#endif
