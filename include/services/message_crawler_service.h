#pragma once

#include <database.h>
#include <dpp/dpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <sys/types.h>

namespace services {

/**
 * Configuration for the message crawler
 */
struct CrawlerConfig {
    dpp::snowflake target_guild_id{1030424871173361704ULL};
    std::unordered_set<dpp::snowflake> excluded_channels{
        dpp::snowflake{1284189678035009670ULL},
        dpp::snowflake{1446982392777805958ULL},
        dpp::snowflake{1233825835140644896ULL}
    };

    // Rate limiting configuration
    // Discord limit: ~5 requests per 5 seconds per route
    // Being conservative to avoid 429s
    std::chrono::milliseconds request_delay{1200};  // 1.2s delay = ~50 req/min
    size_t messages_per_request{100};               // Max allowed by Discord API

    // Word stats configuration
    size_t min_word_length{3};
    std::chrono::minutes stats_update_interval{15}; // How often to recalculate stats
};

/**
 * Overall crawl status
 */
struct CrawlStatus {
    size_t total_channels{0};
    size_t completed_channels{0};
    size_t total_messages{0};
    bool is_crawling{false};
    std::string current_channel_name;
    dpp::snowflake current_channel_id{0};
    std::chrono::system_clock::time_point last_update;
};

/**
 * Service for crawling Discord message history and computing word statistics.
 * Runs as a background service with start/stop lifecycle.
 */
class MessageCrawlerService {
public:
    explicit MessageCrawlerService(dpp::cluster& bot, const CrawlerConfig& config = CrawlerConfig{});
    ~MessageCrawlerService();

    // Lifecycle
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Status
    CrawlStatus get_status() const;
    db::CrawlStatusSummary get_crawl_summary() const;
    std::vector<db::ChannelCrawlProgress> get_channel_progress() const;

    // Word statistics
    std::vector<db::WordStatEntry> get_top_words(size_t limit = 100, const std::string& language = "", bool exclude_stopwords = false) const;
    std::vector<db::WordStatEntry> get_user_top_words(dpp::snowflake user_id, size_t limit = 100, const std::string& language = "", bool exclude_stopwords = false, dpp::snowflake channel_id = 0) const;
    std::vector<db::WordStatEntry> get_channel_top_words(dpp::snowflake channel_id, size_t limit = 100, const std::string& language = "", bool exclude_stopwords = false) const;
    size_t get_unique_word_count(const std::string& language = "", bool exclude_stopwords = false) const;
    std::vector<db::MessageAuthor> get_message_authors() const;
    std::vector<db::MessageChannel> get_message_channels() const;
    void trigger_stats_refresh();  // Force immediate stats recalculation

    // Phrase statistics
    std::vector<db::PhraseStatEntry> get_top_phrases(
        size_t limit = 100,
        const std::string& language = "",
        bool sort_by_pmi = false,
        int word_count_filter = 0,
        size_t min_count = 5
    ) const;
    std::vector<db::PhraseStatEntry> get_user_top_phrases(
        dpp::snowflake user_id,
        size_t limit = 100,
        const std::string& language = "",
        bool sort_by_pmi = false,
        int word_count_filter = 0,
        size_t min_count = 5,
        dpp::snowflake channel_id = 0,
        const std::string& sort_mode = ""  // "count", "pmi", "npmi", "uniqueness"
    ) const;
    std::vector<db::PhraseStatEntry> get_channel_top_phrases(
        dpp::snowflake channel_id,
        size_t limit = 100,
        const std::string& language = "",
        bool sort_by_pmi = false,
        int word_count_filter = 0,
        size_t min_count = 5
    ) const;
    size_t get_unique_phrase_count(const std::string& language = "", int word_count_filter = 0) const;

    // Configuration
    void set_config(const CrawlerConfig& config);
    CrawlerConfig get_config() const;

    // Called when new message arrives (from message handler)
    void on_new_message(const dpp::message& msg);

    // Called when message is edited (from message handler)
    void on_message_update(const dpp::message& msg);

private:
    // Worker threads
    void crawler_thread();           // Main crawl loop for historical messages
    void stats_worker_thread();      // Periodic stats calculation

    // Crawling logic
    void discover_channels();
    void crawl_channel_history(dpp::snowflake channel_id, const std::string& channel_name);
    void crawl_new_messages(dpp::snowflake channel_id);

    // Word processing
    void calculate_word_stats();
    std::string detect_language(const std::string& word) const;
    std::vector<std::string> tokenize(const std::string& content) const;
    bool is_valid_word(const std::string& word) const;

    // Phrase processing
    void calculate_phrase_stats();
    void extract_phrases(
        const std::vector<std::string>& tokens,
        std::unordered_map<std::string, std::tuple<std::vector<std::string>, size_t, std::string>>& phrase_counts
    ) const;
    bool is_valid_phrase(const std::vector<std::string>& words) const;
    void calculate_pmi_scores_from_db();

    // Helper methods
    db::CrawledMessage message_to_crawled(const dpp::message& msg) const;
    void wait_for_rate_limit();
    void count_words(
        const std::string& content,
        std::unordered_map<std::string, std::pair<size_t, std::string>>& word_counts,
        const std::string& language_filter = "",
        const std::unordered_set<std::string>* stopwords = nullptr
    ) const;

    // Lemmatizer subprocess management
    void start_lemmatizer_service();
    void stop_lemmatizer_service();
    bool is_lemmatizer_running() const;
    std::vector<std::string> lemmatize_words(const std::vector<std::string>& words) const;

    // Members
    dpp::cluster& bot_;
    CrawlerConfig config_;
    pid_t lemmatizer_pid_{0};
    std::string lemmatizer_socket_path_{"/tmp/lemmatizer.sock"};

    // Thread control
    std::thread crawler_worker_;
    std::thread stats_worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stats_refresh_requested_{false};

    // Synchronization
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // State
    std::unordered_set<std::string> stopwords_;
    std::unordered_set<std::string> blacklist_;  // Words to exclude from phrases
    CrawlStatus current_status_;

    // Rate limiting
    std::chrono::steady_clock::time_point last_request_time_;

    // Statistics
    std::atomic<size_t> messages_crawled_session_{0};
};

} // namespace services
