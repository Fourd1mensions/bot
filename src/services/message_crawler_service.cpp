#include "services/message_crawler_service.h"
#include <database.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <regex>
#include <sstream>
#include <unordered_set>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace services {

// Constants
namespace {
    constexpr size_t kWordStatsBatchSize = 1000;
    constexpr size_t kProgressLogInterval = 1000;
    constexpr size_t kSmallBatchThreshold = 50;
    constexpr std::chrono::seconds kRateLimitRetryDelay{5};
}

MessageCrawlerService::MessageCrawlerService(dpp::cluster& bot, const CrawlerConfig& config)
    : bot_(bot), config_(config) {
    spdlog::info("[Crawler] Initialized with target guild {}", config_.target_guild_id.str());
}

MessageCrawlerService::~MessageCrawlerService() {
    stop();
    stop_lemmatizer_service();
}

void MessageCrawlerService::start() {
    if (running_.load()) {
        spdlog::warn("[Crawler] Already running");
        return;
    }

    running_.store(true);

    // Load stopwords from database
    try {
        stopwords_ = db::Database::instance().get_stopwords();
        spdlog::info("[Crawler] Loaded {} stopwords", stopwords_.size());
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to load stopwords: {}", e.what());
    }

    // Load word blacklist from database
    try {
        blacklist_ = db::Database::instance().get_word_blacklist();
        spdlog::info("[Crawler] Loaded {} blacklisted words", blacklist_.size());
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to load word blacklist: {}", e.what());
    }

    // Start lemmatizer service if not running
    start_lemmatizer_service();

    // Start worker threads
    crawler_worker_ = std::thread(&MessageCrawlerService::crawler_thread, this);
    stats_worker_ = std::thread(&MessageCrawlerService::stats_worker_thread, this);

    spdlog::info("[Crawler] Started background workers");
}

void MessageCrawlerService::stop() {
    if (!running_.load()) {
        return;
    }

    spdlog::info("[Crawler] Stopping...");
    running_.store(false);
    cv_.notify_all();

    if (crawler_worker_.joinable()) {
        crawler_worker_.join();
    }
    if (stats_worker_.joinable()) {
        stats_worker_.join();
    }

    spdlog::info("[Crawler] Stopped");
}

CrawlStatus MessageCrawlerService::get_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_status_;
}

db::CrawlStatusSummary MessageCrawlerService::get_crawl_summary() const {
    try {
        return db::Database::instance().get_crawl_status_summary(config_.target_guild_id);
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get crawl summary: {}", e.what());
        return {};
    }
}

std::vector<db::ChannelCrawlProgress> MessageCrawlerService::get_channel_progress() const {
    try {
        return db::Database::instance().get_all_crawl_progress(config_.target_guild_id);
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get channel progress: {}", e.what());
        return {};
    }
}

std::vector<db::WordStatEntry> MessageCrawlerService::get_top_words(size_t limit, const std::string& language, bool exclude_stopwords) const {
    try {
        return db::Database::instance().get_top_words(limit, language, exclude_stopwords);
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get top words: {}", e.what());
        return {};
    }
}

size_t MessageCrawlerService::get_unique_word_count(const std::string& language, bool exclude_stopwords) const {
    try {
        return db::Database::instance().get_unique_word_count(language, exclude_stopwords);
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get unique word count: {}", e.what());
        return 0;
    }
}

std::vector<db::WordStatEntry> MessageCrawlerService::get_user_top_words(dpp::snowflake user_id, size_t limit, const std::string& language, bool exclude_stopwords, dpp::snowflake channel_id) const {
    try {
        // Load stopwords if needed
        std::unordered_set<std::string> stopwords_set;
        if (exclude_stopwords) {
            stopwords_set = db::Database::instance().get_stopwords();
        }

        // Calculate word stats on-the-fly for this user
        std::unordered_map<std::string, std::pair<size_t, std::string>> word_counts;

        const std::unordered_set<std::string>* stopwords_ptr = exclude_stopwords ? &stopwords_set : nullptr;

        // Use channel-specific or all-channel processing
        if (channel_id != 0) {
            db::Database::instance().process_user_channel_messages(user_id, channel_id, [&](const std::string& content) {
                count_words(content, word_counts, language, stopwords_ptr);
                return true;
            });
        } else {
            db::Database::instance().process_user_messages(user_id, [&](const std::string& content) {
                count_words(content, word_counts, language, stopwords_ptr);
                return true;
            });
        }

        // Convert to vector and sort by count
        std::vector<db::WordStatEntry> entries;
        entries.reserve(word_counts.size());

        for (const auto& [key, value] : word_counts) {
            auto pos = key.rfind('_');
            std::string word = key.substr(0, pos);

            db::WordStatEntry entry;
            entry.word = word;
            entry.count = value.first;
            entry.language = value.second;
            entries.push_back(entry);
        }

        // Sort by count descending
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.count > b.count;
        });

        // Limit results
        if (entries.size() > limit) {
            entries.resize(limit);
        }

        return entries;
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get user top words: {}", e.what());
        return {};
    }
}

std::vector<db::WordStatEntry> MessageCrawlerService::get_channel_top_words(dpp::snowflake channel_id, size_t limit, const std::string& language, bool exclude_stopwords) const {
    try {
        // Load stopwords if needed
        std::unordered_set<std::string> stopwords_set;
        if (exclude_stopwords) {
            stopwords_set = db::Database::instance().get_stopwords();
        }

        // Calculate word stats on-the-fly for this channel
        std::unordered_map<std::string, std::pair<size_t, std::string>> word_counts;

        const std::unordered_set<std::string>* stopwords_ptr = exclude_stopwords ? &stopwords_set : nullptr;

        db::Database::instance().process_channel_messages(channel_id, [&](const std::string& content) {
            count_words(content, word_counts, language, stopwords_ptr);
            return true;
        });

        // Convert to vector and sort by count
        std::vector<db::WordStatEntry> entries;
        entries.reserve(word_counts.size());

        for (const auto& [key, value] : word_counts) {
            auto pos = key.rfind('_');
            std::string word = key.substr(0, pos);

            db::WordStatEntry entry;
            entry.word = word;
            entry.count = value.first;
            entry.language = value.second;
            entries.push_back(entry);
        }

        // Sort by count descending
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.count > b.count;
        });

        // Limit results
        if (entries.size() > limit) {
            entries.resize(limit);
        }

        return entries;
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get channel top words: {}", e.what());
        return {};
    }
}

std::vector<db::MessageAuthor> MessageCrawlerService::get_message_authors() const {
    try {
        return db::Database::instance().get_message_authors();
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get message authors: {}", e.what());
        return {};
    }
}

std::vector<db::MessageChannel> MessageCrawlerService::get_message_channels() const {
    try {
        return db::Database::instance().get_message_channels();
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get message channels: {}", e.what());
        return {};
    }
}

void MessageCrawlerService::trigger_stats_refresh() {
    stats_refresh_requested_.store(true);
    cv_.notify_all();
}

void MessageCrawlerService::set_config(const CrawlerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

CrawlerConfig MessageCrawlerService::get_config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void MessageCrawlerService::on_message_update(const dpp::message& msg) {
    // Skip excluded channels
    if (config_.excluded_channels.contains(msg.channel_id)) {
        return;
    }

    // Skip if not from target guild
    if (msg.guild_id != config_.target_guild_id) {
        return;
    }

    // Only update if the message exists in our database
    try {
        if (db::Database::instance().message_exists(msg.id)) {
            // Use current time as edited_at (Discord doesn't always provide edited_timestamp reliably)
            db::Database::instance().update_message(
                msg.id,
                msg.content,
                std::chrono::system_clock::now()
            );
            spdlog::debug("[Crawler] Updated edited message {}", msg.id.str());
        }
    } catch (const std::exception& e) {
        spdlog::debug("[Crawler] Failed to update edited message: {}", e.what());
    }
}

void MessageCrawlerService::on_new_message(const dpp::message& msg) {
    // Skip excluded channels
    if (config_.excluded_channels.contains(msg.channel_id)) {
        return;
    }

    // Skip if not from target guild
    if (msg.guild_id != config_.target_guild_id) {
        return;
    }

    // Store the message
    try {
        auto crawled = message_to_crawled(msg);
        db::Database::instance().store_message(crawled);

        // Update newest_message_id in progress
        auto progress = db::Database::instance().get_crawl_progress(msg.channel_id);
        if (progress) {
            if (msg.id > progress->newest_message_id) {
                progress->newest_message_id = msg.id;
                progress->total_messages++;
                progress->last_crawl = std::chrono::system_clock::now();
                db::Database::instance().save_crawl_progress(*progress);
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("[Crawler] Failed to store real-time message: {}", e.what());
    }
}

void MessageCrawlerService::crawler_thread() {
    spdlog::info("[Crawler] Crawler thread started");

    // Wait for bot to be ready (interruptible)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(5), [this] {
            return !running_.load();
        });
    }

    while (running_.load()) {
        try {
            // Discover channels in the guild
            discover_channels();

            // Get all channels to crawl
            auto progress_list = db::Database::instance().get_all_crawl_progress(config_.target_guild_id);

            // First pass: complete initial crawl for all channels
            for (const auto& progress : progress_list) {
                if (!running_.load()) break;

                // Skip excluded channels
                if (config_.excluded_channels.contains(progress.channel_id)) {
                    continue;
                }

                if (!progress.initial_crawl_complete) {
                    // Get channel name for logging
                    std::string channel_name = progress.channel_id.str();

                    crawl_channel_history(progress.channel_id, channel_name);
                }
            }

            // Second pass: fetch new messages for all channels
            for (const auto& progress : progress_list) {
                if (!running_.load()) break;

                // Skip excluded channels
                if (config_.excluded_channels.contains(progress.channel_id)) {
                    continue;
                }

                if (progress.initial_crawl_complete && progress.newest_message_id != 0) {
                    crawl_new_messages(progress.channel_id);
                }
            }

            // Update status
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto summary = db::Database::instance().get_crawl_status_summary(config_.target_guild_id);
                current_status_.total_channels = summary.total_channels;
                current_status_.completed_channels = summary.completed_channels;
                current_status_.total_messages = summary.total_messages;
                current_status_.is_crawling = false;
                current_status_.last_update = std::chrono::system_clock::now();
            }

            // Wait before next crawl cycle
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::minutes(1), [this] {
                return !running_.load();
            });

        } catch (const std::exception& e) {
            spdlog::error("[Crawler] Error in crawler thread: {}", e.what());
            // Interruptible error delay
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(30), [this] {
                return !running_.load();
            });
        }
    }

    spdlog::info("[Crawler] Crawler thread stopped");
}

void MessageCrawlerService::stats_worker_thread() {
    spdlog::info("[Crawler] Stats worker thread started");

    // Short initial delay before first calculation (interruptible)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(10), [this] {
            return !running_.load();
        });
    }

    // Initial stats calculation on startup
    if (running_.load()) {
        try {
            spdlog::info("[Crawler] Running initial stats calculation...");
            calculate_word_stats();
        } catch (const std::exception& e) {
            spdlog::error("[Crawler] Error in initial stats calculation: {}", e.what());
        }
    }

    while (running_.load()) {
        try {
            // Wait for interval or refresh request
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, config_.stats_update_interval, [this] {
                return !running_.load() || stats_refresh_requested_.load();
            });

            // Calculate stats on interval or refresh request
            if (running_.load()) {
                stats_refresh_requested_.store(false);
                calculate_word_stats();
            }

        } catch (const std::exception& e) {
            spdlog::error("[Crawler] Error in stats worker: {}", e.what());
            // Interruptible error delay
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::minutes(1), [this] {
                return !running_.load();
            });
        }
    }

    spdlog::info("[Crawler] Stats worker thread stopped");
}

void MessageCrawlerService::discover_channels() {
    spdlog::debug("[Crawler] Discovering channels in guild {}", config_.target_guild_id.str());

    // Use synchronous channel fetch via callback
    std::promise<dpp::channel_map> promise;
    auto future = promise.get_future();

    bot_.channels_get(config_.target_guild_id, [&promise](const dpp::confirmation_callback_t& callback) {
        if (callback.is_error()) {
            spdlog::error("[Crawler] Failed to get channels: {}", callback.get_error().message);
            promise.set_value({});
            return;
        }

        promise.set_value(callback.get<dpp::channel_map>());
    });

    auto channels = future.get();
    size_t new_channels = 0;

    std::vector<db::DiscordChannel> channels_to_cache;

    for (const auto& [id, channel] : channels) {
        // Skip excluded channels
        if (config_.excluded_channels.contains(id)) {
            continue;
        }

        // Only process text channels
        if (channel.get_type() != dpp::channel_type::CHANNEL_TEXT &&
            channel.get_type() != dpp::channel_type::CHANNEL_ANNOUNCEMENT) {
            continue;
        }

        // Cache channel info
        db::DiscordChannel dc;
        dc.channel_id = id;
        dc.guild_id = config_.target_guild_id;
        dc.channel_name = channel.name;
        dc.channel_type = static_cast<int>(channel.get_type());
        channels_to_cache.push_back(dc);

        // Check if we already have progress for this channel
        auto existing = db::Database::instance().get_crawl_progress(id);
        if (!existing) {
            // Create initial progress entry
            db::ChannelCrawlProgress progress;
            progress.channel_id = id;
            progress.guild_id = config_.target_guild_id;
            progress.total_messages = 0;
            progress.initial_crawl_complete = false;
            progress.last_crawl = std::chrono::system_clock::now();

            db::Database::instance().save_crawl_progress(progress);
            new_channels++;
        }
    }

    // Cache all discovered channels
    if (!channels_to_cache.empty()) {
        db::Database::instance().cache_channels_batch(channels_to_cache);
        spdlog::debug("[Crawler] Cached {} channel info records", channels_to_cache.size());
    }

    if (new_channels > 0) {
        spdlog::info("[Crawler] Discovered {} new channels", new_channels);
    }
}

void MessageCrawlerService::crawl_channel_history(dpp::snowflake channel_id, const std::string& channel_name) {
    spdlog::info("[Crawler] Starting historical crawl for channel {} ({})", channel_name, channel_id.str());

    // Update status
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_status_.is_crawling = true;
        current_status_.current_channel_id = channel_id;
        current_status_.current_channel_name = channel_name;
    }

    // Get current progress
    auto progress_opt = db::Database::instance().get_crawl_progress(channel_id);
    if (!progress_opt) {
        spdlog::warn("[Crawler] No progress entry for channel {}", channel_id.str());
        return;
    }

    auto progress = *progress_opt;
    dpp::snowflake before_id = progress.oldest_message_id;
    bool first_batch = (before_id == 0);

    while (running_.load()) {
        wait_for_rate_limit();

        // Fetch messages
        std::promise<dpp::message_map> promise;
        auto future = promise.get_future();

        // Track if this was a rate limit error
        std::atomic<bool> rate_limited{false};

        if (before_id == 0) {
            // First request - get newest messages
            bot_.messages_get(channel_id, 0, 0, 0, config_.messages_per_request,
                [&promise, &rate_limited](const dpp::confirmation_callback_t& callback) {
                    if (callback.is_error()) {
                        if (callback.get_error().message.find("rate limit") != std::string::npos) {
                            rate_limited.store(true);
                        }
                        spdlog::warn("[Crawler] Failed to get messages: {}", callback.get_error().message);
                        promise.set_value({});
                        return;
                    }
                    promise.set_value(callback.get<dpp::message_map>());
                });
        } else {
            // Subsequent request - get older messages
            bot_.messages_get(channel_id, 0, before_id, 0, config_.messages_per_request,
                [&promise, &rate_limited](const dpp::confirmation_callback_t& callback) {
                    if (callback.is_error()) {
                        if (callback.get_error().message.find("rate limit") != std::string::npos) {
                            rate_limited.store(true);
                        }
                        spdlog::warn("[Crawler] Failed to get messages: {}", callback.get_error().message);
                        promise.set_value({});
                        return;
                    }
                    promise.set_value(callback.get<dpp::message_map>());
                });
        }

        auto messages = future.get();

        // If rate limited, wait and retry
        if (rate_limited.load()) {
            spdlog::info("[Crawler] Rate limited, waiting {} seconds before retry...",
                        kRateLimitRetryDelay.count());
            std::this_thread::sleep_for(kRateLimitRetryDelay);
            continue;  // Retry the same request
        }

        if (messages.empty()) {
            // No more messages - mark channel as complete
            progress.initial_crawl_complete = true;
            progress.last_crawl = std::chrono::system_clock::now();
            db::Database::instance().save_crawl_progress(progress);

            spdlog::info("[Crawler] Completed historical crawl for channel {} ({} messages total)",
                        channel_name, progress.total_messages);
            break;
        }

        // Convert to CrawledMessage and store
        std::vector<db::CrawledMessage> batch;
        batch.reserve(messages.size());

        dpp::snowflake oldest_in_batch{UINT64_MAX};
        dpp::snowflake newest_in_batch{0};

        for (const auto& [msg_id, msg] : messages) {
            batch.push_back(message_to_crawled(msg));

            if (msg_id < oldest_in_batch) oldest_in_batch = msg_id;
            if (msg_id > newest_in_batch) newest_in_batch = msg_id;
        }

        // Store batch
        db::Database::instance().store_messages_batch(batch);

        // Update progress
        if (first_batch) {
            progress.newest_message_id = newest_in_batch;
            first_batch = false;
        }
        progress.oldest_message_id = oldest_in_batch;
        progress.total_messages += batch.size();
        progress.last_crawl = std::chrono::system_clock::now();
        db::Database::instance().save_crawl_progress(progress);

        before_id = oldest_in_batch;
        messages_crawled_session_ += batch.size();

        // Log progress periodically or if batch was small (nearing end)
        if (progress.total_messages % kProgressLogInterval < 100 || batch.size() < kSmallBatchThreshold) {
            spdlog::info("[Crawler] {} : {} messages crawled",
                        channel_name, progress.total_messages);
        }

        // If we got less than requested, we've reached the end
        if (messages.size() < config_.messages_per_request) {
            progress.initial_crawl_complete = true;
            progress.last_crawl = std::chrono::system_clock::now();
            db::Database::instance().save_crawl_progress(progress);

            spdlog::info("[Crawler] Completed historical crawl for channel {} ({} messages)",
                        channel_name, progress.total_messages);
            break;
        }
    }
}

void MessageCrawlerService::crawl_new_messages(dpp::snowflake channel_id) {
    auto progress_opt = db::Database::instance().get_crawl_progress(channel_id);
    if (!progress_opt || progress_opt->newest_message_id == 0) {
        return;
    }

    auto progress = *progress_opt;
    dpp::snowflake after_id = progress.newest_message_id;

    wait_for_rate_limit();

    // Fetch new messages
    std::promise<dpp::message_map> promise;
    auto future = promise.get_future();

    bot_.messages_get(channel_id, 0, 0, after_id, config_.messages_per_request,
        [&promise](const dpp::confirmation_callback_t& callback) {
            if (callback.is_error()) {
                promise.set_value({});
                return;
            }
            promise.set_value(callback.get<dpp::message_map>());
        });

    auto messages = future.get();

    if (messages.empty()) {
        return;
    }

    // Convert and store
    std::vector<db::CrawledMessage> batch;
    batch.reserve(messages.size());

    dpp::snowflake newest_in_batch{0};

    for (const auto& [msg_id, msg] : messages) {
        batch.push_back(message_to_crawled(msg));

        if (msg_id > newest_in_batch) newest_in_batch = msg_id;
    }

    db::Database::instance().store_messages_batch(batch);

    // Update progress
    progress.newest_message_id = newest_in_batch;
    progress.total_messages += batch.size();
    progress.last_crawl = std::chrono::system_clock::now();
    db::Database::instance().save_crawl_progress(progress);

    spdlog::debug("[Crawler] Channel {}: fetched {} new messages",
                 channel_id.str(), batch.size());
}

void MessageCrawlerService::calculate_word_stats() {
    spdlog::info("[Crawler] Starting word statistics calculation...");

    auto start = std::chrono::steady_clock::now();
    auto& db = db::Database::instance();

    // Check if this is incremental or full recalculation
    int64_t last_message_id = db.get_stats_last_message_id("word_stats");
    bool is_incremental = last_message_id > 0;

    if (!is_incremental) {
        // Full recalculation - clear stats first
        db.clear_word_stats();
        spdlog::info("[Crawler] Full word stats recalculation");
    } else {
        spdlog::info("[Crawler] Incremental word stats update from message_id {}", last_message_id);
    }

    // Process messages in batches to limit memory usage
    constexpr size_t kBatchSize = 50000;
    std::unordered_map<std::string, std::pair<size_t, std::string>> word_counts;
    size_t messages_processed = 0;
    size_t total_unique_words = 0;
    int64_t max_message_id = last_message_id;

    auto flush_batch = [&]() {
        if (word_counts.empty()) return;

        std::vector<std::tuple<std::string, size_t, std::string>> word_list;
        word_list.reserve(word_counts.size());

        for (const auto& [key, value] : word_counts) {
            auto pos = key.rfind('_');
            std::string word = key.substr(0, pos);
            word_list.emplace_back(word, value.first, value.second);
        }

        total_unique_words += word_counts.size();
        db.upsert_word_stats(word_list);

        // Clear to free memory
        word_counts.clear();
        spdlog::debug("[Crawler] Flushed {} words to DB, {} messages processed", word_list.size(), messages_processed);
    };

    // Process messages incrementally
    max_message_id = db.process_messages_incremental(last_message_id, [&](int64_t msg_id, const std::string& content) {
        count_words(content, word_counts);
        messages_processed++;

        // Flush every kBatchSize messages
        if (messages_processed % kBatchSize == 0) {
            flush_batch();
            if (!running_.load()) return false;
        }
        return running_.load();
    });

    if (!running_.load()) {
        spdlog::info("[Crawler] Word stats calculation interrupted");
        return;
    }

    // Flush remaining
    flush_batch();

    // Update last processed message ID
    if (max_message_id > last_message_id) {
        db.set_stats_last_message_id("word_stats", max_message_id);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    spdlog::info("[Crawler] Word stats calculated: {} messages, ~{} unique words in {}ms ({})",
                messages_processed, total_unique_words, elapsed_ms,
                is_incremental ? "incremental" : "full");

    // Calculate phrase stats (also incremental)
    calculate_phrase_stats();
}

std::string MessageCrawlerService::detect_language(const std::string& word) const {
    size_t cyrillic_count = 0;
    size_t latin_count = 0;

    for (size_t i = 0; i < word.length();) {
        unsigned char c = word[i];

        // UTF-8 Cyrillic range (U+0400-U+04FF)
        if (c >= 0xD0 && c <= 0xD3 && i + 1 < word.length()) {
            cyrillic_count++;
            i += 2;
        }
        // ASCII Latin
        else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            latin_count++;
            i++;
        }
        else {
            i++;
        }
    }

    if (cyrillic_count > latin_count) return "ru";
    if (latin_count > cyrillic_count) return "en";
    return "unknown";
}

std::vector<std::string> MessageCrawlerService::tokenize(const std::string& content) const {
    std::vector<std::string> words;

    // Remove URLs
    static const std::regex url_regex(R"(https?://\S+)", std::regex::optimize);
    std::string cleaned = std::regex_replace(content, url_regex, " ");

    // Remove Discord mentions/emojis
    static const std::regex discord_regex(R"(<[@#!:][^>]+>)", std::regex::optimize);
    cleaned = std::regex_replace(cleaned, discord_regex, " ");

    // Split by non-word characters (keeping Cyrillic)
    std::string current_word;

    for (size_t i = 0; i < cleaned.length();) {
        unsigned char c = cleaned[i];

        bool is_word_char = false;

        // ASCII alphanumeric
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            is_word_char = true;
            current_word += static_cast<char>(std::tolower(c));
            i++;
        }
        // UTF-8 Cyrillic (2-byte sequences starting with D0-D3)
        else if (c >= 0xD0 && c <= 0xD3 && i + 1 < cleaned.length()) {
            is_word_char = true;
            // Convert to lowercase for Cyrillic
            unsigned char c2 = cleaned[i + 1];

            // Basic lowercase conversion for Russian
            if (c == 0xD0 && c2 >= 0x90 && c2 <= 0x9F) {
                // А-П -> а-п
                current_word += static_cast<char>(c);
                current_word += static_cast<char>(c2 + 0x20);
            } else if (c == 0xD0 && c2 >= 0xA0 && c2 <= 0xAF) {
                // Р-Я -> р-я
                current_word += static_cast<char>(0xD1);
                current_word += static_cast<char>(c2 - 0x20);
            } else {
                current_word += cleaned.substr(i, 2);
            }
            i += 2;
        }
        else {
            // Not a word character - end current word
            if (!current_word.empty()) {
                words.push_back(current_word);
                current_word.clear();
            }
            i++;
        }
    }

    // Don't forget last word
    if (!current_word.empty()) {
        words.push_back(current_word);
    }

    return words;
}

bool MessageCrawlerService::is_valid_word(const std::string& word) const {
    // Count UTF-8 characters (not bytes)
    // Leading bytes are NOT continuation bytes (0x80-0xBF)
    // Continuation bytes have pattern 10xxxxxx (0x80-0xBF)
    size_t char_count = 0;
    for (unsigned char c : word) {
        if ((c & 0xC0) != 0x80) {
            char_count++;
        }
    }

    if (char_count < config_.min_word_length) {
        return false;
    }

    // Check if it's all digits
    bool all_digits = true;
    for (char c : word) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            all_digits = false;
            break;
        }
    }
    if (all_digits) {
        return false;
    }

    return true;
}

db::CrawledMessage MessageCrawlerService::message_to_crawled(const dpp::message& msg) const {
    db::CrawledMessage crawled;
    crawled.message_id = msg.id;
    crawled.channel_id = msg.channel_id;
    crawled.author_id = msg.author.id;
    crawled.content = msg.content;
    crawled.created_at = std::chrono::system_clock::from_time_t(
        static_cast<time_t>(msg.id.get_creation_time()));
    crawled.is_bot = msg.author.is_bot();

    // Reply tracking
    if (msg.message_reference.message_id != 0) {
        crawled.reply_to_message_id = msg.message_reference.message_id;
    }

    // Attachments
    if (!msg.attachments.empty()) {
        crawled.has_attachments = true;
        for (const auto& attachment : msg.attachments) {
            crawled.attachment_urls.push_back(attachment.url);
        }
    }

    // Cache author info
    try {
        db::DiscordUser user;
        user.user_id = msg.author.id;
        user.username = msg.author.username;
        user.global_name = msg.author.global_name;
        user.avatar_hash = msg.author.avatar.to_string();
        user.is_bot = msg.author.is_bot();
        db::Database::instance().cache_discord_user(user);
    } catch (const std::exception& e) {
        spdlog::debug("[Crawler] Failed to cache user {}: {}", msg.author.id.str(), e.what());
    }

    return crawled;
}

void MessageCrawlerService::wait_for_rate_limit() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - last_request_time_;
    if (elapsed < config_.request_delay) {
        std::this_thread::sleep_for(config_.request_delay - elapsed);
    }
    last_request_time_ = std::chrono::steady_clock::now();
}

void MessageCrawlerService::count_words(
    const std::string& content,
    std::unordered_map<std::string, std::pair<size_t, std::string>>& word_counts,
    const std::string& language_filter,
    const std::unordered_set<std::string>* stopwords
) const {
    auto words = tokenize(content);

    for (const auto& word : words) {
        if (!is_valid_word(word)) {
            continue;
        }

        // Filter by stopwords if provided
        if (stopwords && stopwords->contains(word)) {
            continue;
        }

        std::string lang = detect_language(word);

        // Filter by language if specified
        if (!language_filter.empty() && lang != language_filter) {
            continue;
        }

        auto key = word + "_" + lang;
        auto it = word_counts.find(key);
        if (it == word_counts.end()) {
            word_counts[key] = {1, lang};
        } else {
            it->second.first++;
        }
    }
}

// ============================================================================
// Phrase statistics
// ============================================================================

std::vector<db::PhraseStatEntry> MessageCrawlerService::get_top_phrases(
    size_t limit,
    const std::string& language,
    bool sort_by_pmi,
    int word_count_filter,
    size_t min_count
) const {
    return db::Database::instance().get_top_phrases(limit, language, sort_by_pmi, word_count_filter, min_count);
}

size_t MessageCrawlerService::get_unique_phrase_count(const std::string& language, int word_count_filter) const {
    return db::Database::instance().get_unique_phrase_count(language, word_count_filter);
}

std::vector<db::PhraseStatEntry> MessageCrawlerService::get_user_top_phrases(
    dpp::snowflake user_id,
    size_t limit,
    const std::string& language,
    bool sort_by_pmi,
    int word_count_filter,
    size_t min_count,
    dpp::snowflake channel_id,
    const std::string& sort_mode
) const {
    try {
        // Calculate phrase stats on-the-fly for this user
        std::unordered_map<std::string, std::tuple<std::vector<std::string>, size_t, std::string>> phrase_counts;
        std::unordered_map<std::string, std::pair<size_t, std::string>> word_counts;
        size_t total_bigrams = 0;
        size_t user_total_phrases = 0;

        auto process_content = [&](const std::string& content) {
            auto tokens = tokenize(content);

            // Count words for PMI calculation
            for (const auto& word : tokens) {
                if (!is_valid_word(word)) continue;
                std::string lang = detect_language(word);
                if (!language.empty() && lang != language) continue;

                auto key = word + "_" + lang;
                auto it = word_counts.find(key);
                if (it == word_counts.end()) {
                    word_counts[key] = {1, lang};
                } else {
                    it->second.first++;
                }
            }

            // Extract phrases
            extract_phrases(tokens, phrase_counts);
            if (tokens.size() >= 2) {
                total_bigrams += tokens.size() - 1;
            }
            return true;
        };

        // Use channel-specific or all-channel processing
        if (channel_id != 0) {
            db::Database::instance().process_user_channel_messages(user_id, channel_id, process_content);
        } else {
            db::Database::instance().process_user_messages(user_id, process_content);
        }

        // Get global phrase frequencies for uniqueness calculation
        auto global_frequencies = db::Database::instance().get_global_phrase_frequencies(language, word_count_filter, 1);

        // Calculate global total
        size_t global_total = 0;
        for (const auto& [_, count] : global_frequencies) {
            global_total += count;
        }

        // Calculate user total phrases
        for (const auto& [_, tuple] : phrase_counts) {
            user_total_phrases += std::get<1>(tuple);
        }

        // Filter by language and word count, apply min_count
        std::vector<db::PhraseStatEntry> entries;
        entries.reserve(phrase_counts.size());

        // Calculate total words for PMI
        size_t total_words = 0;
        for (const auto& [_, pair] : word_counts) {
            total_words += pair.first;
        }

        for (const auto& [key, tuple] : phrase_counts) {
            const auto& words = std::get<0>(tuple);
            size_t count = std::get<1>(tuple);
            const auto& lang = std::get<2>(tuple);

            // Apply filters
            if (count < min_count) continue;
            if (!language.empty() && lang != language) continue;
            if (word_count_filter > 0 && static_cast<int>(words.size()) != word_count_filter) continue;

            // Extract phrase from key
            auto pos = key.rfind('_');
            std::string phrase = key.substr(0, pos);

            db::PhraseStatEntry entry;
            entry.phrase = phrase;
            entry.words = words;
            entry.word_count = static_cast<int>(words.size());
            entry.count = count;
            entry.language = lang;

            // Calculate PMI and NPMI
            if (total_words > 0 && total_bigrams > 0 && count >= 5) {
                double pmi = 0.0;
                double p_xy = 0.0;

                if (words.size() == 2) {
                    auto it1 = word_counts.find(words[0] + "_" + lang);
                    auto it2 = word_counts.find(words[1] + "_" + lang);
                    if (it1 != word_counts.end() && it2 != word_counts.end()) {
                        p_xy = static_cast<double>(count) / total_bigrams;
                        double p_x = static_cast<double>(it1->second.first) / total_words;
                        double p_y = static_cast<double>(it2->second.first) / total_words;
                        if (p_x > 0 && p_y > 0 && p_xy > 0) {
                            pmi = std::log2(p_xy / (p_x * p_y));
                        }
                    }
                } else if (words.size() == 3) {
                    auto it1 = word_counts.find(words[0] + "_" + lang);
                    auto it2 = word_counts.find(words[1] + "_" + lang);
                    auto it3 = word_counts.find(words[2] + "_" + lang);
                    if (it1 != word_counts.end() && it2 != word_counts.end() && it3 != word_counts.end()) {
                        p_xy = static_cast<double>(count) / total_bigrams;
                        double p_x = static_cast<double>(it1->second.first) / total_words;
                        double p_y = static_cast<double>(it2->second.first) / total_words;
                        double p_z = static_cast<double>(it3->second.first) / total_words;
                        if (p_x > 0 && p_y > 0 && p_z > 0 && p_xy > 0) {
                            pmi = std::log2(p_xy / (p_x * p_y * p_z));
                        }
                    }
                }
                entry.pmi_score = pmi;

                // Calculate NPMI
                if (p_xy > 0) {
                    double neg_log_p_xy = -std::log2(p_xy);
                    if (neg_log_p_xy > 0) {
                        entry.npmi_score = std::max(-1.0, std::min(1.0, pmi / neg_log_p_xy));
                    }
                }
            }

            // Calculate uniqueness_score
            // uniqueness = (user_freq / user_total) / (global_freq / global_total)
            //            = (user_freq * global_total) / (global_freq * user_total)
            if (user_total_phrases > 0 && global_total > 0) {
                auto global_it = global_frequencies.find(phrase);
                if (global_it != global_frequencies.end() && global_it->second > 0) {
                    double uniqueness = (static_cast<double>(count) * global_total) /
                                       (static_cast<double>(global_it->second) * user_total_phrases);
                    entry.uniqueness_score = uniqueness;
                } else {
                    // Phrase not in global stats - very unique!
                    entry.uniqueness_score = 100.0;  // High uniqueness value
                }
            }

            entries.push_back(std::move(entry));
        }

        // Sort based on mode
        if (sort_mode == "uniqueness") {
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                double u_a = a.uniqueness_score.value_or(0.0);
                double u_b = b.uniqueness_score.value_or(0.0);
                return u_a > u_b;
            });
        } else if (sort_mode == "npmi") {
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                double npmi_a = a.npmi_score.value_or(-999.0);
                double npmi_b = b.npmi_score.value_or(-999.0);
                return npmi_a > npmi_b;
            });
        } else if (sort_by_pmi || sort_mode == "pmi") {
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                double pmi_a = a.pmi_score.value_or(-999.0);
                double pmi_b = b.pmi_score.value_or(-999.0);
                return pmi_a > pmi_b;
            });
        } else {
            // Default: sort by count
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.count > b.count;
            });
        }

        // Limit results
        if (entries.size() > limit) {
            entries.resize(limit);
        }

        return entries;
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get user top phrases: {}", e.what());
        return {};
    }
}

std::vector<db::PhraseStatEntry> MessageCrawlerService::get_channel_top_phrases(
    dpp::snowflake channel_id,
    size_t limit,
    const std::string& language,
    bool sort_by_pmi,
    int word_count_filter,
    size_t min_count
) const {
    try {
        // Calculate phrase stats on-the-fly for this channel
        std::unordered_map<std::string, std::tuple<std::vector<std::string>, size_t, std::string>> phrase_counts;
        std::unordered_map<std::string, std::pair<size_t, std::string>> word_counts;
        size_t total_bigrams = 0;

        db::Database::instance().process_channel_messages(channel_id, [&](const std::string& content) {
            auto tokens = tokenize(content);

            // Count words for PMI calculation
            for (const auto& word : tokens) {
                if (!is_valid_word(word)) continue;
                std::string lang = detect_language(word);
                if (!language.empty() && lang != language) continue;

                auto key = word + "_" + lang;
                auto it = word_counts.find(key);
                if (it == word_counts.end()) {
                    word_counts[key] = {1, lang};
                } else {
                    it->second.first++;
                }
            }

            // Extract phrases
            extract_phrases(tokens, phrase_counts);
            if (tokens.size() >= 2) {
                total_bigrams += tokens.size() - 1;
            }
            return true;
        });

        // Filter by language and word count, apply min_count
        std::vector<db::PhraseStatEntry> entries;
        entries.reserve(phrase_counts.size());

        // Calculate total words for PMI
        size_t total_words = 0;
        for (const auto& [_, pair] : word_counts) {
            total_words += pair.first;
        }

        for (const auto& [key, tuple] : phrase_counts) {
            const auto& words = std::get<0>(tuple);
            size_t count = std::get<1>(tuple);
            const auto& lang = std::get<2>(tuple);

            // Apply filters
            if (count < min_count) continue;
            if (!language.empty() && lang != language) continue;
            if (word_count_filter > 0 && static_cast<int>(words.size()) != word_count_filter) continue;

            // Extract phrase from key
            auto pos = key.rfind('_');
            std::string phrase = key.substr(0, pos);

            db::PhraseStatEntry entry;
            entry.phrase = phrase;
            entry.words = words;
            entry.word_count = static_cast<int>(words.size());
            entry.count = count;
            entry.language = lang;

            // Calculate PMI
            if (total_words > 0 && total_bigrams > 0 && count >= 5) {
                double pmi = 0.0;
                if (words.size() == 2) {
                    auto it1 = word_counts.find(words[0] + "_" + lang);
                    auto it2 = word_counts.find(words[1] + "_" + lang);
                    if (it1 != word_counts.end() && it2 != word_counts.end()) {
                        double p_xy = static_cast<double>(count) / total_bigrams;
                        double p_x = static_cast<double>(it1->second.first) / total_words;
                        double p_y = static_cast<double>(it2->second.first) / total_words;
                        if (p_x > 0 && p_y > 0 && p_xy > 0) {
                            pmi = std::log2(p_xy / (p_x * p_y));
                        }
                    }
                } else if (words.size() == 3) {
                    auto it1 = word_counts.find(words[0] + "_" + lang);
                    auto it2 = word_counts.find(words[1] + "_" + lang);
                    auto it3 = word_counts.find(words[2] + "_" + lang);
                    if (it1 != word_counts.end() && it2 != word_counts.end() && it3 != word_counts.end()) {
                        double p_xyz = static_cast<double>(count) / total_bigrams;
                        double p_x = static_cast<double>(it1->second.first) / total_words;
                        double p_y = static_cast<double>(it2->second.first) / total_words;
                        double p_z = static_cast<double>(it3->second.first) / total_words;
                        if (p_x > 0 && p_y > 0 && p_z > 0 && p_xyz > 0) {
                            pmi = std::log2(p_xyz / (p_x * p_y * p_z));
                        }
                    }
                }
                entry.pmi_score = pmi;
            }

            entries.push_back(std::move(entry));
        }

        // Sort by PMI or count
        if (sort_by_pmi) {
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                double pmi_a = a.pmi_score.value_or(-999.0);
                double pmi_b = b.pmi_score.value_or(-999.0);
                return pmi_a > pmi_b;
            });
        } else {
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.count > b.count;
            });
        }

        // Limit results
        if (entries.size() > limit) {
            entries.resize(limit);
        }

        return entries;
    } catch (const std::exception& e) {
        spdlog::error("[Crawler] Failed to get channel top phrases: {}", e.what());
        return {};
    }
}

void MessageCrawlerService::extract_phrases(
    const std::vector<std::string>& tokens,
    std::unordered_map<std::string, std::tuple<std::vector<std::string>, size_t, std::string>>& phrase_counts
) const {
    // Track phrases seen in this message to count each only once
    std::unordered_set<std::string> seen_in_message;

    // Bigrams
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        std::vector<std::string> words = {tokens[i], tokens[i+1]};
        if (!is_valid_phrase(words)) continue;

        std::string phrase = tokens[i] + " " + tokens[i+1];
        std::string lang = detect_language(phrase);
        std::string key = phrase + "_" + lang;

        // Only count once per message
        if (seen_in_message.count(key)) continue;
        seen_in_message.insert(key);

        auto& entry = phrase_counts[key];
        std::get<0>(entry) = words;
        std::get<1>(entry)++;
        std::get<2>(entry) = lang;
    }

    // Trigrams
    for (size_t i = 0; i + 2 < tokens.size(); i++) {
        std::vector<std::string> words = {tokens[i], tokens[i+1], tokens[i+2]};
        if (!is_valid_phrase(words)) continue;

        std::string phrase = tokens[i] + " " + tokens[i+1] + " " + tokens[i+2];
        std::string lang = detect_language(phrase);
        std::string key = phrase + "_" + lang;

        // Only count once per message
        if (seen_in_message.count(key)) continue;
        seen_in_message.insert(key);

        auto& entry = phrase_counts[key];
        std::get<0>(entry) = words;
        std::get<1>(entry)++;
        std::get<2>(entry) = lang;
    }
}

bool MessageCrawlerService::is_valid_phrase(const std::vector<std::string>& words) const {
    if (words.empty()) return false;

    // Each word must be valid
    for (const auto& w : words) {
        if (!is_valid_word(w)) return false;
    }

    // First and last words must NOT be stopwords
    // (middle of trigram can be a stopword: "кот на диване")
    if (stopwords_.contains(words.front()) || stopwords_.contains(words.back())) {
        return false;
    }

    // NEW: Check word blacklist
    for (const auto& w : words) {
        if (blacklist_.contains(w)) {
            return false;
        }
    }

    // NEW: Each word must contain at least one letter (not pure numbers/symbols)
    for (const auto& w : words) {
        bool has_letter = false;
        for (size_t i = 0; i < w.length();) {
            unsigned char c = w[i];
            // ASCII letter
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                has_letter = true;
                break;
            }
            // UTF-8 Cyrillic (starts with D0-D3)
            if (c >= 0xD0 && c <= 0xD3 && i + 1 < w.length()) {
                has_letter = true;
                break;
            }
            // Move to next character
            if (c < 0x80) i++;
            else if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i++;
        }
        if (!has_letter) {
            return false;
        }
    }

    // NEW: First and last words should have at least 2 characters
    // (allows short words in the middle like "кот на диване")
    auto count_utf8_chars = [](const std::string& s) -> size_t {
        size_t count = 0;
        for (unsigned char c : s) {
            if ((c & 0xC0) != 0x80) count++;
        }
        return count;
    };

    if (count_utf8_chars(words.front()) < 2 || count_utf8_chars(words.back()) < 2) {
        return false;
    }

    // Reject phrases with all identical words ("конюшня конюшня конюшня")
    if (words.size() >= 2) {
        bool all_same = true;
        for (size_t i = 1; i < words.size(); i++) {
            if (words[i] != words[0]) {
                all_same = false;
                break;
            }
        }
        if (all_same) return false;
    }

    return true;
}

namespace {
    // Helper function to calculate Log-Likelihood Ratio (G²)
    double calculate_llr_score(size_t count_xy, size_t count_x, size_t count_y, size_t total) {
        // Handle edge cases
        if (count_xy == 0 || count_x < count_xy || count_y < count_xy || total == 0) {
            return 0.0;
        }

        size_t o11 = count_xy;                              // phrase count
        size_t o12 = count_x - count_xy;                    // x without y
        size_t o21 = count_y - count_xy;                    // y without x
        size_t o22 = total - count_x - count_y + count_xy;  // neither x nor y

        auto g = [](size_t observed, double expected) -> double {
            if (observed == 0 || expected <= 0) return 0.0;
            return static_cast<double>(observed) * std::log(static_cast<double>(observed) / expected);
        };

        size_t row1 = o11 + o12;
        size_t row2 = o21 + o22;
        size_t col1 = o11 + o21;
        size_t col2 = o12 + o22;
        double n = static_cast<double>(total);

        if (row1 == 0 || row2 == 0 || col1 == 0 || col2 == 0) {
            return 0.0;
        }

        double e11 = static_cast<double>(row1) * static_cast<double>(col1) / n;
        double e12 = static_cast<double>(row1) * static_cast<double>(col2) / n;
        double e21 = static_cast<double>(row2) * static_cast<double>(col1) / n;
        double e22 = static_cast<double>(row2) * static_cast<double>(col2) / n;

        double llr = 2.0 * (g(o11, e11) + g(o12, e12) + g(o21, e21) + g(o22, e22));
        return llr;
    }
}

void MessageCrawlerService::calculate_pmi_scores_from_db() {
    spdlog::info("[Crawler] Calculating PMI/NPMI/LLR scores from database...");

    auto& db = db::Database::instance();

    // Load word stats from database
    auto word_counts = db.get_word_stats_map();
    size_t total_words = db.get_total_word_count();
    size_t total_bigrams = db.get_total_bigram_count();

    if (total_words == 0 || total_bigrams == 0) {
        spdlog::warn("[Crawler] No word/phrase data for PMI calculation");
        return;
    }

    spdlog::info("[Crawler] Loaded {} word entries, {} total words, {} total bigrams",
                word_counts.size(), total_words, total_bigrams);

    // Load phrases for PMI calculation
    auto phrases = db.get_phrases_for_pmi(5);

    if (!running_.load()) return;

    // Structure to hold all scores for batch update
    struct PhraseScores {
        std::string phrase;
        std::string language;
        double pmi{0.0};
        double npmi{0.0};
        double llr{0.0};
    };

    std::vector<PhraseScores> score_updates;
    score_updates.reserve(phrases.size());

    for (const auto& p : phrases) {
        if (!running_.load()) return;

        PhraseScores scores;
        scores.phrase = p.phrase;
        scores.language = p.language;

        if (p.words.size() == 2) {
            // Bigram: PMI(x,y) = log2(P(xy) / (P(x) * P(y)))
            auto it1 = word_counts.find(p.words[0] + "_" + p.language);
            auto it2 = word_counts.find(p.words[1] + "_" + p.language);

            if (it1 != word_counts.end() && it2 != word_counts.end()) {
                double p_xy = static_cast<double>(p.count) / total_bigrams;
                double p_x = static_cast<double>(it1->second) / total_words;
                double p_y = static_cast<double>(it2->second) / total_words;

                if (p_x > 0 && p_y > 0 && p_xy > 0) {
                    // PMI
                    scores.pmi = std::log2(p_xy / (p_x * p_y));

                    // NPMI = PMI / (-log2(P(xy)))
                    double neg_log_p_xy = -std::log2(p_xy);
                    if (neg_log_p_xy > 0) {
                        scores.npmi = scores.pmi / neg_log_p_xy;
                        scores.npmi = std::max(-1.0, std::min(1.0, scores.npmi));
                    }

                    // LLR (Log-Likelihood Ratio)
                    scores.llr = calculate_llr_score(
                        p.count, it1->second, it2->second, total_bigrams
                    );
                }
            }
        } else if (p.words.size() == 3) {
            // Trigram
            auto it1 = word_counts.find(p.words[0] + "_" + p.language);
            auto it2 = word_counts.find(p.words[1] + "_" + p.language);
            auto it3 = word_counts.find(p.words[2] + "_" + p.language);

            if (it1 != word_counts.end() && it2 != word_counts.end() && it3 != word_counts.end()) {
                double p_xyz = static_cast<double>(p.count) / total_bigrams;
                double p_x = static_cast<double>(it1->second) / total_words;
                double p_y = static_cast<double>(it2->second) / total_words;
                double p_z = static_cast<double>(it3->second) / total_words;

                if (p_x > 0 && p_y > 0 && p_z > 0 && p_xyz > 0) {
                    scores.pmi = std::log2(p_xyz / (p_x * p_y * p_z));

                    double neg_log_p_xyz = -std::log2(p_xyz);
                    if (neg_log_p_xyz > 0) {
                        scores.npmi = scores.pmi / neg_log_p_xyz;
                        scores.npmi = std::max(-1.0, std::min(1.0, scores.npmi));
                    }

                    double llr_xy = calculate_llr_score(p.count, it1->second, it2->second, total_bigrams);
                    double llr_yz = calculate_llr_score(p.count, it2->second, it3->second, total_bigrams);
                    scores.llr = (llr_xy + llr_yz) / 2.0;
                }
            }
        }

        score_updates.push_back(scores);
    }

    // Free memory - word counts no longer needed
    word_counts.clear();

    if (!running_.load()) return;

    // Batch update all scores in DB
    if (!score_updates.empty()) {
        std::vector<std::tuple<std::string, std::string, double>> pmi_updates;
        pmi_updates.reserve(score_updates.size());
        for (const auto& s : score_updates) {
            pmi_updates.emplace_back(s.phrase, s.language, s.pmi);
        }
        db.update_phrase_pmi_scores(pmi_updates);

        std::vector<std::tuple<std::string, std::string, double, double>> npmi_llr_updates;
        npmi_llr_updates.reserve(score_updates.size());
        for (const auto& s : score_updates) {
            npmi_llr_updates.emplace_back(s.phrase, s.language, s.npmi, s.llr);
        }
        db.update_phrase_npmi_llr_scores(npmi_llr_updates);

        spdlog::info("[Crawler] Updated PMI/NPMI/LLR scores for {} phrases", score_updates.size());
    }
}

void MessageCrawlerService::calculate_phrase_stats() {
    spdlog::info("[Crawler] Starting phrase statistics calculation...");

    auto start = std::chrono::steady_clock::now();
    auto& db = db::Database::instance();

    // Check if this is incremental or full recalculation
    int64_t last_message_id = db.get_stats_last_message_id("phrase_stats");
    bool is_incremental = last_message_id > 0;

    if (!is_incremental) {
        // Full recalculation - clear stats first
        db.clear_phrase_stats();
        spdlog::info("[Crawler] Full phrase stats recalculation");
    } else {
        spdlog::info("[Crawler] Incremental phrase stats update from message_id {}", last_message_id);
    }

    // Process messages in batches to limit memory usage
    constexpr size_t kBatchSize = 50000;
    std::unordered_map<std::string, std::tuple<std::vector<std::string>, size_t, std::string>> phrase_counts;
    size_t messages_processed = 0;
    size_t total_unique_phrases = 0;
    int64_t max_message_id = last_message_id;

    auto flush_batch = [&]() {
        if (phrase_counts.empty()) return;

        std::vector<std::tuple<std::string, std::vector<std::string>, int, size_t, std::string>> phrase_list;
        phrase_list.reserve(phrase_counts.size());

        for (const auto& [key, tuple] : phrase_counts) {
            auto pos = key.rfind('_');
            std::string phrase = key.substr(0, pos);
            const auto& words = std::get<0>(tuple);
            size_t count = std::get<1>(tuple);
            const auto& lang = std::get<2>(tuple);
            phrase_list.emplace_back(phrase, words, static_cast<int>(words.size()), count, lang);
        }

        total_unique_phrases += phrase_counts.size();
        db.upsert_phrase_stats(phrase_list);

        // Clear to free memory
        phrase_counts.clear();
        spdlog::debug("[Crawler] Flushed {} phrases to DB, {} messages processed", phrase_list.size(), messages_processed);
    };

    // Process messages incrementally
    max_message_id = db.process_messages_incremental(last_message_id, [&](int64_t msg_id, const std::string& content) {
        auto tokens = tokenize(content);
        extract_phrases(tokens, phrase_counts);
        messages_processed++;

        // Flush every kBatchSize messages
        if (messages_processed % kBatchSize == 0) {
            flush_batch();
            if (!running_.load()) return false;
        }
        return running_.load();
    });

    if (!running_.load()) {
        spdlog::info("[Crawler] Phrase stats calculation interrupted");
        return;
    }

    // Flush remaining
    flush_batch();

    // Update last processed message ID
    if (max_message_id > last_message_id) {
        db.set_stats_last_message_id("phrase_stats", max_message_id);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    spdlog::info("[Crawler] Phrase stats calculated: {} messages, ~{} unique phrases in {}ms ({})",
                messages_processed, total_unique_phrases, elapsed_ms,
                is_incremental ? "incremental" : "full");

    // Calculate PMI scores from database
    if (!running_.load()) return;
    calculate_pmi_scores_from_db();
}

// ============================================================================
// Lemmatizer service management
// ============================================================================

void MessageCrawlerService::start_lemmatizer_service() {
    // Check if already running
    if (is_lemmatizer_running()) {
        spdlog::info("[Crawler] Lemmatizer service already running");
        return;
    }

    // Find the lemmatizer script and venv
    std::filesystem::path script_dir;
    std::vector<std::filesystem::path> search_paths = {
        "scripts",
        "../scripts",
        "/home/nisemonic/patchouli/bot/scripts"
    };

    for (const auto& p : search_paths) {
        if (std::filesystem::exists(p / "lemmatizer_service.py") &&
            std::filesystem::exists(p / "venv" / "bin" / "python3")) {
            script_dir = p;
            break;
        }
    }

    if (script_dir.empty()) {
        spdlog::warn("[Crawler] Lemmatizer service not found. Run 'cmake -B build' to set it up.");
        return;
    }

    auto python_path = std::filesystem::absolute(script_dir / "venv" / "bin" / "python3");
    auto script_path = std::filesystem::absolute(script_dir / "lemmatizer_service.py");

    spdlog::info("[Crawler] Starting lemmatizer service: {} {}", python_path.string(), script_path.string());

    // Fork and exec
    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("[Crawler] Failed to fork lemmatizer process: {}", strerror(errno));
        return;
    }

    if (pid == 0) {
        // Child process
        // Redirect stdout/stderr to /dev/null or a log file
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }

        // Close all inherited fds (except stdin/stdout/stderr) to avoid
        // holding the Crow listening socket after parent restarts
        for (int fd = 3; fd < 1024; ++fd) close(fd);

        // Create new session to detach from terminal
        setsid();

        // Execute the lemmatizer
        execl(python_path.c_str(), python_path.c_str(),
              script_path.c_str(),
              "--socket", lemmatizer_socket_path_.c_str(),
              nullptr);

        // If exec fails
        _exit(1);
    }

    // Parent process
    lemmatizer_pid_ = pid;
    spdlog::info("[Crawler] Lemmatizer service started with PID {}", pid);

    // Wait a bit for the service to start and create the socket
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (std::filesystem::exists(lemmatizer_socket_path_)) {
            spdlog::info("[Crawler] Lemmatizer socket ready");
            return;
        }
    }

    spdlog::warn("[Crawler] Lemmatizer socket not created after 3 seconds");
}

void MessageCrawlerService::stop_lemmatizer_service() {
    if (lemmatizer_pid_ > 0) {
        spdlog::info("[Crawler] Stopping lemmatizer service (PID {})", lemmatizer_pid_);

        // Send SIGTERM
        kill(lemmatizer_pid_, SIGTERM);

        // Wait for process to exit (with timeout)
        for (int i = 0; i < 30; ++i) {
            int status;
            pid_t result = waitpid(lemmatizer_pid_, &status, WNOHANG);
            if (result == lemmatizer_pid_) {
                spdlog::info("[Crawler] Lemmatizer service stopped");
                lemmatizer_pid_ = 0;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Force kill if still running
        spdlog::warn("[Crawler] Lemmatizer service did not stop, sending SIGKILL");
        kill(lemmatizer_pid_, SIGKILL);
        waitpid(lemmatizer_pid_, nullptr, 0);
        lemmatizer_pid_ = 0;
    }

    // Clean up socket file
    if (std::filesystem::exists(lemmatizer_socket_path_)) {
        std::filesystem::remove(lemmatizer_socket_path_);
    }
}

bool MessageCrawlerService::is_lemmatizer_running() const {
    // Check if socket exists
    if (!std::filesystem::exists(lemmatizer_socket_path_)) {
        return false;
    }

    // Try to connect to verify it's actually running
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, lemmatizer_socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    bool connected = (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    close(sock);

    return connected;
}

std::vector<std::string> MessageCrawlerService::lemmatize_words(const std::vector<std::string>& words) const {
    if (words.empty()) {
        return {};
    }

    if (!is_lemmatizer_running()) {
        // Return original words if lemmatizer not available
        return words;
    }

    try {
        // Connect to lemmatizer socket
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            return words;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, lemmatizer_socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(sock);
            return words;
        }

        // Send request
        nlohmann::json request;
        request["words"] = words;
        std::string request_str = request.dump();

        if (send(sock, request_str.c_str(), request_str.size(), 0) < 0) {
            close(sock);
            return words;
        }

        // Receive response
        char buffer[65536];
        std::string response_str;
        ssize_t bytes_read;

        // Set receive timeout
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while ((bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[bytes_read] = '\0';
            response_str += buffer;
        }

        close(sock);

        if (response_str.empty()) {
            return words;
        }

        // Parse response
        auto response = nlohmann::json::parse(response_str);
        if (response.contains("lemmas") && response["lemmas"].is_array()) {
            std::vector<std::string> lemmas;
            for (const auto& lemma : response["lemmas"]) {
                lemmas.push_back(lemma.get<std::string>());
            }
            return lemmas;
        }

        return words;
    } catch (const std::exception& e) {
        spdlog::debug("[Crawler] Lemmatization failed: {}", e.what());
        return words;
    }
}

} // namespace services
