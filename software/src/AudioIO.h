#pragma once

#include <Audio.h>

namespace OC {
  namespace AudioIO {
    // total block size including header is 260 bytes
    // 252 * 260 fits nicely into two 32KB pages
    const int AUDIO_MEMORY = 252;
    AudioStream& InputStream(int interface = 0);
    AudioStream& OutputStream();
    // Lets one app (DOOM) take over the audio outputs: connect its stereo
    // stream once, then switch between it and the applet chain.
    void ConnectOverride(AudioStream& source);
    void SetOverride(bool on);
    void Init();
  }
}
