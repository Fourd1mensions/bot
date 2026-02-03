#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <vector>

namespace services {

/**
 * Embed field for Discord webhook messages
 */
struct WebhookField {
  std::string name;
  std::string value;
  bool inline_field = false;
};

/**
 * Notification severity levels with corresponding colors
 */
enum class NotificationLevel {
  Info,     // Blue - 0x3498DB
  Success,  // Green - 0x2ECC71
  Warning,  // Yellow - 0xF1C40F
  Error     // Red - 0xE74C3C
};

/**
 * Webhook channels for different notification types
 */
enum class WebhookChannel {
  MirrorErrors,   // Beatmap mirror failures
  General,        // General notifications
  Debug           // Debug/dev notifications
};

/**
 * Universal webhook notification service for Discord.
 * Thread-safe, non-blocking (sends in background threads).
 */
class WebhookService {
public:
  WebhookService() = default;

  /**
   * Configure a webhook URL for a specific channel.
   * @param channel The notification channel
   * @param url Discord webhook URL
   */
  void set_webhook(WebhookChannel channel, const std::string& url);

  /**
   * Send a simple notification.
   * @param channel Target channel
   * @param level Severity level (determines color)
   * @param title Embed title
   * @param description Embed description (supports markdown)
   */
  void notify(
    WebhookChannel channel,
    NotificationLevel level,
    const std::string& title,
    const std::string& description
  );

  /**
   * Send a notification with custom fields.
   * @param channel Target channel
   * @param level Severity level
   * @param title Embed title
   * @param description Embed description
   * @param fields Additional embed fields
   */
  void notify(
    WebhookChannel channel,
    NotificationLevel level,
    const std::string& title,
    const std::string& description,
    const std::vector<WebhookField>& fields
  );

  /**
   * Send a notification with URL link.
   * @param channel Target channel
   * @param level Severity level
   * @param title Embed title (becomes clickable link)
   * @param url URL for the title link
   * @param description Embed description
   */
  void notify_with_url(
    WebhookChannel channel,
    NotificationLevel level,
    const std::string& title,
    const std::string& url,
    const std::string& description
  );

  /**
   * Check if a webhook is configured for a channel.
   */
  bool has_webhook(WebhookChannel channel) const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<WebhookChannel, std::string> webhooks_;

  std::optional<std::string> get_webhook_url(WebhookChannel channel) const;
  static uint32_t level_to_color(NotificationLevel level);
  static void send_webhook(const std::string& url, const std::string& payload);
};

} // namespace services
