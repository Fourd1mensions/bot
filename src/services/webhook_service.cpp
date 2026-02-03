#include "services/webhook_service.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <fmt/chrono.h>

#include <thread>
#include <chrono>

using json = nlohmann::json;

namespace services {

void WebhookService::set_webhook(WebhookChannel channel, const std::string& url) {
  if (url.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  webhooks_[channel] = url;

  const char* channel_name = "unknown";
  switch (channel) {
    case WebhookChannel::MirrorErrors: channel_name = "MirrorErrors"; break;
    case WebhookChannel::General: channel_name = "General"; break;
    case WebhookChannel::Debug: channel_name = "Debug"; break;
  }
  spdlog::info("[Webhook] Configured {} channel", channel_name);
}

bool WebhookService::has_webhook(WebhookChannel channel) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = webhooks_.find(channel);
  return it != webhooks_.end() && !it->second.empty();
}

std::optional<std::string> WebhookService::get_webhook_url(WebhookChannel channel) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = webhooks_.find(channel);
  if (it != webhooks_.end() && !it->second.empty()) {
    return it->second;
  }
  return std::nullopt;
}

uint32_t WebhookService::level_to_color(NotificationLevel level) {
  switch (level) {
    case NotificationLevel::Info:    return 0x3498DB;  // Blue
    case NotificationLevel::Success: return 0x2ECC71;  // Green
    case NotificationLevel::Warning: return 0xF1C40F;  // Yellow
    case NotificationLevel::Error:   return 0xE74C3C;  // Red
  }
  return 0x3498DB;
}

void WebhookService::send_webhook(const std::string& url, const std::string& payload) {
  // Send in background thread to not block caller
  std::thread([url, payload]() {
    auto response = cpr::Post(
      cpr::Url{url},
      cpr::Header{{"Content-Type", "application/json"}},
      cpr::Body{payload},
      cpr::Timeout{5000}
    );

    if (response.status_code != 200 && response.status_code != 204) {
      spdlog::warn("[Webhook] Failed to send notification: HTTP {}", response.status_code);
    }
  }).detach();
}

void WebhookService::notify(
    WebhookChannel channel,
    NotificationLevel level,
    const std::string& title,
    const std::string& description
) {
  notify(channel, level, title, description, {});
}

void WebhookService::notify(
    WebhookChannel channel,
    NotificationLevel level,
    const std::string& title,
    const std::string& description,
    const std::vector<WebhookField>& fields
) {
  auto url_opt = get_webhook_url(channel);
  if (!url_opt) {
    return;
  }

  json embed;
  embed["title"] = title;
  embed["description"] = description;
  embed["color"] = level_to_color(level);
  embed["timestamp"] = fmt::format("{:%Y-%m-%dT%H:%M:%SZ}",
    fmt::gmtime(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())));

  if (!fields.empty()) {
    json fields_array = json::array();
    for (const auto& field : fields) {
      fields_array.push_back({
        {"name", field.name},
        {"value", field.value},
        {"inline", field.inline_field}
      });
    }
    embed["fields"] = fields_array;
  }

  json payload;
  payload["embeds"] = json::array({embed});

  send_webhook(*url_opt, payload.dump());
}

void WebhookService::notify_with_url(
    WebhookChannel channel,
    NotificationLevel level,
    const std::string& title,
    const std::string& url,
    const std::string& description
) {
  auto webhook_url_opt = get_webhook_url(channel);
  if (!webhook_url_opt) {
    return;
  }

  json embed;
  embed["title"] = title;
  embed["url"] = url;
  embed["description"] = description;
  embed["color"] = level_to_color(level);
  embed["timestamp"] = fmt::format("{:%Y-%m-%dT%H:%M:%SZ}",
    fmt::gmtime(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())));

  json payload;
  payload["embeds"] = json::array({embed});

  send_webhook(*webhook_url_opt, payload.dump());
}

} // namespace services
