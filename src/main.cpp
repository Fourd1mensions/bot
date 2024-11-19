#include <bot.h>
#include <requests.h>
#include <tbb/tbb.h>

int main(const int argc, const char** argv) {
  std::locale::global(std::locale("en_US.UTF-8"));
  tbb::global_control ctl(tbb::global_control::max_allowed_parallelism, 16);

  bool                delete_commands = false;
  for (const std::string_view param : std::views::counted(argv, argc)) {
    if (param == "--delete")
      delete_commands = true;
  }

  std::string key = Request::read_config("DISCORD_TOKEN");
  Bot         bot(key, delete_commands);

  return EXIT_SUCCESS;
}
