#include "s3_audio_board_codec.h"

#include <algorithm>

#include "config.h"
#include "settings.h"

#ifndef AUDIO_OUTPUT_MAX_VOLUME
#define AUDIO_OUTPUT_MAX_VOLUME 100
#endif

int S3AudioBoardCodec::ToHardwareVolume(int logical) {
    logical = std::clamp(logical, 0, 100);
    return static_cast<int>(logical * (AUDIO_OUTPUT_MAX_VOLUME / 100.0f) + 0.5f);
}

void S3AudioBoardCodec::SetOutputVolume(int volume) {
    volume = std::clamp(volume, 0, 100);
    const int hw = ToHardwareVolume(volume);
    // 父类会以传入值写硬件并持久化，这里传 hw 得到正确的硬件音量。
    BoxAudioCodec::SetOutputVolume(hw);  // 硬件=hw，NVS=hw，output_volume_=hw
    // 对外/持久化保留逻辑值（0-100），供 status 显示。
    output_volume_ = volume;
    Settings("audio", true).SetInt("output_volume", volume);
}

void S3AudioBoardCodec::EnableOutput(bool enable) {
    // 父类在 enable 时会用 output_volume_ 直接写硬件；若此处为逻辑值会导致瞬时越界。
    // 临时切到缩放后的硬件值，调用后再恢复逻辑值。
    const int logical = output_volume_;
    output_volume_ = ToHardwareVolume(logical);
    BoxAudioCodec::EnableOutput(enable);
    output_volume_ = logical;
}
