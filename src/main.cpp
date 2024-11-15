#include "tbb/tbb.h"
#include "bot.h"
#include <requests.h>

int main() {

    std::locale::global(std::locale("en_US.UTF-8"));

    //std::string key = std::getenv("BOT_TOKEN");
    std::string key = Request::read_config("DISCORD_TOKEN"); 
    Bot bot(key);
    bot.start();

    return EXIT_SUCCESS;
}
