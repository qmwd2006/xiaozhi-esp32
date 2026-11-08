#include "music_player.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_log.h>

#include "board.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec_reg.h"
#include "esp_es_parse_types.h"
#include "esp_mp3_dec.h"
#include "http.h"

namespace {
constexpr int kHttpTimeoutMs = 5000;
constexpr size_t kHttpReadBufferSize = 2048;
constexpr uint32_t kMusicTaskStackSize = 8192;
constexpr UBaseType_t kMusicTaskPriority = 2;
// 解码输出缓冲：MP3 单帧最大约 1152 samples * 2 声道 * 2 字节
constexpr size_t kPcmFrameBytes = 8192;
// 一帧播放块的采样数（按输出采样率计），20ms @24k = 480
constexpr int kTargetFrameMs = 20;
const char* TAG = "MusicPlayer";

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

bool GetMp3Version(uint8_t version_bits, uint8_t* version) {
    if (version_bits == 0b11) {
        *version = 0;  // MPEG-1
    } else if (version_bits == 0b10) {
        *version = 1;  // MPEG-2
    } else if (version_bits == 0b00) {
        *version = 2;  // MPEG-2.5
    } else {
        return false;
    }
    return true;
}

// 自定义 MP3 ES parser：定位完整帧并给出帧长、采样率、声道数。
esp_es_parse_err_t Mp3ParseFrame(esp_es_parse_raw_t* in, esp_es_parse_frame_info_t* frame_info) {
    uint8_t* header = in->buffer;

    // 跳过 ID3v2 头（音乐源返回的 MP3 常带）
    if (in->bos) {
        if (in->len >= 10 && header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
            uint32_t tag_size = (static_cast<uint32_t>(header[6] & 0x7F) << 21) |
                                (static_cast<uint32_t>(header[7] & 0x7F) << 14) |
                                (static_cast<uint32_t>(header[8] & 0x7F) << 7) |
                                (static_cast<uint32_t>(header[9] & 0x7F));
            frame_info->skipped_size = 10 + tag_size;
            return ESP_ES_PARSE_ERR_OK;
        }
    }

    if (in->len < 4) {
        return ESP_ES_PARSE_ERR_DATA_NOT_ENOUGH;
    }
    // 帧同步（11 bit）
    if (header[0] != 0xFF || (header[1] & 0xE0) != 0xE0) {
        return ESP_ES_PARSE_ERR_WRONG_HEADER;
    }

    uint8_t version_bits = (header[1] >> 3) & 0x03;
    uint8_t layer_bits = (header[1] >> 1) & 0x03;
    uint8_t bitrate_index = (header[2] >> 4) & 0x0F;
    uint8_t sample_rate_index = (header[2] >> 2) & 0x03;
    uint8_t padding_bit = (header[2] >> 1) & 0x01;
    uint8_t channel_mode = (header[3] >> 6) & 0x03;

    uint8_t version_idx;
    if (!GetMp3Version(version_bits, &version_idx)) {
        return ESP_ES_PARSE_ERR_WRONG_HEADER;
    }
    if (layer_bits == 0b00) {
        return ESP_ES_PARSE_ERR_WRONG_HEADER;
    }
    uint8_t layer_idx = 3 - layer_bits;  // Layer I->0, II->1, III->2

    static const uint16_t kBitrateTable[9][16] = {
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    };
    static const uint32_t kSampleRateTable[3][4] = {
        {44100, 48000, 32000, 0},
        {22050, 24000, 16000, 0},
        {11025, 12000, 8000, 0},
    };

    int sample_rate = kSampleRateTable[version_idx][sample_rate_index];
    if (sample_rate == 0) {
        return ESP_ES_PARSE_ERR_WRONG_HEADER;
    }
    int table_row = version_idx * 3 + layer_idx;
    int bitrate = kBitrateTable[table_row][bitrate_index];
    if (bitrate == 0) {
        return ESP_ES_PARSE_ERR_WRONG_HEADER;
    }

    int samples_per_frame;
    if (layer_bits == 0b11) {  // Layer I
        samples_per_frame = 384;
        frame_info->frame_size = ((12000 * bitrate) / sample_rate + padding_bit) * 4;
    } else if (layer_bits == 0b10) {  // Layer II
        samples_per_frame = 1152;
        frame_info->frame_size = (samples_per_frame / 8 * bitrate * 1000) / sample_rate + padding_bit;
    } else {  // Layer III
        samples_per_frame = (version_idx == 0) ? 1152 : 576;
        frame_info->frame_size = (samples_per_frame / 8 * bitrate * 1000) / sample_rate + padding_bit;
    }

    frame_info->aud_info.sample_rate = sample_rate;
    frame_info->aud_info.sample_num = samples_per_frame;
    frame_info->aud_info.bits_per_sample = 16;
    frame_info->aud_info.channel = (channel_mode == 0x03) ? 1 : 2;  // 3 = single channel
    frame_info->aud_info.bitrate = bitrate * 1000;
    return ESP_ES_PARSE_ERR_OK;
}

// MP3 不在 esp_audio_simple_dec_register_default() 的默认集合里，需自行注册。
void EnsureMp3DecoderRegistered() {
    static bool registered = false;
    if (registered) {
        return;
    }
    esp_audio_dec_register_default();
    esp_audio_simple_dec_reg_info_t reg_info = {};
    reg_info.decoder_ops = ESP_MP3_DEC_DEFAULT_OPS();
    reg_info.parser = Mp3ParseFrame;
    reg_info.free = nullptr;
    esp_audio_simple_dec_register(ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, &reg_info);
    registered = true;
}
}  // namespace

MusicPlayer::MusicPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

MusicPlayer::~MusicPlayer() { Stop(); }

bool MusicPlayer::Start(std::string audio_url, uint32_t playback_id,
                        FinishedCallback finished_callback) {
    if (playback_id == 0 || !IsSupportedUrl(audio_url)) {
        ESP_LOGE(TAG, "Invalid url or playback id");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || worker_running_) {
            ESP_LOGW(TAG, "Music player is busy");
            return false;
        }
        audio_url_ = std::move(audio_url);
        finished_callback_ = std::move(finished_callback);
        playback_id_ = playback_id;
        active_ = true;
        worker_running_ = true;
        cancelled_ = false;
        http_finished_ = false;
        playback_drained_ = false;
        completion_reported_ = false;
    }

    BaseType_t created = xTaskCreate(WorkerEntry, "music_http", kMusicTaskStackSize, this,
                                     kMusicTaskPriority, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create music task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void MusicPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    active_ = false;
    finished_callback_ = nullptr;
}

bool MusicPlayer::IsActive(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && (playback_id == 0 || playback_id == playback_id_);
}

bool MusicPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || worker_running_;
}

bool MusicPlayer::IsCancelled(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_ || !active_ || playback_id != playback_id_;
}

MusicPlayer::FinishedCallback MusicPlayer::CompleteLocked(uint32_t& playback_id) {
    if (completion_reported_ || !active_) {
        return nullptr;
    }
    completion_reported_ = true;
    active_ = false;
    playback_id = playback_id_;
    return finished_callback_;
}

void MusicPlayer::OnPlaybackDrained() {
    FinishedCallback callback;
    uint32_t playback_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        playback_drained_ = true;
        if (http_finished_) {
            callback = CompleteLocked(playback_id);
        }
    }
    if (callback) {
        callback(playback_id, true);
    }
}

void MusicPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<MusicPlayer*>(arg);
    player->WorkerTask();
    vTaskDelete(nullptr);
}

void MusicPlayer::WorkerTask() {
    std::string audio_url;
    uint32_t playback_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_url = audio_url_;
        playback_id = playback_id_;
    }

    bool success = false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (http) {
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("Accept", "audio/mpeg, audio/*");
        http->SetHeader("Accept-Encoding", "identity");
        auto opened = http->Open("GET", audio_url);
        if (opened) {
            auto status = http->GetStatusCode();
            if (status && *status >= 200 && *status < 300 && !IsCancelled(playback_id)) {
                EnsureMp3DecoderRegistered();
                esp_audio_simple_dec_cfg_t dec_cfg = {};
                dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
                dec_cfg.use_frame_dec = false;

                esp_audio_simple_dec_handle_t decoder = nullptr;
                auto ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
                if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
                    ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
                } else {
                    success = true;
                    std::array<char, kHttpReadBufferSize> http_buf;
                    std::vector<uint8_t> pcm_buf(kPcmFrameBytes);
                    int src_rate = 0;
                    int src_channels = 0;
                    esp_ae_rate_cvt_handle_t resampler = nullptr;
                    int resample_rate = 0;
                    const int target_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
                    // 采样率/声道数未知时，按输出格式处理（多数解码器首帧即给出信息）
                    const int effective_rate = target_rate > 0 ? target_rate : 24000;

                    auto close_resampler = [&]() {
                        if (resampler != nullptr) {
                            esp_ae_rate_cvt_close(resampler);
                            resampler = nullptr;
                        }
                    };

                    auto emit_pcm = [&](const uint8_t* data, size_t bytes) {
                        if (bytes < sizeof(int16_t)) {
                            return;
                        }
                        size_t samples = bytes / sizeof(int16_t);
                        const int16_t* src = reinterpret_cast<const int16_t*>(data);

                        // 多声道下混为单声道
                        std::vector<int16_t> mono;
                        if (src_channels > 1) {
                            size_t frames = samples / static_cast<size_t>(src_channels);
                            mono.resize(frames);
                            for (size_t i = 0; i < frames; ++i) {
                                int32_t acc = 0;
                                for (int c = 0; c < src_channels; ++c) {
                                    acc += src[i * src_channels + c];
                                }
                                mono[i] = static_cast<int16_t>(acc / src_channels);
                            }
                        } else {
                            mono.assign(src, src + samples);
                        }
                        if (mono.empty()) {
                            return;
                        }

                        // 重采样到 codec 输出采样率
                        if (src_rate > 0 && target_rate > 0 && src_rate != target_rate) {
                            if (resampler == nullptr || resample_rate != src_rate) {
                                close_resampler();
                                esp_ae_rate_cvt_cfg_t cfg = {};
                                cfg.src_rate = static_cast<uint32_t>(src_rate);
                                cfg.dest_rate = static_cast<uint32_t>(target_rate);
                                cfg.channel = ESP_AUDIO_MONO;
                                cfg.bits_per_sample = ESP_AUDIO_BIT16;
                                cfg.complexity = 2;
                                cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                                if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK) {
                                    resampler = nullptr;
                                }
                                resample_rate = src_rate;
                            }
                            if (resampler != nullptr) {
                                uint32_t max_out = 0;
                                esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(),
                                                                       &max_out);
                                if (max_out == 0) {
                                    return;
                                }
                                std::vector<int16_t> out(max_out);
                                uint32_t actual = max_out;
                                esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono.data(),
                                                        mono.size(), (esp_ae_sample_t)out.data(),
                                                        &actual);
                                mono.assign(out.begin(), out.begin() + actual);
                            }
                        }

                        // 按固定帧长切分后入队
                        const size_t frame_samples =
                            static_cast<size_t>(std::max(1, effective_rate * kTargetFrameMs / 1000));
                        size_t offset = 0;
                        while (offset < mono.size()) {
                            if (IsCancelled(playback_id)) {
                                return;
                            }
                            size_t chunk = std::min(frame_samples, mono.size() - offset);
                            std::vector<int16_t> frame(mono.begin() + offset,
                                                       mono.begin() + offset + chunk);
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                if (cancelled_ || !active_ || playback_id != playback_id_) {
                                    return;
                                }
                                playback_drained_ = false;
                            }
                            if (!audio_service_.PushPcmToPlaybackQueue(std::move(frame), true)) {
                                return;
                            }
                            offset += chunk;
                        }
                    };

                    auto process_input = [&](const uint8_t* data, size_t len, bool eos) {
                        esp_audio_simple_dec_raw_t raw = {};
                        raw.buffer = const_cast<uint8_t*>(data);
                        raw.len = static_cast<uint32_t>(len);
                        raw.eos = eos;

                        while (true) {
                            if (raw.len == 0 && !eos) {
                                break;
                            }
                            esp_audio_simple_dec_out_t out = {};
                            out.buffer = pcm_buf.data();
                            out.len = static_cast<uint32_t>(pcm_buf.size());

                            auto dret = esp_audio_simple_dec_process(decoder, &raw, &out);
                            if (dret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                                pcm_buf.resize(out.needed_size);
                                continue;
                            }
                            if (dret == ESP_AUDIO_ERR_OK) {
                                if (out.decoded_size > 0) {
                                    if (src_rate == 0 || src_channels == 0) {
                                        esp_audio_simple_dec_info_t info = {};
                                        if (esp_audio_simple_dec_get_info(decoder, &info) ==
                                            ESP_AUDIO_ERR_OK) {
                                            src_rate = static_cast<int>(info.sample_rate);
                                            src_channels = info.channel;
                                            ESP_LOGI(TAG, "MP3 stream: %d Hz, %d ch", src_rate,
                                                     src_channels);
                                        }
                                    }
                                    emit_pcm(pcm_buf.data(), out.decoded_size);
                                }
                                // EOS 冲刷只做一轮，避免空输入死循环
                                if (eos && raw.consumed == 0) {
                                    break;
                                }
                            } else if (dret == ESP_AUDIO_ERR_NOT_SUPPORT) {
                                ESP_LOGE(TAG, "Unsupported MP3 stream");
                                success = false;
                                return;
                            } else {
                                // 数据不足/解析错误：等待更多输入
                                break;
                            }

                            if (raw.consumed == 0) {
                                break;
                            }
                            raw.buffer += raw.consumed;
                            raw.len -= raw.consumed;
                            if (raw.len == 0) {
                                break;
                            }
                        }
                    };

                    while (!IsCancelled(playback_id)) {
                        auto size = http->Read(http_buf.data(), http_buf.size());
                        if (!size) {
                            ESP_LOGE(TAG, "Music HTTP read failed: %s",
                                     size.error().ToString().c_str());
                            success = false;
                            break;
                        }
                        if (*size == 0) {
                            break;
                        }
                        process_input(reinterpret_cast<const uint8_t*>(http_buf.data()), *size,
                                      false);
                    }

                    if (success && !IsCancelled(playback_id)) {
                        // 冲刷解码器内部缓存
                        process_input(nullptr, 0, true);
                    }

                    close_resampler();
                    esp_audio_simple_dec_close(decoder);
                }
            } else if (!status) {
                ESP_LOGE(TAG, "Music HTTP status failed: %s",
                         status.error().ToString().c_str());
            } else {
                ESP_LOGE(TAG, "Music HTTP request returned status %d", *status);
            }
        } else {
            ESP_LOGE(TAG, "Failed to open music HTTP request: %s",
                     opened.error().ToString().c_str());
        }
        http->Close();
        http.reset();
    }

    FinishedCallback callback;
    uint32_t completed_playback_id = 0;
    bool report_success = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
        if (!cancelled_ && active_ && playback_id == playback_id_) {
            if (!success) {
                callback = CompleteLocked(completed_playback_id);
            } else {
                http_finished_ = true;
                if (playback_drained_) {
                    callback = CompleteLocked(completed_playback_id);
                    report_success = true;
                }
            }
        }
    }
    if (callback) {
        callback(completed_playback_id, report_success);
    }
}
