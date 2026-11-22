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
constexpr int kHttpTimeoutMs = 10000;
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


namespace {

// ID3v2 的文本帧编码：0=ISO-8859-1, 1=UTF-16(带 BOM), 2=UTF-16BE, 3=UTF-8
std::string Id3TextToUtf8(const uint8_t* data, size_t len) {
    if (len == 0) {
        return std::string();
    }
    uint8_t enc = data[0];
    const uint8_t* p = data + 1;
    size_t n = len - 1;
    std::string out;
    if (enc == 0 || enc == 3) {              // latin1 / utf8
        out.assign(reinterpret_cast<const char*>(p),
                   strnlen(reinterpret_cast<const char*>(p), n));
        return out;
    }
    // UTF-16：跳过 BOM，按小端/大端解码
    bool big_endian = (enc == 2);
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        big_endian = false;
        p += 2; n -= 2;
    } else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        big_endian = true;
        p += 2; n -= 2;
    }
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t ch = big_endian ? (uint32_t(p[i]) << 8 | p[i + 1])
                                 : (uint32_t(p[i + 1]) << 8 | p[i]);
        if (ch == 0) {
            break;
        }
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return out;
}

std::string TrimTrailingSpace(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

}  // namespace

void MusicPlayer::ParseId3(const uint8_t* data, size_t len) {
    std::string title, artist;
    if (len >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        const uint8_t major = data[3];
        const uint32_t tag_size = (static_cast<uint32_t>(data[6] & 0x7F) << 21) |
                                  (static_cast<uint32_t>(data[7] & 0x7F) << 14) |
                                  (static_cast<uint32_t>(data[8] & 0x7F) << 7) |
                                  (static_cast<uint32_t>(data[9] & 0x7F));
        size_t pos = 10;
        const size_t end = std::min<size_t>(len, static_cast<size_t>(10) + tag_size);
        while (pos + 10 <= end) {
            const uint8_t* f = data + pos;
            if (f[0] == 0) {                     // 填充区，结束
                break;
            }
            const uint32_t fsize = (major >= 4)
                ? ((static_cast<uint32_t>(f[4] & 0x7F) << 21) |
                   (static_cast<uint32_t>(f[5] & 0x7F) << 14) |
                   (static_cast<uint32_t>(f[6] & 0x7F) << 7) |
                   (static_cast<uint32_t>(f[7] & 0x7F)))
                : ((static_cast<uint32_t>(f[4]) << 24) |
                   (static_cast<uint32_t>(f[5]) << 16) |
                   (static_cast<uint32_t>(f[6]) << 8) | f[7]);
            if (fsize == 0 || pos + 10 + fsize > len) {
                break;
            }
            const bool is_title = (f[0] == 'T' && f[1] == 'I' && f[2] == 'T' && f[3] == '2');
            const bool is_artist = (f[0] == 'T' && f[1] == 'P' && f[2] == 'E' && f[3] == '1');
            if (is_title || is_artist) {
                std::string v = TrimTrailingSpace(Id3TextToUtf8(f + 10, fsize));
                if (is_title && title.empty()) {
                    title = v;
                } else if (is_artist && artist.empty()) {
                    artist = v;
                }
            }
            pos += 10 + fsize;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id3_title_ = title;
        id3_artist_ = artist;
        id3_parsed_ = true;
    }
}

std::string MusicPlayer::TagTitle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id3_title_;
}

std::string MusicPlayer::TagArtist() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id3_artist_;
}

MusicPlayer::MusicPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

MusicPlayer::~MusicPlayer() { Stop(); }

bool MusicPlayer::Start(std::string audio_url, uint32_t playback_id,
                        FinishedCallback finished_callback) {
    if (playback_id == 0 || !IsSupportedUrl(audio_url)) {
        ESP_LOGE(TAG, "Invalid url or playback id");
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);

    auto apply_request = [&]() {
        audio_url_ = std::move(audio_url);
        finished_callback_ = std::move(finished_callback);
        playback_id_ = playback_id;
        active_ = true;
        cancelled_ = false;
        paused_ = false;
        http_finished_ = false;
        playback_drained_ = false;
        completion_reported_ = false;
        total_bytes_ = 0;
        current_byte_ = 0;
        played_samples_ = 0;
        position_rate_ = 0;
        id3_parsed_ = false;
        id3_title_.clear();
        id3_artist_.clear();
        bitrate_bps_ = 0;
        seek_target_byte_ = -1;
        seek_pending_ = false;
        pending_seek_seconds_ = -1;
        skip_remaining_samples_ = 0;
    };

    if (worker_running_) {
        // 旧 worker 还在跑：把本次请求排队并取消旧曲目。
        // 旧 worker 退出时会自动拉起新曲目，避免 Stop/Start 竞态。
        ESP_LOGI(TAG, "Switching track while worker is running, queueing request");
        pending_url_ = std::move(audio_url);
        pending_callback_ = std::move(finished_callback);
        pending_playback_id_ = playback_id;
        cancelled_ = true;
        active_ = false;
        paused_ = false;
        pause_cv_.notify_all();
        // 持锁清空播放队列：旧 worker 在推帧前会检查 cancelled_ 而退出，
        // 而新 worker 要等旧 worker 退出（需要本锁）才会创建，
        // 因此这里不会误删新曲目的帧。AudioService 的回调在释放自身锁后
        // 才调用，不会与 mutex_ 形成反向依赖。
        audio_service_.ResetDecoder();
        return true;
    }

    apply_request();
    worker_running_ = true;
    if (!StartWorkerLocked()) {
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        return false;
    }
    return true;
}

bool MusicPlayer::StartWorkerLocked() {
    BaseType_t created = xTaskCreate(WorkerEntry, "music_http", kMusicTaskStackSize, this,
                                     kMusicTaskPriority, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create music task");
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void MusicPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        active_ = false;
        paused_ = false;
        // 丢弃排队中的换曲请求：Stop 是「真的不播了」。
        pending_url_.clear();
        pending_callback_ = nullptr;
        pending_playback_id_ = 0;
        // 保留 finished_callback_：worker 退出时会回调，
        // 调用方据此决定是否接续播放下一首（避免 Stop 后立即 Start 的竞态）。
    }
    pause_cv_.notify_all();
}

void MusicPlayer::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return;
    }
    paused_ = true;
    ESP_LOGI(TAG, "Music paused");
}

void MusicPlayer::Resume() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || !paused_) {
            return;
        }
        paused_ = false;
        ESP_LOGI(TAG, "Music resumed");
    }
    pause_cv_.notify_all();
}

bool MusicPlayer::IsPaused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

bool MusicPlayer::Seek(int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        return false;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    // 刚启动时码率/总长尚未就绪：先记下目标秒数，等解析出头信息后再换算。
    if (total_bytes_ <= 0 || bitrate_bps_ <= 0) {
        pending_seek_seconds_ = seconds;
        ESP_LOGI(TAG, "Seek deferred until bitrate known: %d s", seconds);
        return true;
    }
    int64_t byte = static_cast<int64_t>(bitrate_bps_) / 8 * seconds;
    if (position_rate_ > 0) {
        played_samples_ = static_cast<int64_t>(seconds) * position_rate_;
    }
    if (byte > total_bytes_) {
        byte = total_bytes_;
    }
    seek_target_byte_ = byte;
    seek_pending_ = true;
    ESP_LOGI(TAG, "Seek requested: %d s -> byte %lld", seconds,
             static_cast<long long>(byte));
    return true;
}

int MusicPlayer::PositionSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // 以"真正入队播放的采样数"为准：current_byte_ 是 HTTP 预读字节，会比实际播放快好几倍
    if (position_rate_ > 0) {
        return static_cast<int>(played_samples_ / position_rate_);
    }
    if (bitrate_bps_ <= 0) {
        return 0;
    }
    return static_cast<int>(current_byte_ * 8 / bitrate_bps_);
}

int MusicPlayer::DurationSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bitrate_bps_ <= 0 || total_bytes_ <= 0) {
        return 0;
    }
    return static_cast<int>(total_bytes_ * 8 / bitrate_bps_);
}

void MusicPlayer::WaitIfPaused() {
    std::unique_lock<std::mutex> lock(mutex_);
    pause_cv_.wait(lock, [this]() { return !paused_ || cancelled_ || !active_; });
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
        // 该超时作用于「块间」等待，不是整个响应；音乐流会持续有数据。
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("Accept", "audio/mpeg, audio/*");
        http->SetHeader("Accept-Encoding", "identity");
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        // 部分音源对不带 Referer 的请求返回 403。
        http->SetHeader("Referer", "https://www.kuwo.cn/");
        auto opened = http->Open("GET", audio_url);
        if (opened) {
            auto status = http->GetStatusCode();
            if (status && *status >= 200 && *status < 300 && !IsCancelled(playback_id)) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    total_bytes_ = static_cast<int64_t>(http->GetBodyLength());
                    current_byte_ = 0;
                }
                ESP_LOGI(TAG, "HTTP %d, content-length=%d", *status,
                         static_cast<int>(http->GetBodyLength()));
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
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        position_rate_ = effective_rate;
                    }

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

                        // 跳转支持：丢弃目标位置之前的采样。
                        // 在有损压缩里按时间精确对齐很难，这里按「采样数 = 秒数 ×
                        // 输出采样率」做尽力而为的续播（误差在一个解码块内）。
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            int skip_seek_seconds = -1;
                            if (pending_seek_seconds_ >= 0) {
                                // 没有字节跳转可用（拿不到码率/不支持 Range）：靠丢弃采样对齐
                                skip_seek_seconds = pending_seek_seconds_;
                                pending_seek_seconds_ = -1;
                            } else if (seek_pending_ && bitrate_bps_ > 0) {
                                // 已经用 Range 重新打开到目标字节了，不能再丢一遍采样，
                                // 否则会跳过目标位置之后的内容（实测跳完直接播完/停住）
                                seek_pending_ = false;
                                seek_target_byte_ = -1;
                            }
                            if (skip_seek_seconds > 0) {
                                const int rate = effective_rate > 0 ? effective_rate : 24000;
                                skip_remaining_samples_ +=
                                    static_cast<int64_t>(skip_seek_seconds) * rate;
                            }
                        }
                        if (skip_remaining_samples_ > 0) {
                            const int64_t available = static_cast<int64_t>(mono.size());
                            if (skip_remaining_samples_ >= available) {
                                // 整块都在跳转点之前：丢弃并继续等下一块。
                                skip_remaining_samples_ -= available;
                                return;
                            }
                            mono.erase(mono.begin(),
                                       mono.begin() + skip_remaining_samples_);
                            skip_remaining_samples_ = 0;
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
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                played_samples_ += static_cast<int64_t>(chunk);
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
                                            // esp_audio_simple_dec_get_info() 的 bitrate 单位是 kbps，
                                            // 直接当 bps 用会小 1000 倍：position/duration 变毫秒、
                                            // Seek 算出 1/1000 的偏移（跳到 13 s 而不是 269 s）。
                                            const int bitrate_kbps = static_cast<int>(info.bitrate);
                                            const int bitrate_bps =
                                                bitrate_kbps > 0 && bitrate_kbps < 1000
                                                    ? bitrate_kbps * 1000
                                                    : bitrate_kbps;
                                            ESP_LOGI(TAG, "MP3 stream: %d Hz, %d ch, %d kbps",
                                                     src_rate, src_channels, bitrate_kbps);
                                            std::lock_guard<std::mutex> lock(mutex_);
                                            bitrate_bps_ = bitrate_bps;
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
                        // 暂停时阻塞在这里，不读取也不推帧。
                        WaitIfPaused();
                        if (IsCancelled(playback_id)) {
                            break;
                        }
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
                        {
                            bool need_id3 = false;
                            {
                                std::lock_guard<std::mutex> lock(mutex_);
                                need_id3 = !id3_parsed_;
                            }
                            if (need_id3) {
                                ParseId3(reinterpret_cast<const uint8_t*>(http_buf.data()), *size);
                            }
                            std::lock_guard<std::mutex> lock(mutex_);
                            current_byte_ += *size;
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
    bool switch_to_pending = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
        if (!pending_url_.empty()) {
            // 有排队的换曲请求：直接接管参数并重启 worker，
            // 不向调用方汇报「结束」，避免上层误以为播放已终止。
            ESP_LOGI(TAG, "Starting queued track after worker exit");
            audio_url_ = std::move(pending_url_);
            pending_url_.clear();
            finished_callback_ = std::move(pending_callback_);
            pending_callback_ = nullptr;
            playback_id_ = pending_playback_id_;
            pending_playback_id_ = 0;
            active_ = true;
            worker_running_ = true;
            cancelled_ = false;
            paused_ = false;
            http_finished_ = false;
            playback_drained_ = false;
            completion_reported_ = false;
            total_bytes_ = 0;
            current_byte_ = 0;
            bitrate_bps_ = 0;
            seek_target_byte_ = -1;
            seek_pending_ = false;
            pending_seek_seconds_ = -1;
            skip_remaining_samples_ = 0;
            switch_to_pending = true;
        } else if (playback_id == playback_id_) {
            if (cancelled_) {
                // 被主动停止：通知调用方，以便接续播放（下一曲/替换曲目）。
                completed_playback_id = playback_id_;
                callback = finished_callback_;
                finished_callback_ = nullptr;
                completion_reported_ = true;
                active_ = false;
                report_success = false;
            } else if (active_) {
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
    }
    if (switch_to_pending) {
        // 与 Start() 一致：持锁创建任务。
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_url_.empty()) {
            // 竞态：在这两步之间又有新的换曲请求，直接接管它，
            // 避免丢失请求导致「下一首」失效。
            ESP_LOGI(TAG, "Newer track queued during switch, taking it");
            audio_url_ = std::move(pending_url_);
            pending_url_.clear();
            finished_callback_ = std::move(pending_callback_);
            pending_callback_ = nullptr;
            playback_id_ = pending_playback_id_;
            pending_playback_id_ = 0;
            active_ = true;
            cancelled_ = false;
            paused_ = false;
            http_finished_ = false;
            playback_drained_ = false;
            completion_reported_ = false;
        }
        if (cancelled_ || !active_) {
            // 期间被 Stop() 撤销：清理状态，避免 worker_running_ 卡在 true。
            worker_running_ = false;
            active_ = false;
            return;
        }
        if (!StartWorkerLocked()) {
            ESP_LOGE(TAG, "Failed to start queued track");
            worker_running_ = false;
            active_ = false;
        }
        return;
    }
    if (callback) {
        callback(completed_playback_id, report_success);
    }
}
