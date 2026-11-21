#include <csignal>
#include <print>

#include "config.hpp"
#include "server.hpp"

int main(int argc, char** argv)
{
    auto config = wfa::parse_args(argc, argv);
    if (!config) {
        if (config.error().empty())
            return 0;
        std::println(stderr, "wayfaraway: {}", config.error());
        return 1;
    }

    wlr_log_init(config->verbose ? WLR_DEBUG : WLR_INFO, nullptr);
    wlr_log(WLR_INFO, "wayfaraway %s starting", WAYFARAWAY_VERSION);

    // A vanished RDP socket must not kill the whole session.
    std::signal(SIGPIPE, SIG_IGN);

    wfa::Server server(std::move(*config));
    if (!server.start())
        return 1;
    server.run();
    return 0;
}
