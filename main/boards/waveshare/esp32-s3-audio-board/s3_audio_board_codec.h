#ifndef _S3_AUDIO_BOARD_CODEC_H_
#define _S3_AUDIO_BOARD_CODEC_H_

#include "codecs/box_audio_codec.h"

// 对外逻辑音量 0-100；硬件实际音量按 AUDIO_OUTPUT_MAX_VOLUME（见 config.h）线性缩放，
// 例如上限 65 时：逻辑 100 -> 硬件 65，逻辑 50 -> 硬件 33。
class S3AudioBoardCodec : public BoxAudioCodec {
public:
    using BoxAudioCodec::BoxAudioCodec;

    void SetOutputVolume(int volume) override;
    void EnableOutput(bool enable) override;

    // 逻辑音量 -> 硬件音量（0-100 范围线性缩放到 [0, AUDIO_OUTPUT_MAX_VOLUME]）。
    static int ToHardwareVolume(int logical);
};

#endif  // _S3_AUDIO_BOARD_CODEC_H_
