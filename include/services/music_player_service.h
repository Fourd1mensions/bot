#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dpp/snowflake.h>

// Forward declarations
namespace dpp {
class cluster;
class discord_client;
class voiceconn;
class discord_voice_client;
struct voice_ready_t;
struct voice_state_update_t;
}

namespace services {

struct Chapter {
    std::string title;
    int start_time = 0;
    int end_time = 0;
};

struct TrackInfo {
    std::string url;
    std::string title;
    int duration_seconds = 0;  // 0 = unknown
    int start_seconds = 0;     // seek offset from URL (?t=120)
    int resume_offset = 0;     // transient: elapsed position for restore (not serialized)
    std::string requester;     // Discord username
    std::string thumbnail;
    std::string audio_path;          // local file path for osu tracks (empty for YouTube)
    uint32_t osu_beatmapset_id = 0;  // beatmapset ID for osu tracks (0 for YouTube)
    std::vector<Chapter> chapters;
};

enum class PlaybackState {
    Idle,
    Playing,
    Paused,
    Stopped
};

inline std::string playback_state_to_string(PlaybackState s) {
    switch (s) {
        case PlaybackState::Idle:    return "idle";
        case PlaybackState::Playing: return "playing";
        case PlaybackState::Paused:  return "paused";
        case PlaybackState::Stopped: return "stopped";
    }
    return "unknown";
}

struct GuildMusicState {
    PlaybackState state = PlaybackState::Idle;
    std::optional<TrackInfo> now_playing;
    std::deque<TrackInfo> queue;
    std::deque<TrackInfo> history;
    int volume = 50;  // 0-100
    float speed = 1.0f;
    bool nightcore = false;
    bool reverb = false;
    bool echo = false;
    bool pipeline_pending = false;  // bg pipeline swap in progress
    std::string error;
    int elapsed_seconds = 0;
    int detected_bpm = 0;
};

class MusicPlayerService {
public:
    explicit MusicPlayerService(dpp::cluster& bot, const std::string& cookies_path = "");
    ~MusicPlayerService();

    MusicPlayerService(const MusicPlayerService&) = delete;
    MusicPlayerService& operator=(const MusicPlayerService&) = delete;

    // YouTube cookies path for yt-dlp authentication
    void set_cookies_path(const std::string& path);
    std::string get_cookies_path() const;

    // Main controls
    struct PlayResult {
        bool success = false;
        std::string error;
        std::string title;
        int queue_position = 0;  // 0 = playing now, >0 = queued
    };

    PlayResult play(dpp::snowflake guild_id, dpp::snowflake channel_id,
                    const std::string& url, const std::string& requester);
    PlayResult play(dpp::snowflake guild_id, dpp::snowflake channel_id,
                    const TrackInfo& track_info);

    std::vector<TrackInfo> fetch_playlist(const std::string& url);
    static bool is_playlist_url(const std::string& url);
    bool skip(dpp::snowflake guild_id);
    bool stop(dpp::snowflake guild_id);
    bool pause(dpp::snowflake guild_id);
    bool resume(dpp::snowflake guild_id);
    bool set_volume(dpp::snowflake guild_id, int volume);
    bool set_speed(dpp::snowflake guild_id, float speed);
    bool set_nightcore(dpp::snowflake guild_id, bool enabled);
    bool set_reverb(dpp::snowflake guild_id, bool enabled);
    bool set_echo(dpp::snowflake guild_id, bool enabled);
    bool seek(dpp::snowflake guild_id, int position_seconds);
    bool remove(dpp::snowflake guild_id, size_t index);
    bool remove_history(dpp::snowflake guild_id, size_t index);

    // Search YouTube via yt-dlp ytsearch
    struct SearchResult {
        std::string url;
        std::string title;
        std::string thumbnail;
        int duration_seconds = 0;
        std::string channel;
    };
    std::vector<SearchResult> search(const std::string& query, int max_results = 5);

    // State queries
    GuildMusicState get_state(dpp::snowflake guild_id);
    dpp::snowflake get_bot_id() const;

    // DPP event callbacks
    void on_voice_ready(const dpp::voice_ready_t& event);
    void on_voice_state_update(const dpp::voice_state_update_t& event);

    // State change callback (called after each state-altering operation)
    using StateChangeCallback = std::function<void(uint64_t)>;
    void set_on_state_change(StateChangeCallback cb);

    // Shutdown
    void shutdown();

    // State persistence (memcached)
    void save_state(dpp::snowflake guild_id);
    void restore_all_states();

private:
    // Result of creating an audio pipeline (yt-dlp | ffmpeg)
    struct PipelineInfo {
        pid_t ytdlp_pid = -1;
        pid_t ffmpeg_pid = -1;
        int audio_pipe_fd = -1;  // read end of ffmpeg PCM output
    };

    // Background pipeline waiting to be swapped in
    struct BgPipeline {
        pid_t ytdlp_pid = -1;
        pid_t ffmpeg_pid = -1;
        int audio_pipe_fd = -1;
        int elapsed_offset = 0;
        float speed = 1.0f;
        bool nightcore = false;
        bool reverb = false;
        bool echo = false;
        bool swap_ready = false;
    };

    struct GuildPlayer {
        std::mutex mutex;
        PlaybackState state = PlaybackState::Idle;
        std::deque<TrackInfo> queue;
        std::deque<TrackInfo> history;  // last N finished tracks (most recent first)
        std::optional<TrackInfo> now_playing;
        int volume = 50;
        float speed = 1.0f;
        bool nightcore = false;
        bool reverb = false;
        bool echo = false;
        int elapsed_offset = 0;   // base position after seek for elapsed calculation
        int detected_bpm = 0;     // 0 = not detected; cached per track for echo sync
        std::string last_error;

        // Voice connection state
        dpp::snowflake voice_channel_id{0};
        bool voice_ready = false;
        bool stopping = false;  // true when intentionally stopping (stop cmd / queue empty)
        std::condition_variable voice_ready_cv;

        // Subprocess management (main pipeline)
        pid_t ytdlp_pid = -1;
        pid_t ffmpeg_pid = -1;
        int audio_pipe_fd = -1;  // Read end of ffmpeg output

        // Background pipeline for seamless swap
        BgPipeline bg;
        uint32_t bg_generation = 0;

        // Worker thread
        std::jthread worker;
        std::condition_variable_any work_cv;
        bool has_work = false;

        // Timing
        std::chrono::steady_clock::time_point playback_started_at;
        int elapsed_seconds = 0;
    };

    // State persistence helpers
    void save_state_locked(GuildPlayer& player, dpp::snowflake guild_id);
    void save_guild_list();

    GuildPlayer& get_or_create_player(dpp::snowflake guild_id);
    void worker_loop(std::stop_token stop_token, dpp::snowflake guild_id);
    bool play_track(GuildPlayer& player, dpp::snowflake guild_id, const TrackInfo& track, std::stop_token& stop_token);
    void kill_subprocesses(GuildPlayer& player);
    void kill_bg_pipeline(GuildPlayer& player);  // must hold player.mutex
    void disconnect_voice(dpp::snowflake guild_id);
    dpp::discord_client* get_shard_for_guild(dpp::snowflake guild_id);

    // Starts a bg pipeline request (launches detached thread)
    void request_bg_pipeline(dpp::snowflake guild_id, const std::string& url,
                              int ffmpeg_ss, int elapsed_offset,
                              float speed, bool nightcore, bool reverb, bool echo,
                              int detected_bpm, uint32_t generation);

    // Metadata fetch via yt-dlp --dump-json (uses fork/exec, no shell)
    std::optional<TrackInfo> fetch_metadata(const std::string& url);

    // Audio pipeline creation (pure, doesn't modify player state)
    PipelineInfo create_pipeline(const std::string& url, int start_seconds,
                                  float speed, bool nightcore, bool reverb, bool echo,
                                  int detected_bpm = 0);
    // Local file pipeline (ffmpeg only, no yt-dlp)
    PipelineInfo create_local_pipeline(const std::string& filepath, int start_seconds,
                                        const std::string& filter);
    // Wrapper that stores PipelineInfo in player fields
    bool start_audio_pipeline(GuildPlayer& player, const std::string& url, int start_seconds = 0);
    void stream_audio(GuildPlayer& player, dpp::snowflake guild_id, std::stop_token& stop_token);

    void notify_state_change(uint64_t guild_id);

    dpp::cluster& bot_;
    StateChangeCallback on_state_change_;
    std::mutex callback_mutex_;
    std::mutex players_mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<GuildPlayer>> players_;
    std::atomic<bool> shutting_down_{false};
    std::jthread restore_thread_;  // async restore on startup
    mutable std::mutex cookies_mutex_;
    std::string cookies_path_;
};

} // namespace services
