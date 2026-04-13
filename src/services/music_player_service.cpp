#include "services/music_player_service.h"
#include "cache.h"

#include <dpp/dpp.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <regex>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace services {

// PCM frame size for DPP: 2 channels * 2 bytes * 2880 samples = 11520 bytes
static constexpr size_t PCM_FRAME_SIZE = 11520;
static constexpr size_t MAX_HISTORY_SIZE = 10;

// Build ffmpeg aecho filter string with N taps spread over max_ms, exponential decay.
static std::string build_echo_filter(int taps, int max_ms, double start_gain = 0.6, double end_gain = 0.08) {
    std::string delays, decays;
    double ratio = (taps > 1) ? std::pow(end_gain / start_gain, 1.0 / (taps - 1)) : 1.0;
    for (int i = 0; i < taps; ++i) {
        if (i > 0) { delays += '|'; decays += '|'; }
        int ms = static_cast<int>(std::round((i + 1.0) / taps * max_ms));
        double gain = start_gain * std::pow(ratio, i);
        delays += std::to_string(ms);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.4f", gain);
        decays += buf;
    }
    return "aecho=1.0:0.9:" + delays + ":" + decays;
}

// Detect BPM from audio source using onset energy autocorrelation.
// For URLs (YouTube etc.) uses yt-dlp | ffmpeg pipeline; for local files, ffmpeg only.
// Returns BPM in [60, 200] or 0 on failure.
static int detect_bpm(const std::string& source, int start_seconds = 0,
                       const std::string& cookies_path = "") {
    constexpr int SAMPLE_RATE = 44100;
    constexpr int DURATION = 15;
    constexpr int WIN = 1024;
    constexpr int HOP = 512;
    constexpr int MIN_BPM = 60;
    constexpr int MAX_BPM = 200;

    bool is_url = source.find("://") != std::string::npos;
    int output_fd = -1;
    pid_t ytdlp_pid = -1, ffmpeg_pid = -1;

    if (is_url) {
        // yt-dlp → pipe → ffmpeg → pipe → us
        int yt_to_ff[2], ff_to_us[2];
        if (pipe(yt_to_ff) < 0) return 0;
        if (pipe(ff_to_us) < 0) { close(yt_to_ff[0]); close(yt_to_ff[1]); return 0; }

        ytdlp_pid = fork();
        if (ytdlp_pid < 0) {
            close(yt_to_ff[0]); close(yt_to_ff[1]);
            close(ff_to_us[0]); close(ff_to_us[1]);
            return 0;
        }
        if (ytdlp_pid == 0) {
            close(yt_to_ff[0]); close(ff_to_us[0]); close(ff_to_us[1]);
            dup2(yt_to_ff[1], STDOUT_FILENO); close(yt_to_ff[1]);
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }

            std::vector<const char*> args = {"yt-dlp", "-f", "bestaudio", "-o", "-"};
            if (!cookies_path.empty()) { args.push_back("--cookies"); args.push_back(cookies_path.c_str()); }
            args.push_back(source.c_str());
            args.push_back(nullptr);
            execvp("yt-dlp", const_cast<char* const*>(args.data()));
            _exit(1);
        }
        close(yt_to_ff[1]);

        ffmpeg_pid = fork();
        if (ffmpeg_pid < 0) {
            close(yt_to_ff[0]); close(ff_to_us[0]); close(ff_to_us[1]);
            kill(ytdlp_pid, SIGKILL); waitpid(ytdlp_pid, nullptr, 0);
            return 0;
        }
        if (ffmpeg_pid == 0) {
            close(ff_to_us[0]);
            dup2(yt_to_ff[0], STDIN_FILENO); close(yt_to_ff[0]);
            dup2(ff_to_us[1], STDOUT_FILENO); close(ff_to_us[1]);
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }

            std::string ss_str;
            std::vector<const char*> args = {"ffmpeg"};
            if (start_seconds > 0) {
                ss_str = std::to_string(start_seconds);
                args.push_back("-ss"); args.push_back(ss_str.c_str());
            }
            args.insert(args.end(), {"-i", "pipe:0", "-t", "15",
                "-f", "s16le", "-ar", "44100", "-ac", "1", "pipe:1", nullptr});
            execvp("ffmpeg", const_cast<char* const*>(args.data()));
            _exit(1);
        }
        close(yt_to_ff[0]); close(ff_to_us[1]);
        output_fd = ff_to_us[0];
    } else {
        // Local file: ffmpeg only
        int pipefd[2];
        if (pipe(pipefd) < 0) return 0;

        ffmpeg_pid = fork();
        if (ffmpeg_pid < 0) { close(pipefd[0]); close(pipefd[1]); return 0; }
        if (ffmpeg_pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }

            std::string ss_str;
            std::vector<const char*> args = {"ffmpeg"};
            if (start_seconds > 0) {
                ss_str = std::to_string(start_seconds);
                args.push_back("-ss"); args.push_back(ss_str.c_str());
            }
            args.push_back("-i"); args.push_back(source.c_str());
            args.insert(args.end(), {"-t", "15", "-f", "s16le",
                "-ar", "44100", "-ac", "1", "pipe:1", nullptr});
            execvp("ffmpeg", const_cast<char* const*>(args.data()));
            _exit(1);
        }
        close(pipefd[1]);
        output_fd = pipefd[0];
    }

    // Read PCM data with timeout
    const size_t max_samples = SAMPLE_RATE * DURATION;
    std::vector<int16_t> pcm;
    pcm.reserve(max_samples);

    std::array<char, 32768> buf;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

    while (pcm.size() < max_samples) {
        int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        if (remaining_ms <= 0) break;

        struct pollfd pfd{output_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, std::min(remaining_ms, 1000));
        if (ret <= 0) break;

        ssize_t n = read(output_fd, buf.data(), buf.size());
        if (n <= 0) break;

        const int16_t* samples = reinterpret_cast<const int16_t*>(buf.data());
        size_t count = n / sizeof(int16_t);
        for (size_t i = 0; i < count && pcm.size() < max_samples; ++i)
            pcm.push_back(samples[i]);
    }

    close(output_fd);
    if (ytdlp_pid > 0) { kill(ytdlp_pid, SIGKILL); waitpid(ytdlp_pid, nullptr, 0); }
    if (ffmpeg_pid > 0) { kill(ffmpeg_pid, SIGKILL); waitpid(ffmpeg_pid, nullptr, 0); }

    if (pcm.size() < static_cast<size_t>(SAMPLE_RATE * 4)) {
        spdlog::warn("[music] BPM detection: insufficient audio ({} samples)", pcm.size());
        return 0;
    }

    // Compute energy per window
    size_t num_frames = (pcm.size() - WIN) / HOP + 1;
    std::vector<double> energy(num_frames);
    for (size_t f = 0; f < num_frames; ++f) {
        double sum = 0;
        for (int j = 0; j < WIN; ++j) {
            double s = pcm[f * HOP + j] / 32768.0;
            sum += s * s;
        }
        energy[f] = sum;
    }

    // Onset function: half-wave rectified first difference
    std::vector<double> onset(num_frames > 0 ? num_frames - 1 : 0);
    for (size_t i = 0; i < onset.size(); ++i) {
        double diff = energy[i + 1] - energy[i];
        onset[i] = diff > 0 ? diff : 0;
    }

    if (onset.size() < 100) return 0;

    // Autocorrelation in BPM range (normalized by overlap count)
    int min_lag = static_cast<int>(60.0 * SAMPLE_RATE / (MAX_BPM * HOP));
    int max_lag = static_cast<int>(60.0 * SAMPLE_RATE / (MIN_BPM * HOP));
    max_lag = std::min(max_lag, static_cast<int>(onset.size() / 2));

    if (min_lag >= max_lag) return 0;

    std::vector<double> corr(max_lag + 1, 0.0);
    for (int lag = min_lag; lag <= max_lag; ++lag) {
        double sum = 0;
        size_t count = onset.size() - lag;
        for (size_t i = 0; i < count; ++i)
            sum += onset[i] * onset[i + lag];
        corr[lag] = sum / static_cast<double>(count);  // normalize
    }

    // Find best peak
    double best_val = 0;
    int best_lag = 0;
    for (int lag = min_lag; lag <= max_lag; ++lag) {
        if (corr[lag] > best_val) {
            best_val = corr[lag];
            best_lag = lag;
        }
    }

    if (best_lag <= 0 || best_val <= 0) return 0;

    // Octave correction: if double-lag (half BPM) has a strong peak, prefer it
    // This prevents detecting 2x the real BPM
    int double_lag = best_lag * 2;
    if (double_lag <= max_lag && corr[double_lag] > best_val * 0.75) {
        spdlog::debug("[music] BPM octave correction: lag {} (corr {:.4f}) → {} (corr {:.4f})",
                      best_lag, best_val, double_lag, corr[double_lag]);
        best_lag = double_lag;
        best_val = corr[double_lag];
    }

    // Also check half-lag (double BPM) — if it's much stronger, prefer it
    int half_lag = best_lag / 2;
    if (half_lag >= min_lag && corr[half_lag] > best_val * 1.5) {
        spdlog::debug("[music] BPM half correction: lag {} → {} (corr {:.4f} >> {:.4f})",
                      best_lag, half_lag, corr[half_lag], best_val);
        best_lag = half_lag;
    }

    int bpm = static_cast<int>(std::round(60.0 * SAMPLE_RATE / (best_lag * HOP)));
    bpm = std::clamp(bpm, MIN_BPM, MAX_BPM);

    spdlog::info("[music] Detected BPM: {} (lag={}, samples={}, corr={:.4f})",
                 bpm, best_lag, pcm.size(), best_val);
    return bpm;
}

// Copy cookies file to a temp path so yt-dlp cannot corrupt the original.
// If the original is empty/corrupt, auto-restore from .bak backup.
// Returns empty string if no cookies or copy fails.
static std::string make_cookies_copy(const std::string& cookies_path) {
    if (cookies_path.empty()) return "";

    std::string effective_path = cookies_path;
    std::string backup_path = cookies_path + ".bak";

    // If original is missing or empty, restore from backup
    if (!std::filesystem::exists(effective_path) ||
        std::filesystem::file_size(effective_path) == 0) {
        if (std::filesystem::exists(backup_path) &&
            std::filesystem::file_size(backup_path) > 0) {
            spdlog::warn("[music] Cookies file empty/missing, restoring from backup");
            try {
                std::filesystem::copy_file(backup_path, effective_path,
                    std::filesystem::copy_options::overwrite_existing);
            } catch (const std::exception& e) {
                spdlog::error("[music] Failed to restore cookies from backup: {}", e.what());
                return "";
            }
        } else {
            return "";
        }
    }

    try {
        auto tmp = std::filesystem::temp_directory_path() /
                   ("patchouli_cookies_" + std::to_string(getpid()) + "_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
        std::filesystem::copy_file(effective_path, tmp, std::filesystem::copy_options::overwrite_existing);
        return tmp.string();
    } catch (const std::exception& e) {
        spdlog::warn("[music] Failed to copy cookies: {}", e.what());
        return "";
    }
}

// Parse start time from URL parameters (?t=120, &t=1h2m3s, ?start=90, etc.)
static int parse_start_time(const std::string& url) {
    // Look for t= or start= parameter
    for (const auto& param : {"t=", "start="}) {
        auto pos = url.find(param);
        if (pos == std::string::npos) continue;
        // Make sure it's a parameter (preceded by ? or &)
        if (pos > 0 && url[pos - 1] != '?' && url[pos - 1] != '&') continue;

        pos += std::strlen(param);
        std::string val;
        while (pos < url.size() && url[pos] != '&' && url[pos] != '#') {
            val += url[pos++];
        }
        if (val.empty()) continue;

        // Try formats: plain seconds, or 1h2m3s
        int total = 0;
        if (val.find_first_of("hms") != std::string::npos) {
            // Parse 1h2m3s format
            int num = 0;
            for (char c : val) {
                if (c >= '0' && c <= '9') {
                    num = num * 10 + (c - '0');
                } else if (c == 'h') {
                    total += num * 3600; num = 0;
                } else if (c == 'm') {
                    total += num * 60; num = 0;
                } else if (c == 's') {
                    total += num; num = 0;
                }
            }
            total += num; // trailing number without suffix = seconds
        } else {
            // Plain seconds
            try { total = std::stoi(val); } catch (...) {}
        }
        return total;
    }
    return 0;
}

MusicPlayerService::MusicPlayerService(dpp::cluster& bot, const std::string& cookies_path)
    : bot_(bot), cookies_path_(cookies_path) {
    // Auto-detect cookies file if none provided
    if (cookies_path_.empty() && std::filesystem::exists("data/cookies.txt")) {
        cookies_path_ = "data/cookies.txt";
        spdlog::info("[music] Auto-detected cookies file: {}", cookies_path_);
    }
    spdlog::info("[music] MusicPlayerService initialized (cookies={})",
                 cookies_path_.empty() ? "none" : cookies_path_);
}

void MusicPlayerService::set_cookies_path(const std::string& path) {
    std::lock_guard lock(cookies_mutex_);
    cookies_path_ = path;
    spdlog::info("[music] Cookies path updated: {}", path.empty() ? "none" : path);
}

std::string MusicPlayerService::get_cookies_path() const {
    std::lock_guard lock(cookies_mutex_);
    return cookies_path_;
}

void MusicPlayerService::set_on_state_change(StateChangeCallback cb) {
    std::lock_guard lock(callback_mutex_);
    on_state_change_ = std::move(cb);
}

void MusicPlayerService::notify_state_change(uint64_t guild_id) {
    std::lock_guard lock(callback_mutex_);
    if (on_state_change_) on_state_change_(guild_id);
}

MusicPlayerService::~MusicPlayerService() {
    shutdown();
}

void MusicPlayerService::shutdown() {
    if (shutting_down_.exchange(true)) return;
    spdlog::info("[music] Shutting down...");

    // Join restore thread first (it checks shutting_down_ / stop_token)
    if (restore_thread_.joinable()) {
        restore_thread_.request_stop();
        restore_thread_.join();
    }

    std::lock_guard lock(players_mutex_);

    // Save all player states before cleanup
    for (auto& [guild_id, player] : players_) {
        std::lock_guard pl(player->mutex);
        save_state_locked(*player, dpp::snowflake(guild_id));
    }

    for (auto& [guild_id, player] : players_) {
        {
            std::lock_guard pl(player->mutex);
            player->state = PlaybackState::Stopped;
            player->has_work = false;
            kill_bg_pipeline(*player);
            kill_subprocesses(*player);
        }
        player->work_cv.notify_all();
        if (player->worker.joinable()) {
            player->worker.request_stop();
        }
    }

    // Wait for workers to finish
    for (auto& [guild_id, player] : players_) {
        if (player->worker.joinable()) {
            player->worker.join();
        }
        disconnect_voice(dpp::snowflake(guild_id));
    }
    players_.clear();
    spdlog::info("[music] Shutdown complete");
}

MusicPlayerService::GuildPlayer& MusicPlayerService::get_or_create_player(dpp::snowflake guild_id) {
    std::lock_guard lock(players_mutex_);
    auto it = players_.find(guild_id);
    if (it == players_.end()) {
        auto player = std::make_unique<GuildPlayer>();
        auto& ref = *player;

        // Start worker thread for this guild
        ref.worker = std::jthread([this, gid = guild_id](std::stop_token st) {
            worker_loop(st, gid);
        });

        players_[guild_id] = std::move(player);
        return ref;
    }
    return *it->second;
}

MusicPlayerService::PlayResult MusicPlayerService::play(
    dpp::snowflake guild_id, dpp::snowflake channel_id,
    const std::string& url, const std::string& requester)
{
    PlayResult result;

    if (shutting_down_) {
        result.error = "Service is shutting down";
        return result;
    }

    // Fetch metadata first (blocking, but on API thread)
    spdlog::info("[music] Fetching metadata for: {}", url);
    auto meta = fetch_metadata(url);
    if (!meta) {
        result.error = "Failed to fetch video info. Check the URL.";
        return result;
    }

    auto& player = get_or_create_player(guild_id);
    {
    std::lock_guard lock(player.mutex);

    player.stopping = false;

    // Set the voice channel and connect if needed
    if (!player.voice_ready || player.voice_channel_id != channel_id) {
        player.voice_channel_id = channel_id;
        player.voice_ready = false;
        player.last_error.clear();

        spdlog::info("[music] Connecting to voice channel {} in guild {}", channel_id, guild_id);

        // Connect to voice via shard — DPP will fire on_voice_ready when done
        auto* shard = get_shard_for_guild(guild_id);
        if (shard) {
            // Always disconnect first to clear stale DPP internal state
            auto* existing = shard->get_voice(guild_id);
            if (existing) {
                spdlog::info("[music] Clearing existing voice connection for guild {}", guild_id);
                shard->disconnect_voice(guild_id);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            spdlog::info("[music] Calling connect_voice(guild={}, channel={})", guild_id, channel_id);
            shard->connect_voice(guild_id, channel_id, false, false);
        } else {
            spdlog::error("[music] No shard available for voice connection");
        }
    } else {
        spdlog::info("[music] Voice already ready for channel {}", channel_id);
    }

    meta->requester = requester;

    if (player.state == PlaybackState::Playing || player.state == PlaybackState::Paused) {
        // Queue the track
        player.queue.push_back(*meta);
        result.success = true;
        result.title = meta->title;
        result.queue_position = static_cast<int>(player.queue.size());
        spdlog::info("[music] Queued '{}' at position {} for guild {}",
                     meta->title, result.queue_position, guild_id);
    } else {
        // Start playing immediately
        player.queue.push_back(*meta);
        player.has_work = true;
        player.work_cv.notify_all();

        result.success = true;
        result.title = meta->title;
        result.queue_position = 0;
        spdlog::info("[music] Starting playback of '{}' in guild {}", meta->title, guild_id);
    }

    save_state_locked(player, guild_id);
    }  // release player.mutex

    if (result.success) {
        save_guild_list();
        notify_state_change(guild_id);
    }
    return result;
}

MusicPlayerService::PlayResult MusicPlayerService::play(
    dpp::snowflake guild_id, dpp::snowflake channel_id,
    const TrackInfo& track_info)
{
    PlayResult result;

    if (shutting_down_) {
        result.error = "Service is shutting down";
        return result;
    }

    spdlog::info("[music] Playing pre-built track: '{}'", track_info.title);

    auto& player = get_or_create_player(guild_id);
    {
    std::lock_guard lock(player.mutex);

    player.stopping = false;

    // Set the voice channel and connect if needed
    if (!player.voice_ready || player.voice_channel_id != channel_id) {
        player.voice_channel_id = channel_id;
        player.voice_ready = false;
        player.last_error.clear();

        spdlog::info("[music] Connecting to voice channel {} in guild {}", channel_id, guild_id);

        auto* shard = get_shard_for_guild(guild_id);
        if (shard) {
            auto* existing = shard->get_voice(guild_id);
            if (existing) {
                spdlog::info("[music] Clearing existing voice connection for guild {}", guild_id);
                shard->disconnect_voice(guild_id);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            spdlog::info("[music] Calling connect_voice(guild={}, channel={})", guild_id, channel_id);
            shard->connect_voice(guild_id, channel_id, false, false);
        } else {
            spdlog::error("[music] No shard available for voice connection");
        }
    } else {
        spdlog::info("[music] Voice already ready for channel {}", channel_id);
    }

    if (player.state == PlaybackState::Playing || player.state == PlaybackState::Paused) {
        player.queue.push_back(track_info);
        result.success = true;
        result.title = track_info.title;
        result.queue_position = static_cast<int>(player.queue.size());
        spdlog::info("[music] Queued '{}' at position {} for guild {}",
                     track_info.title, result.queue_position, guild_id);
    } else {
        player.queue.push_back(track_info);
        player.has_work = true;
        player.work_cv.notify_all();

        result.success = true;
        result.title = track_info.title;
        result.queue_position = 0;
        spdlog::info("[music] Starting playback of '{}' in guild {}", track_info.title, guild_id);
    }

    save_state_locked(player, guild_id);
    }  // release player.mutex

    if (result.success) {
        save_guild_list();
        notify_state_change(guild_id);
    }
    return result;
}

bool MusicPlayerService::skip(dpp::snowflake guild_id) {
    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (player.state != PlaybackState::Playing && player.state != PlaybackState::Paused) {
            return false;
        }

        spdlog::info("[music] Skipping track in guild {}", guild_id);
        kill_bg_pipeline(player);
        kill_subprocesses(player);
    }
    // Worker loop will pick up the next track; save after locks released
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::stop(dpp::snowflake guild_id) {
    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        spdlog::info("[music] Stopping playback in guild {}", guild_id);
        player.stopping = true;
        player.state = PlaybackState::Stopped;
        player.queue.clear();
        player.now_playing.reset();
        kill_bg_pipeline(player);
        kill_subprocesses(player);
    }

    // Disconnect from voice outside the lock to avoid potential deadlock
    disconnect_voice(guild_id);
    // Save stopped state (queue cleared, history preserved)
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::pause(dpp::snowflake guild_id) {
    bool changed = false;
    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (player.state == PlaybackState::Playing) {
            player.state = PlaybackState::Paused;
            spdlog::info("[music] Paused in guild {}", guild_id);
            changed = true;
        } else if (player.state == PlaybackState::Paused) {
            player.state = PlaybackState::Playing;
            spdlog::info("[music] Resumed in guild {}", guild_id);
            changed = true;
        }
    }
    if (changed) { save_state(guild_id); notify_state_change(guild_id); }
    return changed;
}

bool MusicPlayerService::resume(dpp::snowflake guild_id) {
    bool changed = false;
    {
    std::lock_guard lock(players_mutex_);
    auto it = players_.find(guild_id);
    if (it == players_.end()) return false;

    auto& player = *it->second;
    std::lock_guard pl(player.mutex);

    if (player.state == PlaybackState::Paused) {
        player.state = PlaybackState::Playing;
        spdlog::info("[music] Resumed in guild {}", guild_id);
        changed = true;
    }
    }
    if (changed) { save_state(guild_id); notify_state_change(guild_id); }
    return changed;
}

bool MusicPlayerService::set_volume(dpp::snowflake guild_id, int volume) {
    if (volume < 0 || volume > 100) return false;
    {
    std::lock_guard lock(players_mutex_);
    auto it = players_.find(guild_id);
    if (it == players_.end()) return false;

    auto& player = *it->second;
    std::lock_guard pl(player.mutex);
    player.volume = volume;
    spdlog::info("[music] Volume set to {} in guild {}", volume, guild_id);
    }
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::set_speed(dpp::snowflake guild_id, float speed) {
    if (speed < 0.5f || speed > 2.0f) return false;

    std::string url;
    int ffmpeg_ss = 0, pos = 0, bpm = 0;
    uint32_t gen = 0;
    bool nc = false, rv = false, ec = false, need_bg = false;

    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (player.speed == speed) return true;
        player.speed = speed;

        if ((player.state == PlaybackState::Playing || player.state == PlaybackState::Paused)
            && player.now_playing) {
            kill_bg_pipeline(player);
            player.bg_generation++;
            pos = player.elapsed_seconds;
            ffmpeg_ss = pos + player.now_playing->start_seconds;
            url = player.now_playing->audio_path.empty()
                ? player.now_playing->url : player.now_playing->audio_path;
            gen = player.bg_generation;
            nc = player.nightcore;
            rv = player.reverb;
            ec = player.echo;
            bpm = player.detected_bpm;
            need_bg = true;
            spdlog::info("[music] Speed {}x → bg pipeline at {}s in guild {}", speed, pos, guild_id);
        } else {
            spdlog::info("[music] Speed set to {}x in guild {}", speed, guild_id);
        }
    }

    if (need_bg) request_bg_pipeline(guild_id, url, ffmpeg_ss, pos, speed, nc, rv, ec, bpm, gen);
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::set_nightcore(dpp::snowflake guild_id, bool enabled) {
    std::string url;
    int ffmpeg_ss = 0, pos = 0, bpm = 0;
    uint32_t gen = 0;
    float spd = 1.0f;
    bool rv = false, ec = false, need_bg = false;

    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (player.nightcore == enabled) return true;
        player.nightcore = enabled;

        if ((player.state == PlaybackState::Playing || player.state == PlaybackState::Paused)
            && player.now_playing) {
            kill_bg_pipeline(player);
            player.bg_generation++;
            pos = player.elapsed_seconds;
            ffmpeg_ss = pos + player.now_playing->start_seconds;
            url = player.now_playing->audio_path.empty()
                ? player.now_playing->url : player.now_playing->audio_path;
            gen = player.bg_generation;
            spd = player.speed;
            rv = player.reverb;
            ec = player.echo;
            bpm = player.detected_bpm;
            need_bg = true;
            spdlog::info("[music] Nightcore {} → bg pipeline at {}s in guild {}",
                         enabled ? "ON" : "OFF", pos, guild_id);
        } else {
            spdlog::info("[music] Nightcore {} in guild {}", enabled ? "ON" : "OFF", guild_id);
        }
    }

    if (need_bg) request_bg_pipeline(guild_id, url, ffmpeg_ss, pos, spd, enabled, rv, ec, bpm, gen);
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::set_reverb(dpp::snowflake guild_id, bool enabled) {
    std::string url;
    int ffmpeg_ss = 0, pos = 0, bpm = 0;
    uint32_t gen = 0;
    float spd = 1.0f;
    bool nc = false, ec = false, need_bg = false;

    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (player.reverb == enabled) return true;
        player.reverb = enabled;

        if ((player.state == PlaybackState::Playing || player.state == PlaybackState::Paused)
            && player.now_playing) {
            kill_bg_pipeline(player);
            player.bg_generation++;
            pos = player.elapsed_seconds;
            ffmpeg_ss = pos + player.now_playing->start_seconds;
            url = player.now_playing->audio_path.empty()
                ? player.now_playing->url : player.now_playing->audio_path;
            gen = player.bg_generation;
            spd = player.speed;
            nc = player.nightcore;
            ec = player.echo;
            bpm = player.detected_bpm;
            need_bg = true;
            spdlog::info("[music] Reverb {} → bg pipeline at {}s in guild {}",
                         enabled ? "ON" : "OFF", pos, guild_id);
        } else {
            spdlog::info("[music] Reverb {} in guild {}", enabled ? "ON" : "OFF", guild_id);
        }
    }

    if (need_bg) request_bg_pipeline(guild_id, url, ffmpeg_ss, pos, spd, nc, enabled, ec, bpm, gen);
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::set_echo(dpp::snowflake guild_id, bool enabled) {
    std::string url;
    int ffmpeg_ss = 0, pos = 0, bpm = 0;
    uint32_t gen = 0;
    float spd = 1.0f;
    bool nc = false, rv = false, need_bg = false;

    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (player.echo == enabled) return true;
        player.echo = enabled;

        if ((player.state == PlaybackState::Playing || player.state == PlaybackState::Paused)
            && player.now_playing) {
            kill_bg_pipeline(player);
            player.bg_generation++;
            pos = player.elapsed_seconds;
            ffmpeg_ss = pos + player.now_playing->start_seconds;
            url = player.now_playing->audio_path.empty()
                ? player.now_playing->url : player.now_playing->audio_path;
            gen = player.bg_generation;
            spd = player.speed;
            nc = player.nightcore;
            rv = player.reverb;
            bpm = player.detected_bpm;
            need_bg = true;
            spdlog::info("[music] Echo {} → bg pipeline at {}s in guild {}",
                         enabled ? "ON" : "OFF", pos, guild_id);
        } else {
            spdlog::info("[music] Echo {} in guild {}", enabled ? "ON" : "OFF", guild_id);
        }
    }

    if (need_bg) request_bg_pipeline(guild_id, url, ffmpeg_ss, pos, spd, nc, rv, enabled, bpm, gen);
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::seek(dpp::snowflake guild_id, int position_seconds) {
    if (position_seconds < 0) return false;

    std::string url;
    int ffmpeg_ss = 0, bpm = 0;
    uint32_t gen = 0;
    float spd = 1.0f;
    bool nc = false, rv = false, ec = false, need_bg = false;

    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (player.state != PlaybackState::Playing && player.state != PlaybackState::Paused)
            return false;
        if (!player.now_playing) return false;

        kill_bg_pipeline(player);
        player.bg_generation++;
        player.elapsed_seconds = position_seconds;  // immediate UI update
        ffmpeg_ss = position_seconds + player.now_playing->start_seconds;
        url = player.now_playing->audio_path.empty()
            ? player.now_playing->url : player.now_playing->audio_path;
        gen = player.bg_generation;
        spd = player.speed;
        nc = player.nightcore;
        rv = player.reverb;
        ec = player.echo;
        bpm = player.detected_bpm;
        need_bg = true;
        spdlog::info("[music] Seek to {}s → bg pipeline in guild {}", position_seconds, guild_id);
    }

    if (need_bg) request_bg_pipeline(guild_id, url, ffmpeg_ss, position_seconds, spd, nc, rv, ec, bpm, gen);
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::remove(dpp::snowflake guild_id, size_t index) {
    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (index >= player.queue.size()) return false;
        player.queue.erase(player.queue.begin() + static_cast<ptrdiff_t>(index));
    }
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

bool MusicPlayerService::remove_history(dpp::snowflake guild_id, size_t index) {
    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return false;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);

        if (index >= player.history.size()) return false;
        player.history.erase(player.history.begin() + static_cast<ptrdiff_t>(index));
    }
    save_state(guild_id);
    notify_state_change(guild_id);
    return true;
}

static std::tuple<std::string, std::string, int>
run_ytdlp_search(const std::string& query, const std::string& cookies_path) {
    int pipefd[2];
    int errpipefd[2];
    if (pipe(pipefd) != 0 || pipe(errpipefd) != 0) {
        return {"", "pipe() failed", -1};
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        close(errpipefd[0]); close(errpipefd[1]);
        return {"", "fork() failed", -1};
    }

    if (pid == 0) {
        close(pipefd[0]);
        close(errpipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(errpipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        close(errpipefd[1]);

        if (!cookies_path.empty()) {
            execlp("yt-dlp", "yt-dlp",
                   "--dump-single-json", "--flat-playlist",
                   "--remote-components", "ejs:github",
                   "--cookies", cookies_path.c_str(),
                   "--", query.c_str(),
                   nullptr);
        } else {
            execlp("yt-dlp", "yt-dlp",
                   "--dump-single-json", "--flat-playlist",
                   "--remote-components", "ejs:github",
                   "--", query.c_str(),
                   nullptr);
        }
        _exit(1);
    }

    close(pipefd[1]);
    close(errpipefd[1]);

    std::string output, err_output;
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = read(pipefd[0], buffer.data(), buffer.size())) > 0)
        output.append(buffer.data(), n);
    close(pipefd[0]);
    while ((n = read(errpipefd[0], buffer.data(), buffer.size())) > 0)
        err_output.append(buffer.data(), n);
    close(errpipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {std::move(output), std::move(err_output), exit_code};
}

std::vector<MusicPlayerService::SearchResult> MusicPlayerService::search(
    const std::string& query, int max_results)
{
    std::vector<SearchResult> results;
    if (query.empty() || max_results <= 0) return results;

    std::string search_query = "ytsearch" + std::to_string(max_results) + ":" + query;
    std::string cookies_tmp = make_cookies_copy(get_cookies_path());

    auto [output, err_output, exit_code] = run_ytdlp_search(search_query, cookies_tmp);

    if ((exit_code != 0 || output.empty()) && !cookies_tmp.empty()) {
        spdlog::warn("[music] search failed with cookies (exit={}), retrying without", exit_code);
        std::tie(output, err_output, exit_code) = run_ytdlp_search(search_query, "");
    }

    if (!cookies_tmp.empty()) {
        std::filesystem::remove(cookies_tmp);
    }

    if (exit_code != 0 || output.empty()) {
        spdlog::error("[music] yt-dlp search failed (exit={}): {}",
                      exit_code, err_output.empty() ? "(no stderr)" : err_output);
        return results;
    }

    try {
        auto j = nlohmann::json::parse(output);
        if (!j.contains("entries") || !j["entries"].is_array()) {
            spdlog::warn("[music] Search response has no entries array");
            return results;
        }
        for (const auto& entry : j["entries"]) {
            SearchResult r;
            r.url = entry.value("url", "");
            // flat-playlist url is just the video ID — build full URL
            if (!r.url.empty() && r.url.find("http") == std::string::npos) {
                r.url = "https://www.youtube.com/watch?v=" + r.url;
            }
            r.title = entry.value("title", "Unknown");
            r.duration_seconds = static_cast<int>(entry.value("duration", 0.0));
            r.channel = entry.value("channel", entry.value("uploader", ""));
            // flat-playlist thumbnails may be in "thumbnails" array
            if (entry.contains("thumbnails") && entry["thumbnails"].is_array() &&
                !entry["thumbnails"].empty()) {
                r.thumbnail = entry["thumbnails"].back().value("url", "");
            } else {
                r.thumbnail = entry.value("thumbnail", "");
            }
            if (!r.url.empty()) {
                results.push_back(std::move(r));
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[music] Failed to parse search JSON: {}", e.what());
    }

    spdlog::info("[music] Search '{}': {} results", query, results.size());
    return results;
}

GuildMusicState MusicPlayerService::get_state(dpp::snowflake guild_id) {
    GuildMusicState state;

    std::lock_guard lock(players_mutex_);
    auto it = players_.find(guild_id);
    if (it == players_.end()) {
        state.state = PlaybackState::Idle;
        return state;
    }

    auto& player = *it->second;
    std::lock_guard pl(player.mutex);

    state.state = player.state;
    state.now_playing = player.now_playing;
    state.queue = player.queue;
    state.volume = player.volume;
    state.history = player.history;
    state.speed = player.speed;
    state.nightcore = player.nightcore;
    state.reverb = player.reverb;
    state.echo = player.echo;
    state.pipeline_pending = player.bg_generation > 0 || player.bg.swap_ready;
    state.error = player.last_error;

    // elapsed_seconds is now tracked by frames sent, no wall-clock adjustment needed
    state.elapsed_seconds = player.elapsed_seconds;
    state.detected_bpm = player.detected_bpm;

    return state;
}

dpp::snowflake MusicPlayerService::get_bot_id() const {
    return bot_.me.id;
}

// ═══════════════════════════════════════════════════
// State persistence (memcached)
// ═══════════════════════════════════════════════════

static nlohmann::json track_to_json(const TrackInfo& t) {
    nlohmann::json j = {
        {"url", t.url},
        {"title", t.title},
        {"duration", t.duration_seconds},
        {"start_seconds", t.start_seconds},
        {"requester", t.requester},
        {"thumbnail", t.thumbnail}
    };
    if (!t.audio_path.empty()) j["audio_path"] = t.audio_path;
    if (t.osu_beatmapset_id > 0) j["osu_beatmapset_id"] = t.osu_beatmapset_id;
    if (!t.chapters.empty()) {
        j["chapters"] = nlohmann::json::array();
        for (const auto& ch : t.chapters) {
            j["chapters"].push_back({{"title", ch.title}, {"start", ch.start_time}, {"end", ch.end_time}});
        }
    }
    return j;
}

static TrackInfo track_from_json(const nlohmann::json& j) {
    TrackInfo t;
    t.url = j.value("url", "");
    t.title = j.value("title", "");
    t.duration_seconds = j.value("duration", 0);
    t.start_seconds = j.value("start_seconds", 0);
    t.requester = j.value("requester", "");
    t.thumbnail = j.value("thumbnail", "");
    t.audio_path = j.value("audio_path", "");
    t.osu_beatmapset_id = j.value("osu_beatmapset_id", 0u);
    if (j.contains("chapters") && j["chapters"].is_array()) {
        for (const auto& ch : j["chapters"]) {
            Chapter c;
            c.title = ch.value("title", "");
            c.start_time = ch.value("start", 0);
            c.end_time = ch.value("end", 0);
            if (!c.title.empty()) t.chapters.push_back(std::move(c));
        }
    }
    return t;
}

void MusicPlayerService::save_state_locked(GuildPlayer& player, dpp::snowflake guild_id) {
    try {
        using json = nlohmann::json;
        json j;

        j["state"] = playback_state_to_string(player.state);
        j["volume"] = player.volume;
        j["speed"] = player.speed;
        j["nightcore"] = player.nightcore;
        j["reverb"] = player.reverb;
        j["echo"] = player.echo;
        j["detected_bpm"] = player.detected_bpm;
        j["elapsed_seconds"] = player.elapsed_seconds;
        j["voice_channel_id"] = std::to_string(static_cast<uint64_t>(player.voice_channel_id));

        if (player.now_playing) {
            j["now_playing"] = track_to_json(*player.now_playing);
        } else {
            j["now_playing"] = nullptr;
        }

        j["queue"] = json::array();
        for (const auto& t : player.queue) {
            j["queue"].push_back(track_to_json(t));
        }

        j["history"] = json::array();
        for (const auto& t : player.history) {
            j["history"].push_back(track_to_json(t));
        }

        auto key = "music:state:" + std::to_string(static_cast<uint64_t>(guild_id));
        cache::MemcachedCache::instance().set(key, j.dump(), std::chrono::seconds(0));
    } catch (const std::exception& e) {
        spdlog::warn("[music] Failed to save state for guild {}: {}", guild_id, e.what());
    }
}

void MusicPlayerService::save_state(dpp::snowflake guild_id) {
    {
        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);
        save_state_locked(player, guild_id);
    }
    save_guild_list();
}

void MusicPlayerService::save_guild_list() {
    try {
        using json = nlohmann::json;
        json arr = json::array();

        std::lock_guard lock(players_mutex_);
        for (const auto& [gid, _] : players_) {
            arr.push_back(std::to_string(gid));
        }

        cache::MemcachedCache::instance().set("music:guilds", arr.dump(), std::chrono::seconds(0));
    } catch (const std::exception& e) {
        spdlog::warn("[music] Failed to save guild list: {}", e.what());
    }
}

void MusicPlayerService::restore_all_states() {
    // Run restore in a separate thread so we don't block DPP's on_ready callback.
    // Voice events (on_voice_ready) are delivered on the event loop — if we block
    // on_ready, the event loop can't deliver them, causing voice timeout.
    // Stored as member so shutdown() can join it safely.
    restore_thread_ = std::jthread([this](std::stop_token stoken) {
        // Wait for DPP to process GUILD_CREATE events and stabilize
        spdlog::info("[music] Restore: waiting 3s for DPP to initialize...");
        std::this_thread::sleep_for(std::chrono::seconds(3));

        if (stoken.stop_requested() || shutting_down_) return;

        using json = nlohmann::json;

        try {
            auto guilds_raw = cache::MemcachedCache::instance().get("music:guilds");
            if (!guilds_raw) {
                spdlog::info("[music] No saved music state found in memcached");
                return;
            }

            spdlog::info("[music] Restore: guild list raw = {}", *guilds_raw);

            auto guild_ids = json::parse(*guilds_raw, nullptr, false);
            if (!guild_ids.is_array() || guild_ids.empty()) {
                spdlog::info("[music] Saved guild list is empty");
                return;
            }

            for (const auto& gid_val : guild_ids) {
                if (stoken.stop_requested() || shutting_down_) return;

                std::string gid_str = gid_val.get<std::string>();
                auto guild_id = dpp::snowflake(gid_str);
                if (guild_id == 0) continue;

                auto key = "music:state:" + gid_str;
                auto state_raw = cache::MemcachedCache::instance().get(key);
                if (!state_raw) {
                    spdlog::info("[music] No saved state for guild {}", gid_str);
                    continue;
                }

                auto j = json::parse(*state_raw, nullptr, false);
                if (!j.is_object()) {
                    spdlog::warn("[music] Invalid saved state JSON for guild {}", gid_str);
                    continue;
                }

                spdlog::info("[music] Restore: parsing state for guild {} (state={}, now_playing={})",
                             gid_str, j.value("state", "?"),
                             j.contains("now_playing") && !j["now_playing"].is_null()
                                 ? j["now_playing"].value("title", "?") : "null");

                auto& player = get_or_create_player(guild_id);
                std::lock_guard pl(player.mutex);

                // Restore settings
                player.volume = j.value("volume", 50);
                player.speed = j.value("speed", 1.0f);
                player.nightcore = j.value("nightcore", false);
                player.reverb = j.value("reverb", false);
                player.echo = j.value("echo", false);
                player.detected_bpm = j.value("detected_bpm", 0);

                // Restore history
                player.history.clear();
                if (j.contains("history") && j["history"].is_array()) {
                    for (const auto& tj : j["history"]) {
                        player.history.push_back(track_from_json(tj));
                    }
                }

                // Restore queue
                player.queue.clear();
                if (j.contains("queue") && j["queue"].is_array()) {
                    for (const auto& tj : j["queue"]) {
                        player.queue.push_back(track_from_json(tj));
                    }
                }

                // Restore now_playing — re-queue with seek offset
                std::string saved_state = j.value("state", "idle");
                bool was_active = (saved_state == "playing" || saved_state == "paused");
                int elapsed = j.value("elapsed_seconds", 0);

                if (was_active && j.contains("now_playing") && !j["now_playing"].is_null()) {
                    auto np = track_from_json(j["now_playing"]);
                    // Don't modify start_seconds — it must stay as the original URL
                    // offset (?t=30). The resume_offset tells play_track to seek to
                    // start_seconds + resume_offset, which gives the correct absolute
                    // position without compounding on repeated restarts.
                    np.resume_offset = elapsed;
                    player.queue.push_front(np);

                    // Restore voice channel and trigger playback
                    std::string vc_str = j.value("voice_channel_id", "0");
                    player.voice_channel_id = dpp::snowflake(vc_str);

                    if (player.voice_channel_id != 0) {
                        player.voice_ready = false;
                        player.stopping = false;

                        auto* shard = get_shard_for_guild(guild_id);
                        if (shard) {
                            // Clear stale DPP voice state before connecting (same as play())
                            auto* existing = shard->get_voice(guild_id);
                            if (existing) {
                                spdlog::info("[music] Restore: clearing stale voice for guild {}", guild_id);
                                shard->disconnect_voice(guild_id);
                                std::this_thread::sleep_for(std::chrono::seconds(1));
                            }

                            spdlog::info("[music] Restore: connecting voice guild={} channel={}",
                                         guild_id, player.voice_channel_id);
                            shard->connect_voice(guild_id, player.voice_channel_id, false, false);
                        } else {
                            spdlog::error("[music] Restore: no shard available for guild {}", guild_id);
                        }

                        player.has_work = true;
                        player.work_cv.notify_all();
                    }

                    spdlog::info("[music] Restored guild {} — resuming '{}' at {}s ({} queued, {} history)",
                                 gid_str, np.title, elapsed,
                                 player.queue.size() - 1, player.history.size());
                } else {
                    spdlog::info("[music] Restored guild {} — idle ({} queued, {} history)",
                                 gid_str, player.queue.size(), player.history.size());
                }
            }

            spdlog::info("[music] Restore complete");
        } catch (const std::exception& e) {
            spdlog::error("[music] Failed to restore states: {}", e.what());
        }
    });
}

void MusicPlayerService::on_voice_ready(const dpp::voice_ready_t& event) {
    spdlog::info("[music] Voice ready for channel {}", event.voice_channel_id);

    std::lock_guard lock(players_mutex_);
    for (auto& [gid, player] : players_) {
        std::lock_guard pl(player->mutex);
        if (player->voice_channel_id == event.voice_channel_id) {
            player->voice_ready = true;
            player->voice_ready_cv.notify_all();
            spdlog::info("[music] Voice ready signaled for guild {}", gid);
            break;
        }
    }
}

void MusicPlayerService::on_voice_state_update(const dpp::voice_state_update_t& event) {
    auto bot_id = bot_.me.id;

    // Log all bot voice state changes for debugging
    if (event.state.user_id == bot_id) {
        spdlog::info("[music] Voice state update: guild={} channel={} user=bot",
                     event.state.guild_id, event.state.channel_id);
    }

    // Check if bot was disconnected from voice
    if (event.state.user_id == bot_id && event.state.channel_id == 0) {
        auto guild_id = event.state.guild_id;

        std::lock_guard lock(players_mutex_);
        auto it = players_.find(guild_id);
        if (it == players_.end()) return;

        auto& player = *it->second;
        std::lock_guard pl(player.mutex);
        player.voice_ready = false;

        if (player.stopping || player.state == PlaybackState::Idle) {
            // Intentional disconnect (stop command or queue empty)
            spdlog::info("[music] Bot disconnected from voice in guild {} (intentional)", guild_id);
            player.stopping = false;
        } else if (player.state == PlaybackState::Playing || player.state == PlaybackState::Paused) {
            // Unexpected disconnect — auto-reconnect, keep pipeline alive
            spdlog::warn("[music] Unexpected voice disconnect in guild {}, reconnecting to channel {}...",
                         guild_id, player.voice_channel_id);

            auto channel_id = player.voice_channel_id;
            // Reconnect on a separate thread to avoid blocking the event loop.
            // Capture bot_ as raw pointer (cluster outlives MusicPlayerService)
            // instead of 'this' to avoid use-after-free if service is destroyed
            // while the thread sleeps.
            dpp::cluster* bot_ptr = &bot_;
            std::jthread([bot_ptr, guild_id, channel_id]() {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto* shard = bot_ptr->get_shard(0);
                if (shard) {
                    spdlog::info("[music] Reconnecting voice to channel {} in guild {}", channel_id, guild_id);
                    shard->connect_voice(guild_id, channel_id, false, false);
                }
            }).detach();
        } else {
            spdlog::info("[music] Bot disconnected from voice in guild {} (state={})",
                         guild_id, playback_state_to_string(player.state));
        }
    }
}

// --- Private implementation ---

void MusicPlayerService::worker_loop(std::stop_token stop_token, dpp::snowflake guild_id) {
    spdlog::info("[music] Worker started for guild {}", guild_id);

    while (!stop_token.stop_requested() && !shutting_down_) {
        GuildPlayer* player_ptr = nullptr;
        {
            std::lock_guard lock(players_mutex_);
            auto it = players_.find(guild_id);
            if (it == players_.end()) break;
            player_ptr = it->second.get();
        }
        auto& player = *player_ptr;

        // Wait for work
        {
            std::unique_lock lock(player.mutex);
            player.work_cv.wait(lock, stop_token, [&] {
                return player.has_work || stop_token.stop_requested() || shutting_down_;
            });
            if (stop_token.stop_requested() || shutting_down_) break;
            player.has_work = false;
        }

        // Process queue
        bool should_disconnect = false;
        while (!stop_token.stop_requested() && !shutting_down_) {
            TrackInfo track;
            {
                std::lock_guard lock(player.mutex);
                if (player.state == PlaybackState::Stopped || player.queue.empty()) {
                    player.state = PlaybackState::Idle;
                    player.now_playing.reset();
                    player.voice_ready = false;
                    player.stopping = true;
                    should_disconnect = true;
                    if (!shutting_down_) {
                        save_state_locked(player, guild_id);
                        // Notify outside lock below via should_disconnect path
                    }
                    break;
                }
                track = player.queue.front();
                player.queue.pop_front();
                player.now_playing = track;
                player.state = PlaybackState::Playing;
                // resume_offset is non-zero for restored tracks (so elapsed counter
                // starts at the correct position instead of 0:00)
                player.elapsed_seconds = track.resume_offset;
                player.elapsed_offset = track.resume_offset;
                player.bg_generation = 0;
                player.playback_started_at = std::chrono::steady_clock::now();
                player.last_error.clear();
                if (!shutting_down_) save_state_locked(player, guild_id);
            }

            if (!shutting_down_) notify_state_change(guild_id);
            spdlog::info("[music] Playing '{}' in guild {}", track.title, guild_id);

            bool ok = play_track(player, guild_id, track, stop_token);
            if (!ok) {
                std::lock_guard lock(player.mutex);
                if (player.state != PlaybackState::Stopped) {
                    spdlog::warn("[music] Track '{}' failed in guild {}", track.title, guild_id);
                }
            }

            // Clean up subprocess and record in history
            {
                std::lock_guard lock(player.mutex);
                kill_subprocesses(player);
                if (ok) {
                    player.history.push_front(track);
                    if (player.history.size() > MAX_HISTORY_SIZE)
                        player.history.pop_back();
                }
                // Don't save during shutdown — shutdown() already saved the correct
                // state (Playing) before setting state=Stopped for cleanup.
                if (!shutting_down_) save_state_locked(player, guild_id);
            }
            if (!shutting_down_) notify_state_change(guild_id);
        }

        // Disconnect voice when queue is exhausted
        if (should_disconnect) {
            spdlog::info("[music] Queue empty, disconnecting voice in guild {}", guild_id);
            disconnect_voice(guild_id);
            if (!shutting_down_) notify_state_change(guild_id);
            should_disconnect = false;
        }
    }

    spdlog::info("[music] Worker stopped for guild {}", guild_id);
}

bool MusicPlayerService::play_track(GuildPlayer& player, dpp::snowflake guild_id,
                                     const TrackInfo& track, std::stop_token& stop_token) {
    // Wait for voice to be ready
    bool fresh_connection = false;
    {
        std::unique_lock lock(player.mutex);
        if (!player.voice_ready) {
            fresh_connection = true;
            spdlog::info("[music] Waiting for voice connection (timeout=15s)...");
            bool ready = player.voice_ready_cv.wait_for(lock, std::chrono::seconds(15), [&] {
                return player.voice_ready || stop_token.stop_requested() || shutting_down_;
            });
            if (!ready || stop_token.stop_requested() || shutting_down_) {
                spdlog::error("[music] Voice connection timeout! voice_ready={}, stop={}, shutdown={}",
                              player.voice_ready, stop_token.stop_requested(), shutting_down_.load());
                player.last_error = "Voice connection timeout";
                return false;
            }
            spdlog::info("[music] Voice connection established");
        } else {
            spdlog::info("[music] Voice already ready, proceeding to audio pipeline");
        }
    }

    // On fresh connections, send silence frames to prime the voice pipeline.
    // After a long idle, Discord may not be fully ready to relay audio even
    // after voice_ready fires — silence warmup ensures the first real frame
    // is heard. Skip this between consecutive tracks (voice already active).
    if (fresh_connection) {
        auto* shard = get_shard_for_guild(guild_id);
        if (shard) {
            auto* vconn = shard->get_voice(guild_id);
            if (vconn && vconn->voiceclient) {
                std::array<uint8_t, PCM_FRAME_SIZE> silence{};
                constexpr int WARMUP_FRAMES = 10;  // 10 × 60ms = 600ms
                for (int i = 0; i < WARMUP_FRAMES; ++i) {
                    vconn->voiceclient->send_audio_raw(
                        reinterpret_cast<uint16_t*>(silence.data()), PCM_FRAME_SIZE);
                }
                spdlog::info("[music] Sent {} silence warmup frames (fresh connection)", WARMUP_FRAMES);
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
            }
        }
    }

    // Start the audio pipeline (resume_offset accounts for restored position)
    int seek_pos = track.start_seconds + track.resume_offset;
    std::string source = track.audio_path.empty() ? track.url : track.audio_path;

    // Detect BPM for echo sync (uses yt-dlp+ffmpeg for URLs, ffmpeg for local files)
    {
        std::string cookies_copy = make_cookies_copy(get_cookies_path());
        int bpm = detect_bpm(source, seek_pos, cookies_copy);
        if (!cookies_copy.empty()) std::filesystem::remove(cookies_copy);
        std::lock_guard lock(player.mutex);
        player.detected_bpm = bpm;
    }

    if (!start_audio_pipeline(player, source, seek_pos)) {
        std::lock_guard lock(player.mutex);
        player.last_error = "Failed to start audio pipeline";
        return false;
    }

    // Stream audio to Discord — bg pipeline swaps happen inside stream_audio
    stream_audio(player, guild_id, stop_token);

    // After stream_audio exits, check if a bg pipeline is ready or in progress
    {
        std::lock_guard lock(player.mutex);
        kill_subprocesses(player);
    }

    // If bg pipeline is ready or in progress, wait and swap then re-enter stream_audio
    for (int wait_round = 0; wait_round < 2 && !stop_token.stop_requested() && !shutting_down_; ++wait_round) {
        bool has_bg = false;
        {
            std::lock_guard lock(player.mutex);
            if (player.bg.swap_ready) {
                // Swap immediately
                player.ytdlp_pid = player.bg.ytdlp_pid;
                player.ffmpeg_pid = player.bg.ffmpeg_pid;
                player.audio_pipe_fd = player.bg.audio_pipe_fd;
                player.speed = player.bg.speed;
                player.nightcore = player.bg.nightcore;
                player.reverb = player.bg.reverb;
                player.echo = player.bg.echo;
                player.elapsed_offset = player.bg.elapsed_offset;
                player.bg.ytdlp_pid = -1;
                player.bg.ffmpeg_pid = -1;
                player.bg.audio_pipe_fd = -1;
                player.bg.swap_ready = false;
                player.bg_generation = 0;
                has_bg = true;
                spdlog::info("[music] play_track: swapped bg pipeline on exit");
            } else if (player.bg_generation > 0 && player.state != PlaybackState::Stopped) {
                // bg in progress, wait up to 10s
                has_bg = false;  // will wait below
            } else {
                break;  // track finished normally
            }
        }

        if (has_bg) {
            stream_audio(player, guild_id, stop_token);
            std::lock_guard lock(player.mutex);
            kill_subprocesses(player);
            continue;
        }

        // Wait for bg pipeline to become ready (up to 10s)
        spdlog::info("[music] play_track: waiting for bg pipeline (up to 10s)...");
        bool swapped = false;
        for (int i = 0; i < 100 && !stop_token.stop_requested() && !shutting_down_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard lock(player.mutex);
            if (player.state == PlaybackState::Stopped) break;
            if (player.bg.swap_ready) {
                player.ytdlp_pid = player.bg.ytdlp_pid;
                player.ffmpeg_pid = player.bg.ffmpeg_pid;
                player.audio_pipe_fd = player.bg.audio_pipe_fd;
                player.speed = player.bg.speed;
                player.nightcore = player.bg.nightcore;
                player.reverb = player.bg.reverb;
                player.echo = player.bg.echo;
                player.elapsed_offset = player.bg.elapsed_offset;
                player.bg.ytdlp_pid = -1;
                player.bg.ffmpeg_pid = -1;
                player.bg.audio_pipe_fd = -1;
                player.bg.swap_ready = false;
                player.bg_generation = 0;
                swapped = true;
                spdlog::info("[music] play_track: bg pipeline arrived after wait");
                break;
            }
        }

        if (swapped) {
            stream_audio(player, guild_id, stop_token);
            std::lock_guard lock(player.mutex);
            kill_subprocesses(player);
            continue;
        }

        break;  // bg pipeline didn't arrive in time, move on
    }

    return true;
}

// Run yt-dlp --dump-json and return (stdout, stderr, exit_code)
static std::tuple<std::string, std::string, int>
run_ytdlp_metadata(const std::string& url, const std::string& cookies_path) {
    int pipefd[2];
    int errpipefd[2];
    if (pipe(pipefd) != 0 || pipe(errpipefd) != 0) {
        return {"", "pipe() failed", -1};
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        close(errpipefd[0]); close(errpipefd[1]);
        return {"", "fork() failed", -1};
    }

    if (pid == 0) {
        close(pipefd[0]);
        close(errpipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(errpipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        close(errpipefd[1]);

        if (!cookies_path.empty()) {
            execlp("yt-dlp", "yt-dlp",
                   "--dump-json", "--no-playlist",
                   "--remote-components", "ejs:github",
                   "--cookies", cookies_path.c_str(),
                   "--", url.c_str(),
                   nullptr);
        } else {
            execlp("yt-dlp", "yt-dlp",
                   "--dump-json", "--no-playlist",
                   "--remote-components", "ejs:github",
                   "--", url.c_str(),
                   nullptr);
        }
        _exit(1);
    }

    close(pipefd[1]);
    close(errpipefd[1]);

    std::string output, err_output;
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = read(pipefd[0], buffer.data(), buffer.size())) > 0)
        output.append(buffer.data(), n);
    close(pipefd[0]);
    while ((n = read(errpipefd[0], buffer.data(), buffer.size())) > 0)
        err_output.append(buffer.data(), n);
    close(errpipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {std::move(output), std::move(err_output), exit_code};
}

static std::tuple<std::string, std::string, int>
run_ytdlp_flat_playlist(const std::string& url, const std::string& cookies_path) {
    int pipefd[2];
    int errpipefd[2];
    if (pipe(pipefd) != 0 || pipe(errpipefd) != 0) {
        return {"", "pipe() failed", -1};
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        close(errpipefd[0]); close(errpipefd[1]);
        return {"", "fork() failed", -1};
    }

    if (pid == 0) {
        close(pipefd[0]);
        close(errpipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(errpipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        close(errpipefd[1]);

        for (int fd = 3; fd < 1024; ++fd) close(fd);

        if (!cookies_path.empty()) {
            execlp("yt-dlp", "yt-dlp",
                   "--flat-playlist", "--dump-json",
                   "--remote-components", "ejs:github",
                   "--cookies", cookies_path.c_str(),
                   "--", url.c_str(),
                   nullptr);
        } else {
            execlp("yt-dlp", "yt-dlp",
                   "--flat-playlist", "--dump-json",
                   "--remote-components", "ejs:github",
                   "--", url.c_str(),
                   nullptr);
        }
        _exit(1);
    }

    close(pipefd[1]);
    close(errpipefd[1]);

    std::string output, err_output;
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = read(pipefd[0], buffer.data(), buffer.size())) > 0)
        output.append(buffer.data(), n);
    close(pipefd[0]);
    while ((n = read(errpipefd[0], buffer.data(), buffer.size())) > 0)
        err_output.append(buffer.data(), n);
    close(errpipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return {std::move(output), std::move(err_output), exit_code};
}

bool MusicPlayerService::is_playlist_url(const std::string& url) {
    // youtube.com/playlist?list=... or music.youtube.com/playlist?list=...
    static const std::regex re(R"(https?://(www\.)?(music\.)?youtu(be\.com|\.be)/playlist\?list=)");
    return std::regex_search(url, re);
}

std::vector<TrackInfo> MusicPlayerService::fetch_playlist(const std::string& url) {
    std::string cookies_tmp = make_cookies_copy(get_cookies_path());
    spdlog::info("[music] fetch_playlist: url={}", url);

    auto [output, err_output, exit_code] = run_ytdlp_flat_playlist(url, cookies_tmp);

    if (!cookies_tmp.empty()) {
        std::filesystem::remove(cookies_tmp);
    }

    if (exit_code != 0 || output.empty()) {
        spdlog::error("[music] yt-dlp playlist failed (exit={}): {}",
                      exit_code, err_output.empty() ? "(no stderr)" : err_output);
        return {};
    }

    std::vector<TrackInfo> tracks;
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            TrackInfo info;
            info.url = j.value("webpage_url", j.value("url", ""));
            info.title = j.value("title", "Unknown");
            info.duration_seconds = static_cast<int>(j.value("duration", 0.0));

            if (j.contains("thumbnails") && j["thumbnails"].is_array() && !j["thumbnails"].empty()) {
                info.thumbnail = j["thumbnails"].back().value("url", "");
            }

            if (!info.url.empty()) {
                tracks.push_back(std::move(info));
            }
        } catch (const std::exception& e) {
            spdlog::warn("[music] Failed to parse playlist entry: {}", e.what());
        }
    }

    spdlog::info("[music] Playlist parsed: {} tracks", tracks.size());
    return tracks;
}

std::optional<TrackInfo> MusicPlayerService::fetch_metadata(const std::string& url) {
    std::string cookies_tmp = make_cookies_copy(get_cookies_path());
    spdlog::info("[music] fetch_metadata: cookies={}", cookies_tmp.empty() ? "none" : cookies_tmp);

    auto [output, err_output, exit_code] = run_ytdlp_metadata(url, cookies_tmp);

    // If failed with cookies, retry without them (cookies might be expired)
    if ((exit_code != 0 || output.empty()) && !cookies_tmp.empty()) {
        spdlog::warn("[music] yt-dlp failed with cookies (exit={}), retrying without: {}",
                     exit_code, err_output);
        std::tie(output, err_output, exit_code) = run_ytdlp_metadata(url, "");
    }

    // On successful exit, copy updated cookies back to original (session refresh)
    // then clean up temp file. On failure, just discard.
    if (!cookies_tmp.empty()) {
        if (exit_code == 0) {
            auto original = get_cookies_path();
            if (!original.empty() && std::filesystem::file_size(cookies_tmp) > 0) {
                try {
                    std::filesystem::copy_file(cookies_tmp, original,
                        std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::copy_file(cookies_tmp, original + ".bak",
                        std::filesystem::copy_options::overwrite_existing);
                } catch (...) {}
            }
        }
        std::filesystem::remove(cookies_tmp);
    }

    if (exit_code != 0 || output.empty()) {
        spdlog::error("[music] yt-dlp metadata failed (exit={}): {}",
                      exit_code, err_output.empty() ? "(no stderr)" : err_output);
        return std::nullopt;
    }

    try {
        auto j = nlohmann::json::parse(output);
        TrackInfo info;
        info.url = url;
        info.title = j.value("title", "Unknown");
        info.duration_seconds = static_cast<int>(j.value("duration", 0.0));
        info.thumbnail = j.value("thumbnail", "");
        info.start_seconds = parse_start_time(url);
        if (info.start_seconds > 0 && info.duration_seconds > info.start_seconds) {
            info.duration_seconds -= info.start_seconds;
        }
        if (j.contains("chapters") && j["chapters"].is_array()) {
            for (const auto& ch : j["chapters"]) {
                Chapter c;
                c.title = ch.value("title", "");
                c.start_time = static_cast<int>(ch.value("start_time", 0.0));
                c.end_time = static_cast<int>(ch.value("end_time", 0.0));
                if (info.start_seconds > 0) {
                    c.start_time = std::max(0, c.start_time - info.start_seconds);
                    c.end_time = std::max(0, c.end_time - info.start_seconds);
                }
                if (!c.title.empty() && c.end_time > c.start_time) {
                    info.chapters.push_back(std::move(c));
                }
            }
        }
        spdlog::info("[music] Metadata: title='{}' duration={}s start={}s chapters={}",
                     info.title, info.duration_seconds, info.start_seconds, info.chapters.size());
        return info;
    } catch (const std::exception& e) {
        spdlog::error("[music] Failed to parse yt-dlp JSON: {}", e.what());
        return std::nullopt;
    }
}

MusicPlayerService::PipelineInfo MusicPlayerService::create_pipeline(
    const std::string& url, int start_seconds, float speed, bool nightcore, bool reverb, bool echo,
    int detected_bpm)
{
    bool is_local = !url.empty() && url[0] == '/';

    // Build common ffmpeg filter string
    float effective_speed = speed;
    if (nightcore && speed == 1.0f) effective_speed = 1.25f;

    std::string filter;
    if (nightcore && effective_speed != 1.0f) {
        int new_rate = static_cast<int>(48000 * effective_speed);
        filter = "asetrate=" + std::to_string(new_rate) + ",aresample=48000";
    } else if (effective_speed != 1.0f) {
        filter = "atempo=" + std::to_string(effective_speed);
    }

    if (reverb) {
        std::string reverb_filter = "aecho=0.8:0.7:40|120|200:0.5|0.3|0.2";
        if (filter.empty()) {
            filter = reverb_filter;
        } else {
            filter += "," + reverb_filter;
        }
    }

    if (echo) {
        std::string echo_filter;
        if (detected_bpm > 0) {
            // BPM-synced: taps on eighth-note grid, up to 2 beats (capped 2000ms)
            int eighth_ms = 30000 / detected_bpm;
            int max_ms = std::min(2 * 60000 / detected_bpm, 2000);
            int taps = std::max(max_ms / std::max(eighth_ms, 1), 4);
            echo_filter = build_echo_filter(taps, max_ms);
            spdlog::debug("[music] Echo synced to {}BPM: {}taps, {}ms max, {}ms eighth",
                          detected_bpm, taps, max_ms, eighth_ms);
        } else {
            echo_filter = build_echo_filter(6, 1500, 0.3, 0.02);
        }
        if (filter.empty()) {
            filter = echo_filter;
        } else {
            filter += "," + echo_filter;
        }
    }

    if (is_local) {
        // Local file: ffmpeg reads directly, no yt-dlp needed
        return create_local_pipeline(url, start_seconds, filter);
    }

    // Remote URL: yt-dlp | ffmpeg pipeline
    std::string cookies_tmp = make_cookies_copy(get_cookies_path());
    PipelineInfo info;

    int ytdlp_to_ffmpeg[2];
    int ffmpeg_to_parent[2];

    if (pipe(ytdlp_to_ffmpeg) != 0) {
        spdlog::error("[music] Failed to create pipe 1: {}", strerror(errno));
        return info;
    }
    if (pipe(ffmpeg_to_parent) != 0) {
        spdlog::error("[music] Failed to create pipe 2: {}", strerror(errno));
        close(ytdlp_to_ffmpeg[0]);
        close(ytdlp_to_ffmpeg[1]);
        return info;
    }

    // Fork yt-dlp
    pid_t ytdlp_pid = fork();
    if (ytdlp_pid < 0) {
        spdlog::error("[music] Failed to fork yt-dlp: {}", strerror(errno));
        close(ytdlp_to_ffmpeg[0]);
        close(ytdlp_to_ffmpeg[1]);
        close(ffmpeg_to_parent[0]);
        close(ffmpeg_to_parent[1]);
        return info;
    }

    if (ytdlp_pid == 0) {
        // Child: yt-dlp
        close(ytdlp_to_ffmpeg[0]);
        close(ffmpeg_to_parent[0]);
        close(ffmpeg_to_parent[1]);

        dup2(ytdlp_to_ffmpeg[1], STDOUT_FILENO);
        close(ytdlp_to_ffmpeg[1]);

        int errlog = open("data/ytdlp_stderr.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errlog >= 0) { dup2(errlog, STDERR_FILENO); close(errlog); }

        std::vector<std::string> args = {"yt-dlp", "--no-playlist", "-f", "bestaudio",
                                          "--remote-components", "ejs:github"};
        if (!cookies_tmp.empty()) {
            args.push_back("--cookies");
            args.push_back(cookies_tmp);
        }
        args.push_back("-o");
        args.push_back("-");
        args.push_back("--");
        args.push_back(url);

        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp("yt-dlp", argv.data());
        _exit(1);
    }

    // Fork ffmpeg
    pid_t ffmpeg_pid = fork();
    if (ffmpeg_pid < 0) {
        spdlog::error("[music] Failed to fork ffmpeg: {}", strerror(errno));
        kill(ytdlp_pid, SIGKILL);
        waitpid(ytdlp_pid, nullptr, 0);
        close(ytdlp_to_ffmpeg[0]);
        close(ytdlp_to_ffmpeg[1]);
        close(ffmpeg_to_parent[0]);
        close(ffmpeg_to_parent[1]);
        return info;
    }

    if (ffmpeg_pid == 0) {
        // Child: ffmpeg
        close(ytdlp_to_ffmpeg[1]);
        close(ffmpeg_to_parent[0]);

        dup2(ytdlp_to_ffmpeg[0], STDIN_FILENO);
        close(ytdlp_to_ffmpeg[0]);

        dup2(ffmpeg_to_parent[1], STDOUT_FILENO);
        close(ffmpeg_to_parent[1]);

        int errlog = open("data/ffmpeg_stderr.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errlog >= 0) { dup2(errlog, STDERR_FILENO); close(errlog); }

        std::vector<std::string> args = {"ffmpeg"};

        std::string ss_val;
        if (start_seconds > 0) {
            ss_val = std::to_string(start_seconds);
            args.push_back("-ss");
            args.push_back(ss_val);
        }

        args.push_back("-i");
        args.push_back("pipe:0");

        if (!filter.empty()) {
            args.push_back("-af");
            args.push_back(filter);
        }

        args.push_back("-f");
        args.push_back("s16le");
        args.push_back("-ar");
        args.push_back("48000");
        args.push_back("-ac");
        args.push_back("2");
        args.push_back("pipe:1");

        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp("ffmpeg", argv.data());
        _exit(1);
    }

    // Parent: close unused ends
    close(ytdlp_to_ffmpeg[0]);
    close(ytdlp_to_ffmpeg[1]);
    close(ffmpeg_to_parent[1]);

    info.ytdlp_pid = ytdlp_pid;
    info.ffmpeg_pid = ffmpeg_pid;
    info.audio_pipe_fd = ffmpeg_to_parent[0];

    spdlog::info("[music] Pipeline created (yt-dlp={}, ffmpeg={}, ss={}, speed={}x, nc={}, reverb={}, echo={})",
                 ytdlp_pid, ffmpeg_pid, start_seconds, speed, nightcore, reverb, echo);

    // Clean up temp cookies file after yt-dlp has had time to read it
    if (!cookies_tmp.empty()) {
        std::jthread([path = std::move(cookies_tmp)]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::filesystem::remove(path);
        }).detach();
    }

    return info;
}

MusicPlayerService::PipelineInfo MusicPlayerService::create_local_pipeline(
    const std::string& filepath, int start_seconds, const std::string& filter)
{
    // Local file: ffmpeg reads directly from file, no yt-dlp needed
    PipelineInfo info;

    int ffmpeg_to_parent[2];
    if (pipe(ffmpeg_to_parent) != 0) {
        spdlog::error("[music] Failed to create pipe for local pipeline: {}", strerror(errno));
        return info;
    }

    pid_t ffmpeg_pid = fork();
    if (ffmpeg_pid < 0) {
        spdlog::error("[music] Failed to fork ffmpeg (local): {}", strerror(errno));
        close(ffmpeg_to_parent[0]);
        close(ffmpeg_to_parent[1]);
        return info;
    }

    if (ffmpeg_pid == 0) {
        // Child: ffmpeg reading from local file
        close(ffmpeg_to_parent[0]);

        dup2(ffmpeg_to_parent[1], STDOUT_FILENO);
        close(ffmpeg_to_parent[1]);

        int errlog = open("data/ffmpeg_stderr.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errlog >= 0) { dup2(errlog, STDERR_FILENO); close(errlog); }

        std::vector<std::string> args = {"ffmpeg"};

        std::string ss_val;
        if (start_seconds > 0) {
            ss_val = std::to_string(start_seconds);
            args.push_back("-ss");
            args.push_back(ss_val);
        }

        args.push_back("-i");
        args.push_back(filepath);  // direct file path instead of pipe:0

        if (!filter.empty()) {
            args.push_back("-af");
            args.push_back(filter);
        }

        args.push_back("-f");
        args.push_back("s16le");
        args.push_back("-ar");
        args.push_back("48000");
        args.push_back("-ac");
        args.push_back("2");
        args.push_back("pipe:1");

        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp("ffmpeg", argv.data());
        _exit(1);
    }

    // Parent
    close(ffmpeg_to_parent[1]);

    info.ytdlp_pid = -1;  // no yt-dlp process
    info.ffmpeg_pid = ffmpeg_pid;
    info.audio_pipe_fd = ffmpeg_to_parent[0];

    spdlog::info("[music] Local pipeline created (ffmpeg={}, file={}, ss={})",
                 ffmpeg_pid, filepath, start_seconds);

    return info;
}

bool MusicPlayerService::start_audio_pipeline(GuildPlayer& player, const std::string& url, int start_seconds) {
    float speed;
    bool nightcore, reverb, echo;
    int bpm;
    {
        std::lock_guard lock(player.mutex);
        speed = player.speed;
        nightcore = player.nightcore;
        reverb = player.reverb;
        echo = player.echo;
        bpm = player.detected_bpm;
    }

    auto info = create_pipeline(url, start_seconds, speed, nightcore, reverb, echo, bpm);
    if (info.audio_pipe_fd < 0) {
        return false;
    }

    std::lock_guard lock(player.mutex);
    player.ytdlp_pid = info.ytdlp_pid;
    player.ffmpeg_pid = info.ffmpeg_pid;
    player.audio_pipe_fd = info.audio_pipe_fd;
    return true;
}

void MusicPlayerService::stream_audio(GuildPlayer& player, dpp::snowflake guild_id,
                                       std::stop_token& stop_token) {
    // PCM frame: 2880 samples @ 48kHz = 60ms per frame
    static constexpr auto FRAME_DURATION = std::chrono::milliseconds(60);

    std::array<uint8_t, PCM_FRAME_SIZE> buffer;
    uint64_t frames_sent = 0;
    uint32_t consecutive_send_failures = 0;
    bool reconnecting = false;
    auto next_frame_time = std::chrono::steady_clock::now();

    // Capture speed and elapsed_offset at start (they don't change within a single pipeline run)
    float speed;
    bool nightcore;
    int elapsed_offset;
    {
        std::lock_guard lock(player.mutex);
        speed = player.speed;
        nightcore = player.nightcore;
        elapsed_offset = player.elapsed_offset;
    }
    // Effective speed accounts for default nightcore rate
    float effective_speed = speed;
    if (nightcore && speed == 1.0f) effective_speed = 1.25f;

    // Helper to send a full PCM frame to the voice client
    auto send_frame = [&]() -> bool {
        auto* shard = get_shard_for_guild(guild_id);
        if (!shard) {
            if (frames_sent == 0) spdlog::warn("[music] No shard available for audio send");
            return false;
        }
        auto* vconn = shard->get_voice(guild_id);
        if (!vconn || !vconn->voiceclient) {
            if (frames_sent % 500 == 0) {
                spdlog::warn("[music] No voice client for guild {} (vconn={}, frame={})",
                             guild_id, (void*)vconn, frames_sent);
            }
            return false;
        }
        vconn->voiceclient->send_audio_raw(
            reinterpret_cast<uint16_t*>(buffer.data()),
            PCM_FRAME_SIZE);
        return true;
    };

    // Read exactly `count` bytes from fd (blocking), returns false on EOF/error
    auto read_full = [](int fd, uint8_t* dest, size_t count) -> bool {
        size_t total = 0;
        while (total < count) {
            ssize_t n = read(fd, dest + total, count - total);
            if (n > 0) {
                total += n;
            } else if (n == 0) {
                return false; // EOF
            } else {
                if (errno == EINTR) continue;
                return false; // error
            }
        }
        return true;
    };

    while (!stop_token.stop_requested() && !shutting_down_) {
        // Check for bg pipeline swap
        PlaybackState current_state;
        int fd;
        int vol;
        bool did_swap = false;
        bool sync_elapsed = false;
        {
            std::lock_guard lock(player.mutex);

            if (player.bg.swap_ready) {
                // Save old pipeline info for async cleanup
                pid_t old_ytdlp = player.ytdlp_pid;
                pid_t old_ffmpeg = player.ffmpeg_pid;
                int old_fd = player.audio_pipe_fd;

                // Swap in the new pipeline
                player.ytdlp_pid = player.bg.ytdlp_pid;
                player.ffmpeg_pid = player.bg.ffmpeg_pid;
                player.audio_pipe_fd = player.bg.audio_pipe_fd;
                player.speed = player.bg.speed;
                player.nightcore = player.bg.nightcore;
                player.reverb = player.bg.reverb;
                player.echo = player.bg.echo;
                player.elapsed_offset = player.bg.elapsed_offset;

                // Reset bg slot and generation
                player.bg.ytdlp_pid = -1;
                player.bg.ffmpeg_pid = -1;
                player.bg.audio_pipe_fd = -1;
                player.bg.swap_ready = false;
                player.bg_generation = 0;

                // Update local tracking variables
                speed = player.speed;
                nightcore = player.nightcore;
                elapsed_offset = player.elapsed_offset;
                effective_speed = speed;
                if (nightcore && speed == 1.0f) effective_speed = 1.25f;
                frames_sent = 0;
                next_frame_time = std::chrono::steady_clock::now();

                did_swap = true;
                spdlog::info("[music] Swapped to bg pipeline (speed={}x, nc={}, reverb={}, echo={}, offset={}s)",
                             speed, nightcore, player.reverb, player.echo, elapsed_offset);

                // Kill old pipeline async
                std::jthread([old_ytdlp, old_ffmpeg, old_fd]() {
                    if (old_fd >= 0) close(old_fd);
                    if (old_ytdlp > 0) {
                        kill(old_ytdlp, SIGTERM);
                        int status;
                        if (waitpid(old_ytdlp, &status, WNOHANG) == 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            kill(old_ytdlp, SIGKILL);
                            waitpid(old_ytdlp, &status, 0);
                        }
                    }
                    if (old_ffmpeg > 0) {
                        kill(old_ffmpeg, SIGTERM);
                        int status;
                        if (waitpid(old_ffmpeg, &status, WNOHANG) == 0) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            kill(old_ffmpeg, SIGKILL);
                            waitpid(old_ffmpeg, &status, 0);
                        }
                    }
                }).detach();
            }

            current_state = player.state;
            fd = player.audio_pipe_fd;
            vol = player.volume;
        }

        if (did_swap && !shutting_down_) notify_state_change(guild_id);

        if (current_state == PlaybackState::Stopped) break;
        if (fd < 0) break;

        if (current_state == PlaybackState::Paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            // Reset timing so we don't try to catch up after unpause
            next_frame_time = std::chrono::steady_clock::now();
            continue;
        }

        // Read a full PCM frame (blocking)
        if (!read_full(fd, buffer.data(), PCM_FRAME_SIZE)) {
            // EOF or error — check if bg swap is pending before declaring track finished
            {
                std::lock_guard lock(player.mutex);
                if (player.bg.swap_ready || player.bg_generation > 0) {
                    // bg pipeline is ready or in progress — wait for it
                    spdlog::info("[music] EOF on main pipe, waiting for bg pipeline...");
                    // Will be caught by play_track's bg wait logic
                    break;
                }
            }
            int elapsed;
            {
                std::lock_guard lock(player.mutex);
                elapsed = player.elapsed_seconds;
            }
            spdlog::info("[music] Track finished (EOF) after {}s", elapsed);
            break;
        }

        // Apply volume scaling
        auto* samples = reinterpret_cast<int16_t*>(buffer.data());
        size_t num_samples = PCM_FRAME_SIZE / 2;
        double volume_scale = vol / 100.0;

        for (size_t i = 0; i < num_samples; ++i) {
            int32_t scaled = static_cast<int32_t>(samples[i] * volume_scale);
            if (scaled > 32767) scaled = 32767;
            if (scaled < -32768) scaled = -32768;
            samples[i] = static_cast<int16_t>(scaled);
        }

        // Pace: wait until it's time to send this frame
        auto now = std::chrono::steady_clock::now();
        if (next_frame_time > now) {
            std::this_thread::sleep_until(next_frame_time);
        }

        bool sent = send_frame();
        ++frames_sent;
        next_frame_time += FRAME_DURATION;

        if (sent) {
            if (consecutive_send_failures > 0) {
                spdlog::info("[music] Voice send recovered after {} failed frames", consecutive_send_failures);
                consecutive_send_failures = 0;
                reconnecting = false;
            }
        } else {
            ++consecutive_send_failures;

            // After ~3 seconds of failures (50 frames × 60ms), attempt voice reconnect
            if (consecutive_send_failures == 50 && !reconnecting) {
                reconnecting = true;
                spdlog::warn("[music] {} consecutive send failures in guild {}, attempting voice reconnect",
                             consecutive_send_failures, guild_id);

                dpp::snowflake channel_id;
                {
                    std::lock_guard lock(player.mutex);
                    channel_id = player.voice_channel_id;
                    player.voice_ready = false;
                }

                dpp::cluster* bot_ptr = &bot_;
                std::jthread([bot_ptr, guild_id, channel_id]() {
                    auto* shard = bot_ptr->get_shard(0);
                    if (!shard) return;
                    auto* existing = shard->get_voice(guild_id);
                    if (existing) {
                        spdlog::info("[music] Disconnecting stale voice for guild {}", guild_id);
                        shard->disconnect_voice(guild_id);
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    spdlog::info("[music] Reconnecting voice to channel {} in guild {}", channel_id, guild_id);
                    shard->connect_voice(guild_id, channel_id, false, false);
                }).detach();
            }

            // After ~15 seconds of failures (250 frames), give up
            if (consecutive_send_failures >= 250) {
                spdlog::error("[music] Voice connection lost for {} frames (~15s), stopping stream in guild {}",
                              consecutive_send_failures, guild_id);
                break;
            }
        }

        if (frames_sent == 1) {
            spdlog::info("[music] First audio frame sent (ok={})", sent);
        } else if (frames_sent % 1000 == 0) {
            spdlog::info("[music] Audio progress: {} frames ({}s)", frames_sent, (frames_sent * 60) / 1000);
        }

        // If we fell behind (e.g. after pause or reconnect), reset timing
        now = std::chrono::steady_clock::now();
        if (next_frame_time + std::chrono::seconds(1) < now) {
            next_frame_time = now;
        }

        // Update elapsed: offset + output_time * speed (frames @ 60ms each)
        {
            std::lock_guard lock(player.mutex);
            player.elapsed_seconds = elapsed_offset +
                static_cast<int>(static_cast<double>(frames_sent * 60) / 1000.0 * effective_speed);

            // Periodic save every ~10s (166 frames × 60ms ≈ 10s)
            if (frames_sent % 166 == 0) {
                save_state_locked(player, guild_id);
            }
            // WS elapsed sync every ~5s (83 frames × 60ms ≈ 5s)
            sync_elapsed = (frames_sent % 83 == 0);
        }
        if (sync_elapsed && !shutting_down_) {
            notify_state_change(guild_id);
            sync_elapsed = false;
        }
    }
}

void MusicPlayerService::kill_subprocesses(GuildPlayer& player) {
    // Close pipe first so children get SIGPIPE if still writing
    if (player.audio_pipe_fd >= 0) {
        close(player.audio_pipe_fd);
        player.audio_pipe_fd = -1;
    }
    // Send SIGTERM first, then reap. Use blocking waitpid to avoid zombies.
    if (player.ytdlp_pid > 0) {
        kill(player.ytdlp_pid, SIGTERM);
        int status;
        if (waitpid(player.ytdlp_pid, &status, WNOHANG) == 0) {
            // Still running after SIGTERM, give it a moment then force kill
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            kill(player.ytdlp_pid, SIGKILL);
            waitpid(player.ytdlp_pid, &status, 0);
        }
        player.ytdlp_pid = -1;
    }
    if (player.ffmpeg_pid > 0) {
        kill(player.ffmpeg_pid, SIGTERM);
        int status;
        if (waitpid(player.ffmpeg_pid, &status, WNOHANG) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            kill(player.ffmpeg_pid, SIGKILL);
            waitpid(player.ffmpeg_pid, &status, 0);
        }
        player.ffmpeg_pid = -1;
    }
}

void MusicPlayerService::kill_bg_pipeline(GuildPlayer& player) {
    // Caller must hold player.mutex
    if (player.bg.audio_pipe_fd >= 0) {
        close(player.bg.audio_pipe_fd);
        player.bg.audio_pipe_fd = -1;
    }
    if (player.bg.ytdlp_pid > 0) {
        kill(player.bg.ytdlp_pid, SIGKILL);
        waitpid(player.bg.ytdlp_pid, nullptr, 0);  // blocking — SIGKILL is guaranteed
        player.bg.ytdlp_pid = -1;
    }
    if (player.bg.ffmpeg_pid > 0) {
        kill(player.bg.ffmpeg_pid, SIGKILL);
        waitpid(player.bg.ffmpeg_pid, nullptr, 0);  // blocking — SIGKILL is guaranteed
        player.bg.ffmpeg_pid = -1;
    }
    player.bg.swap_ready = false;
    player.bg.elapsed_offset = 0;
    player.bg.speed = 1.0f;
    player.bg.nightcore = false;
    player.bg.reverb = false;
    player.bg.echo = false;
}

void MusicPlayerService::request_bg_pipeline(dpp::snowflake guild_id, const std::string& url,
                                              int ffmpeg_ss, int elapsed_offset,
                                              float speed, bool nightcore, bool reverb, bool echo,
                                              int detected_bpm, uint32_t generation)
{
    // Helper: reset bg_generation if still matching (no newer request superseded us)
    auto reset_generation_if_current = [this, guild_id, generation]() {
        GuildPlayer* p = nullptr;
        {
            std::lock_guard lock(players_mutex_);
            auto it = players_.find(guild_id);
            if (it != players_.end()) p = it->second.get();
        }
        if (p) {
            std::lock_guard pl(p->mutex);
            if (p->bg_generation == generation) p->bg_generation = 0;
        }
    };

    // Helper: kill and reap a pipeline (blocking waitpid after SIGKILL)
    auto kill_pipeline_info = [](PipelineInfo& info) {
        if (info.audio_pipe_fd >= 0) { close(info.audio_pipe_fd); info.audio_pipe_fd = -1; }
        if (info.ytdlp_pid > 0) {
            kill(info.ytdlp_pid, SIGKILL);
            waitpid(info.ytdlp_pid, nullptr, 0);
            info.ytdlp_pid = -1;
        }
        if (info.ffmpeg_pid > 0) {
            kill(info.ffmpeg_pid, SIGKILL);
            waitpid(info.ffmpeg_pid, nullptr, 0);
            info.ffmpeg_pid = -1;
        }
    };

    std::jthread([this, guild_id, url, ffmpeg_ss, elapsed_offset, speed, nightcore, reverb, echo, detected_bpm, generation,
                  reset_generation_if_current, kill_pipeline_info]() {
        spdlog::info("[music] bg pipeline thread started (gen={}, ss={}, speed={}x, nc={}, reverb={}, echo={}, bpm={})",
                     generation, ffmpeg_ss, speed, nightcore, reverb, echo, detected_bpm);

        auto info = create_pipeline(url, ffmpeg_ss, speed, nightcore, reverb, echo, detected_bpm);
        if (info.audio_pipe_fd < 0) {
            spdlog::warn("[music] bg pipeline creation failed (gen={})", generation);
            reset_generation_if_current();
            return;
        }

        // Wait for data to be ready (poll up to 30s)
        struct pollfd pfd;
        pfd.fd = info.audio_pipe_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 30000);
        if (ret <= 0 || !(pfd.revents & POLLIN)) {
            spdlog::warn("[music] bg pipeline poll timeout/error (gen={}, ret={})", generation, ret);
            kill_pipeline_info(info);
            reset_generation_if_current();
            return;
        }

        // Data ready — check if this generation is still current
        GuildPlayer* player_ptr = nullptr;
        {
            std::lock_guard lock(players_mutex_);
            auto it = players_.find(guild_id);
            if (it != players_.end()) player_ptr = it->second.get();
        }
        if (!player_ptr || shutting_down_) {
            kill_pipeline_info(info);
            return;
        }

        std::lock_guard pl(player_ptr->mutex);
        if (player_ptr->bg_generation != generation || player_ptr->state == PlaybackState::Stopped) {
            spdlog::info("[music] bg pipeline gen mismatch or stopped (gen={}, current={})",
                         generation, player_ptr->bg_generation);
            kill_pipeline_info(info);
            return;
        }

        // Store in bg slot and flag as ready
        player_ptr->bg.ytdlp_pid = info.ytdlp_pid;
        player_ptr->bg.ffmpeg_pid = info.ffmpeg_pid;
        player_ptr->bg.audio_pipe_fd = info.audio_pipe_fd;
        player_ptr->bg.elapsed_offset = elapsed_offset;
        player_ptr->bg.speed = speed;
        player_ptr->bg.nightcore = nightcore;
        player_ptr->bg.reverb = reverb;
        player_ptr->bg.echo = echo;
        player_ptr->bg.swap_ready = true;
        spdlog::info("[music] bg pipeline ready for swap (gen={})", generation);
    }).detach();
}

void MusicPlayerService::disconnect_voice(dpp::snowflake guild_id) {
    auto* shard = get_shard_for_guild(guild_id);
    if (shard) {
        auto* vconn = shard->get_voice(guild_id);
        if (vconn) {
            shard->disconnect_voice(guild_id);
            spdlog::info("[music] Disconnected from voice in guild {}", guild_id);
        }
    }
}

dpp::discord_client* MusicPlayerService::get_shard_for_guild(dpp::snowflake guild_id) {
    auto id = static_cast<uint64_t>(guild_id);
    uint32_t shard_id = static_cast<uint32_t>((id >> 22) % bot_.numshards);
    return bot_.get_shard(shard_id);
}

} // namespace services
