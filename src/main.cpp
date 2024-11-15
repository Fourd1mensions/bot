#include <tbb/tbb.h>
#include <bot.h>
#include <requests.h>

int main() {
    std::locale::global(std::locale("en_US.UTF-8"));

    std::string key = Request::read_config("DISCORD_TOKEN"); 
    Bot bot(key);

    return EXIT_SUCCESS;
}
