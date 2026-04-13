#include <bot.h>
#include <utils.h>
#include <database.h>
#include <cache.h>
#include <migration.h>

#include <tbb/tbb.h>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <unistd.h>
#include <cstring>
#include <cerrno>

std::atomic<bool> shutdown_requested{false};
Bot* global_bot_ptr = nullptr;
int shutdown_pipe[2] = {-1, -1};

void signal_handler(int signal) {
  // Write to stderr since it's unbuffered and async-signal-safe
  const char* msg = "\n[SIGNAL] Received shutdown signal, stopping...\n";
  write(STDERR_FILENO, msg, strlen(msg));

  shutdown_requested.store(true, std::memory_order_release);

  // Notify shutdown thread via pipe
  if (shutdown_pipe[1] != -1) {
    const char byte = 'x';
    ssize_t res = write(shutdown_pipe[1], &byte, 1);
    (void)res;
  }
}

int main(const int argc, const char** argv) {
  std::locale::global(std::locale("en_US.UTF-8"));
  tbb::global_control ctl(tbb::global_control::max_allowed_parallelism, 16);

  // Create pipe for async-signal-safe shutdown notification
  if (pipe(shutdown_pipe) == -1) {
    std::cerr << "Failed to create shutdown pipe: " << std::strerror(errno) << std::endl;
    return EXIT_FAILURE;
  }

  bool                delete_commands = false;
  for (const std::string_view param : std::views::counted(argv, argc)) {
    if (param == "--delete")
      delete_commands = true;
  }

  // Load configuration
  std::string key = utils::read_field("DISCORD_TOKEN","config.json");

  // Initialize Database
  try {
    std::string db_host = std::getenv("POSTGRES_HOST") ? std::getenv("POSTGRES_HOST") : "localhost";
    int db_port = std::getenv("POSTGRES_PORT") ? std::stoi(std::getenv("POSTGRES_PORT")) : 9183;
    std::string db_name = std::getenv("POSTGRES_DB") ? std::getenv("POSTGRES_DB") : "patchouli";
    std::string db_user = std::getenv("POSTGRES_USER") ? std::getenv("POSTGRES_USER") : "patchouli";
    std::string db_pass = std::getenv("POSTGRES_PASSWORD") ? std::getenv("POSTGRES_PASSWORD") : "patchouli_pass";

    db::Database::init(db_host, db_port, db_name, db_user, db_pass);
    std::cout << "[INIT] Database initialized" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "[ERROR] Failed to initialize database: " << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  // Initialize Memcached
  try {
    std::string cache_host = std::getenv("MEMCACHED_HOST") ? std::getenv("MEMCACHED_HOST") : "localhost";
    int cache_port = std::getenv("MEMCACHED_PORT") ? std::stoi(std::getenv("MEMCACHED_PORT")) : 9184;

    cache::MemcachedCache::init(cache_host, cache_port);
    std::cout << "[INIT] Memcached cache initialized" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "[ERROR] Failed to initialize cache: " << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  // Run SQL schema migrations
  try {
    migration::run_sql_migrations();
  } catch (const std::exception& e) {
    std::cerr << "[ERROR] Failed to run SQL migrations: " << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  // Run migration if needed
  if (migration::needs_migration()) {
    std::cout << "[MIGRATE] JSON files detected, starting migration..." << std::endl;
    auto stats = migration::perform_migration();
    if (stats.success) {
      std::cout << "[MIGRATE] Migration completed successfully" << std::endl;
    } else {
      std::cerr << "[MIGRATE] Migration failed" << std::endl;
      for (const auto& error : stats.errors) {
        std::cerr << "[MIGRATE] Error: " << error << std::endl;
      }
    }
  }

  Bot         bot(key, delete_commands);

  // Set global pointer for signal handler
  global_bot_ptr = &bot;

  // (Re)install signal handlers after Crow/DPP setup so ours stay active
  {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, nullptr) == -1) {
      std::cerr << "Failed to register SIGINT handler" << std::endl;
      return EXIT_FAILURE;
    }

    if (sigaction(SIGTERM, &sa, nullptr) == -1) {
      std::cerr << "Failed to register SIGTERM handler" << std::endl;
      return EXIT_FAILURE;
    }
  }

  std::cout << "Signal handlers registered. Press Ctrl-C to stop." << std::endl;

  std::cout << "Bot is running. Press Ctrl-C to stop." << std::endl;

  // Start bot (non-blocking) and wait for signal through pipe
  bot.start();
  char buf;
  while (true) {
    ssize_t n = read(shutdown_pipe[0], &buf, 1);
    if (n > 0) break;
    if (n == -1 && errno == EINTR) continue;
    if (n <= 0) break;
  }

  if (global_bot_ptr) {
    global_bot_ptr->shutdown();
  }

  // Clean up
  global_bot_ptr = nullptr;
  close(shutdown_pipe[0]);
  close(shutdown_pipe[1]);

  std::cout << "Bot stopped." << std::endl;

  return EXIT_SUCCESS;
}
