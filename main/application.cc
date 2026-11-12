#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_control.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <limits>

#define TAG "Application"

namespace {
constexpr int kOtaCheckIntervalSeconds = 300;
}  // namespace

Application::Application() : notify_player_(audio_service_), music_player_(audio_service_) {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    notify_player_.Stop();
    if (mqtt_control_ != nullptr) {
        mqtt_control_->Stop();
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (startup_wake_word_hint_timer_ != nullptr) {
        esp_timer_stop(startup_wake_word_hint_timer_);
        esp_timer_delete(startup_wake_word_hint_timer_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    ESP_LOGI(TAG, "After board/audio init");
    SystemInfo::PrintHeapStats();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    callbacks.on_playback_progress = [this](uint32_t playback_id, uint32_t media_position_ms) {
        notify_player_.OnPlaybackProgress(playback_id, media_position_ms);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();
    // Registration is complete here, so this is the only place the whole inventory exists.
    // It is logged because a server that accepts a bounded number of tools drops the rest
    // quietly: the only sign on this device was an alert at boot and a sad face, which is
    // easy to see and hard to connect to a tool list.
    mcp_server.LogToolInventory();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_STARTUP_HINT_TIMEOUT;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
            }
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            if (audio_service_.IsPlaybackIdle()) {
                notify_player_.OnPlaybackDrained();
                music_player_.OnPlaybackDrained();
            }
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                bool sent = protocol_ && protocol_->SendAudio(std::move(packet));
                if (sent) {
                    audio_send_failures_ = 0;
                } else {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    HandleAudioSendFailure();
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
            // 空闲时按间隔检查 OTA，便于远程推送固件（无需手动重启）。
            PeriodicOtaCheck();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
        if (bits & MAIN_EVENT_STARTUP_HINT_TIMEOUT) {
            HideStartupWakeWordHint();
        }
        ShowStartupWakeWordHint();
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    // 网络恢复后立即允许一次 OTA 检查，无需等待退避。
    next_ota_check_seconds_ = 0;
    ota_retry_backoff_seconds_ = 0;
    audio_send_failures_ = 0;
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleAudioSendFailure() {
    // 发送失败通常意味着链路已坏（典型表现：TLS 发送超时、对端不响应）。
    // 若不熔断，每帧音频都会重试并阻塞主循环，最终把设备拖死。
    audio_send_failures_++;
    if (audio_send_failures_ < kMaxConsecutiveAudioSendFailures) {
        return;
    }
    ESP_LOGE(TAG, "Audio send failed %d times in a row, closing channel",
             audio_send_failures_);
    audio_send_failures_ = 0;
    if (music_player_.IsBusy()) {
        StopMusic();
    }
    audio_service_.ResetDecoder();
    if (protocol_ != nullptr) {
        protocol_->CloseAudioChannel();
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateNotifying) {
        StopNotification();
    }
    if (music_player_.IsBusy()) {
        StopMusic();
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    // Protocol start may have already raised MAIN_EVENT_ERROR. Do not replace
    // that alert with the "ready" UI/sound — the main loop can process both
    // events back-to-back because the activation task is lower priority.
    const bool has_error = !last_error_message_.empty();
    if (!startup_completed_) {
        startup_completed_ = true;
        pending_startup_wake_word_hint_ = !has_error;
    }
    if (!has_error) {
        auto display = Board::GetInstance().GetDisplay();
        std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    }

    // Release OTA object after activation is complete
    ota_.reset();
    // 启动时已检查过版本，首次定时检查延后一个周期。
    next_ota_check_seconds_ =
        esp_timer_get_time() / 1000000 + kOtaCheckIntervalSeconds;
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    if (!has_error) {
        Schedule([this]() {
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        });
    }
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    // "robot_2" is declared only by a board shipping its own pack: esp-vocat maps it to
    // neutral.eaf, and the rgb-matrix display special-cases the string. None of the shared
    // esp_emote_assets packs (320_240, 360_360, 1024_600) declares it, so on every board that
    // uses one this call fails, logs two errors and leaves whatever face was already up.
    // Fall back to an emotion all three packs do declare.
    if (!display->SetEmotion("robot_2")) {
        display->SetEmotion("neutral");
    }
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        auto check = ota_->CheckVersion();
        if (!check) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            const auto& err = check.error();
            char error_message[160];
            int error_message_length =
                snprintf(error_message, sizeof(error_message), "%s", err.ToString().c_str());
            if (error_message_length < 0 ||
                error_message_length >= static_cast<int>(sizeof(error_message))) {
                snprintf(error_message, sizeof(error_message), "%s", err.Message());
            }

            char buffer[320];
            int alert_message_length =
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED,
                         retry_delay, error_message);
            if (alert_message_length < 0 ||
                alert_message_length >= static_cast<int>(sizeof(buffer))) {
                snprintf(buffer, sizeof(buffer), "%s", err.Message());
            }
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ForceOtaCheck() {
    ESP_LOGI(TAG, "Force OTA check requested");
    Schedule([this]() { RunOtaCheck(/*force=*/true); });
}

void Application::PeriodicOtaCheck() {
    // 只在空闲态且无音频播放时检查，避免打断对话/播放。
    if (GetDeviceState() != kDeviceStateIdle || music_player_.IsBusy() ||
        notify_player_.IsBusy()) {
        return;
    }
    RunOtaCheck(/*force=*/false);
}

void Application::RunOtaCheck(bool force) {
    if (ota_upgrade_started_ || ota_check_in_progress_) {
        return;
    }
    if (!force && GetDeviceState() != kDeviceStateIdle) {
        return;
    }
    const int64_t now = esp_timer_get_time() / 1000000;
    if (!force && now < next_ota_check_seconds_) {
        return;
    }
    // 先占位，防止重复创建检查任务；真正的状态更新在任务完成后的主任务回调里。
    ota_check_in_progress_ = true;
    if (xTaskCreate(&Application::OtaCheckTaskEntry, "ota_check", 8192, this, 2, nullptr) !=
        pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA check task");
        ota_check_in_progress_ = false;
    }
}

void Application::OtaCheckTaskEntry(void* arg) {
    static_cast<Application*>(arg)->RunOtaCheckTask();
    vTaskDelete(nullptr);
}

void Application::RunOtaCheckTask() {
    // 使用局部 Ota 对象，避免与激活流程共享状态，也不依赖 ota_ 是否已释放。
    Ota ota;
    auto check = ota.CheckVersion();
    if (!check) {
        ESP_LOGW(TAG, "Periodic OTA check failed: %s", check.error().ToString().c_str());
        Schedule([this]() {
            ota_check_in_progress_ = false;
            if (ota_retry_backoff_seconds_ == 0) {
                ota_retry_backoff_seconds_ = 60;
            } else {
                ota_retry_backoff_seconds_ = std::min(ota_retry_backoff_seconds_ * 2, 3600);
            }
            next_ota_check_seconds_ = esp_timer_get_time() / 1000000 + ota_retry_backoff_seconds_;
            ESP_LOGW(TAG, "Next OTA check in %d s", ota_retry_backoff_seconds_);
        });
        return;
    }

    const bool has_new_version = ota.HasNewVersion();
    const std::string url = ota.GetFirmwareUrl();
    const std::string version = ota.GetFirmwareVersion();
    if (has_new_version) {
        ESP_LOGI(TAG, "Periodic OTA: new firmware %s available", version.c_str());
    }

    // 所有成员状态都在主任务更新，避免与主循环竞争。
    Schedule([this, has_new_version, url, version]() {
        ota_check_in_progress_ = false;
        ota_retry_backoff_seconds_ = 0;
        next_ota_check_seconds_ =
            esp_timer_get_time() / 1000000 + kOtaCheckIntervalSeconds;
        if (!has_new_version) {
            return;
        }
        // 检查期间用户可能开始对话/播放，避免此时打断。
        if (GetDeviceState() != kDeviceStateIdle || music_player_.IsBusy() ||
            notify_player_.IsBusy()) {
            ESP_LOGI(TAG, "Skip OTA upgrade: device is busy");
            return;
        }
        ota_upgrade_started_ = true;
        ota_upgrade_url_ = url;
        ota_upgrade_version_ = version;
        // 升级流程会播放提示音并通过 Schedule 更新进度，必须在独立任务执行，
        // 否则进度回调无法在主循环里运行。
        if (xTaskCreate(&Application::OtaUpgradeTaskEntry, "ota_upgrade", 8192, this, 3,
                        nullptr) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA upgrade task");
            ota_upgrade_started_ = false;
        }
    });
}

void Application::OtaUpgradeTaskEntry(void* arg) {
    auto* app = static_cast<Application*>(arg);
    const bool success = app->UpgradeFirmware(app->ota_upgrade_url_, app->ota_upgrade_version_);
    // 成功后 UpgradeFirmware 已重启，不会返回 true；失败时恢复允许再次尝试。
    app->Schedule([app, success]() {
        app->ota_upgrade_started_ = false;
        app->ota_upgrade_url_.clear();
        app->ota_upgrade_version_.clear();
        app->ota_retry_backoff_seconds_ = 0;
        app->next_ota_check_seconds_ =
            esp_timer_get_time() / 1000000 + kOtaCheckIntervalSeconds;
        if (!success) {
            ESP_LOGW(TAG, "Periodic OTA upgrade failed");
            // 升级失败时 UpgradeFirmware 不会复位状态，需回到空闲态，
            // 否则设备会卡在 Upgrading，后续定时检查也不会再触发。
            app->SetDeviceState(kDeviceStateIdle);
            app->DismissAlert();
        }
    });
    vTaskDelete(nullptr);
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "notify") == 0) {
            auto audio_url = cJSON_GetObjectItem(root, "audio_url");
            if (!cJSON_IsString(audio_url) || audio_url->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "Notify message requires audio_url");
                return;
            }

            std::vector<NotifySubtitle> subtitles;
            auto subtitles_json = cJSON_GetObjectItem(root, "subtitles");
            if (subtitles_json != nullptr && !cJSON_IsArray(subtitles_json)) {
                ESP_LOGW(TAG, "Notify subtitles must be an array");
                return;
            }
            if (cJSON_IsArray(subtitles_json)) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach (item, subtitles_json) {
                    auto start_ms = cJSON_GetObjectItem(item, "start_ms");
                    auto text = cJSON_GetObjectItem(item, "text");
                    if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
                        start_ms->valuedouble > std::numeric_limits<uint32_t>::max() ||
                        !cJSON_IsString(text)) {
                        ESP_LOGW(TAG, "Ignoring invalid notify subtitle");
                        continue;
                    }
                    subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                         .text = text->valuestring});
                }
            }

            Schedule([this, url = std::string(audio_url->valuestring),
                      subtitles = std::move(subtitles)]() mutable {
                StartNotification(std::move(url), std::move(subtitles));
            });
        } else if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            CJsonStringUniquePtr root_json(cJSON_PrintUnformatted(root));
            ESP_LOGI(TAG, "Received custom message: %s", root_json ? root_json.get() : "");
            if (cJSON_IsObject(payload)) {
                CJsonStringUniquePtr payload_json(cJSON_PrintUnformatted(payload));
                if (payload_json) {
                    Schedule(
                        [this, display, payload_str = std::string(payload_json.get())]() {
                            display->SetChatMessage("system", payload_str.c_str());
                        });
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();

    // 旁路 MQTT 控制通道：仅在 OTA 下发可选 "mqtt_control" 段时启用，
    // 与语音协议（protocol_）互相独立，用于 idle 状态下的远程触达。
    if (ota_ != nullptr && ota_->HasMqttControlConfig()) {
        mqtt_control_ = std::make_unique<MqttControl>();
        if (!mqtt_control_->Start()) {
            ESP_LOGW(TAG, "mqtt_control start failed");
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    last_error_message_.clear();
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateNotifying) {
        StopNotification();
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    // 音乐只在空闲态输出。进入其它状态（聆听/服务器 TTS/连接）时「暂停」而非
    // 「停止」：保留 HTTP 连接、解码器状态与播放位置，回到空闲后自动续播。
    // 这样用户说「暂停/继续」以及对话打断后的恢复都能真正起作用。
    //
    // 注意：这里必须真正 Stop 而不是 Pause。音乐走的是到流媒体服务的
    // 独立 HTTP 连接；Pause 会一直占着该连接与解码器缓冲，对话期间设备还要
    // 持续上传麦克风音频，两者叠加会挤占内存与网络，实测导致
    // 「EspSsl send timeout」无限重试、主循环假死。
    // 因此打断时释放音乐连接并记住位置，回到空闲后从该位置续播。
    if (new_state != kDeviceStateIdle && music_player_.IsBusy() &&
        !music_player_.IsPaused()) {
        music_paused_by_dialog_ = true;
        music_resume_position_ = music_player_.PositionSeconds();
        music_resume_url_ = current_music_url_;
        music_player_.Stop();
        audio_service_.ResetDecoder();
        ESP_LOGI(TAG, "Music stopped for dialog at %ds (releasing HTTP)",
                 music_resume_position_);
    }
    // 有待播放请求时，每个状态变化都尝试推进一次（Speaking -> Listening
    // 也需要处理，否则会一直停在 Listening 永远不播）。
    if (!pending_music_url_.empty()) {
        TryStartPendingMusic();
    }
    if (new_state == kDeviceStateIdle) {
        // 优先处理「Speaking 期间收到的播放请求」，它比打断续播更新。
        if (pending_music_url_.empty() && music_paused_by_dialog_) {
            // 对话结束：若用户没有显式暂停，则从打断位置续播。
            music_paused_by_dialog_ = false;
            if (!music_user_paused_ && !music_resume_url_.empty()) {
                std::string url = std::move(music_resume_url_);
                int position = music_resume_position_;
                music_resume_url_.clear();
                music_resume_position_ = 0;
                if (StartMusicNow(url)) {
                    if (position > 0) {
                        music_player_.Seek(position);
                    }
                    ESP_LOGI(TAG, "Music resumed from %ds after returning to idle", position);
                }
            } else {
                ESP_LOGI(TAG, "Music stays paused (user requested pause)");
            }
        } else if (!music_queue_.empty() && !music_player_.IsBusy()) {
            // 队列还有歌且当前无播放：播放下一首。
            PlayNextMusicFromQueue();
        }
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // Keep a just-raised network error visible. SetDeviceState(idle)
            // queues STATE_CHANGED after Alert(), and the idle handler would
            // otherwise wipe the status, emotion, and chat message.
            if (last_error_message_.empty()) {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();  // Clear messages first
                display->SetEmotion(
                    "neutral");  // Then set emotion (wechat mode checks child count)
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateNotifying:
            display->SetStatus(Lang::Strings::SPEAKING);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ShowStartupWakeWordHint() {
    // A coalesced activation/state event can queue another idle UI cleanup.
    // Wait for it before adding the hint, or ClearChatMessages() would erase it.
    if (!pending_startup_wake_word_hint_ ||
        (xEventGroupGetBits(event_group_) & MAIN_EVENT_STATE_CHANGED)) {
        return;
    }
    pending_startup_wake_word_hint_ = false;
    const auto state = GetDeviceState();
    const bool wake_word_running = audio_service_.IsWakeWordRunning();
    if (state != kDeviceStateIdle || !last_error_message_.empty() || !wake_word_running) {
        ESP_LOGI(TAG, "Startup wake word hint skipped (state=%d, wake word running=%d, error=%d)",
                 static_cast<int>(state), wake_word_running, !last_error_message_.empty());
        return;
    }

    const auto wake_words = audio_service_.GetWakeWords();
    std::string words;
    for (auto it = wake_words.begin(); it != wake_words.end(); ++it) {
        if (it->empty() || std::find(wake_words.begin(), it, *it) != it) {
            continue;
        }
        if (!words.empty()) {
            words += " / ";
        }
        words += *it;
    }
    if (words.empty()) {
        ESP_LOGW(TAG, "Startup wake word hint skipped: no available wake words");
        return;
    }
    std::string message = Lang::Strings::WAKE_WORD_HINT;
    const auto placeholder = message.find("%s");
    if (placeholder == std::string::npos) {
        ESP_LOGE(TAG, "Startup wake word hint skipped: invalid message template");
        return;
    }
    message.replace(placeholder, 2, words);
    if (startup_wake_word_hint_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto app = static_cast<Application*>(arg);
                    xEventGroupSetBits(app->event_group_, MAIN_EVENT_STARTUP_HINT_TIMEOUT);
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "startup_hint",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &startup_wake_word_hint_timer_));
    }
    ESP_LOGI(TAG, "Startup wake word hint: %s", message.c_str());
    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", message.c_str());
    startup_wake_word_hint_message_ = std::move(message);
    ESP_ERROR_CHECK(esp_timer_start_once(startup_wake_word_hint_timer_, 5000 * 1000));
}

void Application::HideStartupWakeWordHint() {
    if (startup_wake_word_hint_message_.empty()) {
        return;
    }
    // A conversation or alert may have replaced the hint while its timer was running.
    Board::GetInstance().GetDisplay()->RemoveChatMessage("assistant",
                                                         startup_wake_word_hint_message_.c_str());
    startup_wake_word_hint_message_.clear();
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles) {
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
    audio_service_.ReleaseWakeWordResources();
    while (audio_service_.PopPacketFromSendQueue()) {
        // Discard microphone audio left over from a previous conversation.
    }

    if (!SetDeviceState(kDeviceStateNotifying)) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    audio_service_.ResetDecoder();
    uint32_t playback_id = ++notification_playback_id_;
    if (playback_id == 0) {
        playback_id = ++notification_playback_id_;
    }
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);

    bool started = notify_player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [this](uint32_t id, const std::string& text) {
            Schedule([this, id, text]() {
                if (GetDeviceState() == kDeviceStateNotifying && notification_playback_id_ == id) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", text.c_str());
                }
            });
        },
        [this](uint32_t id, bool success) {
            Schedule([this, id, success]() { HandleNotificationFinished(id, success); });
        });

    if (!started) {
        ESP_LOGE(TAG, "Failed to start notification playback");
        StopNotification();
    }
}

void Application::StopNotification() {
    notify_player_.Stop();
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (GetDeviceState() == kDeviceStateNotifying) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleNotificationFinished(uint32_t playback_id, bool success) {
    if (GetDeviceState() != kDeviceStateNotifying || notification_playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
    StopNotification();
}

bool Application::StartMusic(std::string audio_url) {
    // 语义等同 EnqueueMusic：追加到播放队列，若当前空闲则立即开始。
    return EnqueueMusic(std::move(audio_url));
}

void Application::StopMusic() {
    // 用户明确停止：清空整个队列与历史，而不只是当前曲目。
    music_queue_.clear();
    music_history_.clear();
    current_music_url_.clear();
    // 清掉「刚播完的曲目」：否则单曲循环模式会把这首重新入队再播。
    finished_music_url_.clear();
    // 清掉循环模式，避免 Stop 之后又被循环逻辑拉起来。
    music_repeat_mode_ = MusicRepeatMode::kOff;
    music_paused_by_dialog_ = false;
    music_user_paused_ = false;
    // 用户明确停止：作废「对话打断后续播」的位置与待播请求，
    // 避免回到空闲又自己响起来。
    music_resume_url_.clear();
    music_resume_position_ = 0;
    pending_music_url_.clear();
    // 队列与历史已清空，worker 退出后的 HandleMusicFinished 不会续播，
    // 因此无需额外的「主动停止」标记（该标记在切歌时容易残留）。
    music_player_.Stop();
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    // 回到低功耗；仅在空闲态时生效，避免干扰正在进行的对话状态。
    if (GetDeviceState() == kDeviceStateIdle) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }
    ESP_LOGI(TAG, "Music playback stopped");
}

bool Application::PlayMusicNow(std::string audio_url) {
    if (audio_url.rfind("http://", 0) != 0 && audio_url.rfind("https://", 0) != 0) {
        ESP_LOGW(TAG, "Invalid music url: %s", audio_url.c_str());
        return false;
    }
    Schedule([this, url = std::move(audio_url)]() mutable {
        // 立即播放：清空队列与历史，并替换当前曲目。
        music_queue_.clear();
        music_history_.clear();
        pending_music_url_.clear();
        // 丢弃旧曲目残留在播放队列里的 PCM，避免串音。
        audio_service_.ResetDecoder();
        // 唤醒对话期间通常处于 Speaking（播报“正在播放…”），此刻起播会被
        // 状态切换立刻打断。改为记录待播地址，等回到 Idle 再开始。
        if (GetDeviceState() != kDeviceStateIdle) {
            ESP_LOGI(TAG, "Music request deferred until idle (state=%d)",
                     static_cast<int>(GetDeviceState()));
            pending_music_url_ = std::move(url);
            // 旧曲目仍在播的话先停掉，避免与新请求抢音频输出。
            if (music_player_.IsBusy()) {
                music_player_.Stop();
            }
            return;
        }
        // MusicPlayer::Start 在旧 worker 仍在运行时会自动排队换曲，
        // 不需要先 Stop（先 Stop 再 Start 会因 worker_running_ 而失败）。
        if (!StartMusicNow(url)) {
            ESP_LOGE(TAG, "Failed to start music");
            StopMusic();
        }
    });
    return true;
}

void Application::TryStartPendingMusic() {
    if (pending_music_url_.empty() || starting_pending_music_) {
        return;
    }
    if (music_player_.IsBusy() || notify_player_.IsBusy()) {
        return;
    }

    const auto state = GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        // TTS 还在播报（“正在播放…”），等它结束。
        return;
    }
    if (state == kDeviceStateListening) {
        // 自动聆听模式下 TTS 结束后会停在 Listening，不会自动回 Idle。
        // 注意：WebSocket 的 CloseAudioChannel() 只是重置传输层，
        // 不会触发 OnAudioChannelClosed 回调，所以这里必须显式切状态，
        // 否则待播请求会永远卡在 Listening。
        ESP_LOGI(TAG, "Deferred music: leaving Listening to start playback");
        if (protocol_ != nullptr && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        starting_pending_music_ = true;
        const bool switched = SetDeviceState(kDeviceStateIdle);
        starting_pending_music_ = false;
        if (!switched) {
            return;
        }
        // 状态机是在本调用内同步通知监听器的，那一次回调被防重入标志挡掉了，
        // 这里必须主动再推进一次，否则待播请求会永远停在队列里。
        Schedule([this]() { TryStartPendingMusic(); });
        return;
    }
    if (state != kDeviceStateIdle) {
        return;
    }

    std::string url = std::move(pending_music_url_);
    pending_music_url_.clear();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.EnableVoiceProcessing(false);
    ESP_LOGI(TAG, "Starting deferred music request");
    if (!StartMusicNow(url)) {
        ESP_LOGE(TAG, "Failed to start deferred music");
        StopMusic();
    }
}

void Application::HandleMusicFinished(uint32_t playback_id, bool success) {
    if (music_playback_id_ != playback_id) {
        return;
    }
    audio_service_.ResetDecoder();
    current_music_url_.clear();

    ESP_LOGI(TAG, "Music playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");

    // 单曲循环：把刚播完的这首重新放回队首。
    if (music_repeat_mode_ == MusicRepeatMode::kOne && !finished_music_url_.empty()) {
        music_queue_.push_front(finished_music_url_);
    }

    if (!music_queue_.empty()) {
        PlayNextMusicFromQueue();
        return;
    }

    // 队列已空：列表循环则把整段历史重新排入队列。
    if (music_repeat_mode_ == MusicRepeatMode::kList && !music_history_.empty()) {
        for (const auto& u : music_history_) {
            music_queue_.push_back(u);
        }
        music_history_.clear();
        ESP_LOGI(TAG, "Music repeat list: replaying %u songs",
                 static_cast<unsigned>(music_queue_.size()));
        PlayNextMusicFromQueue();
        return;
    }

    // 顺序播放结束，收尾并回到低功耗。
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    if (GetDeviceState() == kDeviceStateIdle) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }
    ESP_LOGI(TAG, "Music queue finished");
}

void Application::PlayNextMusicFromQueue() {
    if (music_queue_.empty()) {
        return;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        return;
    }

    std::string url = std::move(music_queue_.front());
    music_queue_.pop_front();
    // 记录上一首，便于「上一曲」。
    if (!current_music_url_.empty()) {
        music_history_.push_back(current_music_url_);
        if (music_history_.size() > kMusicHistoryMax) {
            music_history_.pop_front();
        }
    }
    // 不清空 current_music_url_ 也行：StartMusicNow 会覆盖它。
    // 若旧 worker 仍在运行，MusicPlayer::Start 会自动排队换曲。
    if (!StartMusicNow(url)) {
        ESP_LOGE(TAG, "Failed to start next music, dropping it");
        // 跳过这一首，继续尝试后面的。
        Schedule([this]() { PlayNextMusicFromQueue(); });
        return;
    }
}

bool Application::StartMusicNow(const std::string& url) {
    current_music_url_ = url;
    finished_music_url_ = url;
    music_user_paused_ = false;
    music_paused_by_dialog_ = false;
    // 新的播放请求作废之前的续播位置。
    music_resume_url_.clear();
    music_resume_position_ = 0;

    uint32_t playback_id = ++music_playback_id_;
    if (playback_id == 0) {
        playback_id = ++music_playback_id_;
    }

    bool started = music_player_.Start(
        url, playback_id,
        [this](uint32_t id, bool success) {
            Schedule([this, id, success]() { HandleMusicFinished(id, success); });
        });
    if (!started) {
        return false;
    }
    ESP_LOGI(TAG, "Music playback started, id=%lu, remaining=%u",
             static_cast<unsigned long>(playback_id),
             static_cast<unsigned>(music_queue_.size()));
    return true;
}

void Application::NextMusic() {
    Schedule([this]() {
        // 用户明确切歌：作废「对话打断后续播」的位置，
        // 否则回到 idle 时会被续播逻辑抢走，表现为「下一首不播放」。
        music_paused_by_dialog_ = false;
        music_resume_url_.clear();
        music_resume_position_ = 0;
        if (music_queue_.empty()) {
            // 队列为空说明当前是「单曲播放」（play_url），没有已知的下一首。
            // 此时绝不能 StopMusic：那会把音乐直接停掉，表现为「下一首不播放」。
            // 交由上层（语音助手重新点歌）决定播什么，这里保持现状。
            ESP_LOGW(TAG, "NextMusic: queue empty (single-track mode), keep playing");
            return;
        }
        if (GetDeviceState() != kDeviceStateIdle) {
            // 对话/播报中：先停掉当前曲目（释放 HTTP），回到空闲态时由
            // HandleStateChangedEvent 接着播放队列里的下一首。
            if (!current_music_url_.empty()) {
                music_history_.push_back(current_music_url_);
                if (music_history_.size() > kMusicHistoryMax) {
                    music_history_.pop_front();
                }
                current_music_url_.clear();
            }
            music_player_.Stop();
            audio_service_.ResetDecoder();
            ESP_LOGI(TAG, "NextMusic deferred until idle, queue=%u",
                     static_cast<unsigned>(music_queue_.size()));
            return;
        }
        // 直接把下一首交给 Start：MusicPlayer 会在旧 worker 退出后自动接续，
        // 不要先 Stop（会导致 Start 因 worker_running_ 失败而卡住）。
        audio_service_.ResetDecoder();
        PlayNextMusicFromQueue();
    });
}

void Application::PrevMusic() {
    Schedule([this]() {
        // 用户明确切歌：作废打断续播状态。
        music_paused_by_dialog_ = false;
        music_resume_url_.clear();
        music_resume_position_ = 0;
        if (music_history_.empty()) {
            ESP_LOGW(TAG, "PrevMusic: no history");
            return;
        }
        if (GetDeviceState() != kDeviceStateIdle) {
            ESP_LOGW(TAG, "PrevMusic: device not idle");
            return;
        }
        // 当前曲目若存在，放回队首，便于再「下一曲」切回来。
        if (!current_music_url_.empty()) {
            music_queue_.push_front(current_music_url_);
        }
        std::string url = music_history_.back();
        music_history_.pop_back();
        current_music_url_.clear();
        audio_service_.ResetDecoder();
        // Start 内部会排队换曲，无需先 Stop。
        if (!StartMusicNow(url)) {
            ESP_LOGE(TAG, "Failed to start previous music");
        }
    });
}

int Application::MusicQueueLength() const {
    int n = static_cast<int>(music_queue_.size());
    if (music_player_.IsActive()) {
        n += 1;
    }
    return n;
}

void Application::PauseMusic() {
    music_user_paused_ = true;
    music_paused_by_dialog_ = false;
    music_player_.Pause();
    ESP_LOGI(TAG, "Music paused by user");
}

void Application::ResumeMusic() {
    music_user_paused_ = false;
    music_player_.Resume();
    ESP_LOGI(TAG, "Music resumed by user");
}

bool Application::SeekMusic(int seconds) { return music_player_.Seek(seconds); }

bool Application::EnqueueMusic(std::string audio_url) {
    if (audio_url.rfind("http://", 0) != 0 && audio_url.rfind("https://", 0) != 0) {
        ESP_LOGW(TAG, "Invalid music url: %s", audio_url.c_str());
        return false;
    }
    Schedule([this, url = std::move(audio_url)]() mutable {
        music_queue_.push_back(std::move(url));
        ESP_LOGI(TAG, "Music enqueued, queue length=%u",
                 static_cast<unsigned>(music_queue_.size()));
        // 若当前空闲且没有正在播放，立刻开始。
        if (!music_player_.IsBusy() && GetDeviceState() == kDeviceStateIdle) {
            PlayNextMusicFromQueue();
        }
    });
    return true;
}

bool Application::SkipMusic(int delta_seconds) {
    int pos = music_player_.PositionSeconds() + delta_seconds;
    if (pos < 0) {
        pos = 0;
    }
    return music_player_.Seek(pos);
}

void Application::SetMusicRepeatMode(MusicRepeatMode mode) {
    music_repeat_mode_ = mode;
    const char* name = mode == MusicRepeatMode::kOne ? "one"
                       : mode == MusicRepeatMode::kList ? "list"
                                                        : "off";
    ESP_LOGI(TAG, "Music repeat mode: %s", name);
}

bool Application::IsMusicPaused() const { return music_player_.IsPaused(); }

int Application::MusicPositionSeconds() const { return music_player_.PositionSeconds(); }

int Application::MusicDurationSeconds() const { return music_player_.DurationSeconds(); }

bool Application::IsMusicPlaying() const { return music_player_.IsActive(); }

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateNotifying) {
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        if (GetDeviceState() == kDeviceStateNotifying) {
            StopNotification();
        }
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
