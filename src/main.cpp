#include "tbb/tbb.h"
#include "bot.h"

int main() {

    std::locale::global(std::locale("en_US.UTF-8"));

    //std::string key = std::getenv("BOT_TOKEN");
    std::string key = ""; 
    Bot bot(key);
    bot.start();

    return EXIT_SUCCESS;
}
