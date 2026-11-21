#include "config.hpp"

#include <charconv>
#include <cstdlib>
#include <format>
#include <print>
#include <string_view>

namespace wfa {

namespace {

constexpr std::string_view usage = R"(Usage: wayfaraway [options]

Headless Wayland compositor, served over RDP. The desktop always starts
locked; unlock it with the password of the user running the compositor.

Options:
  -a, --address ADDR      RDP listen address (default: 0.0.0.0)
  -p, --port PORT         RDP listen port (default: 3389)
      --cert FILE         TLS certificate (default: $CONFIG/tls.crt)
      --key FILE          TLS private key (default: $CONFIG/tls.key)
      --size WxH          Initial output size (default: 1920x1080)
      --pam-service NAME  PAM service used to unlock (default: {})
  -t, --terminal CMD      Terminal launched with Alt+Return (default: foot)
  -s, --startup CMD       Run CMD once the compositor is up (repeatable)
  -v, --verbose           Verbose logging
  -h, --help              Show this help

$CONFIG is $XDG_CONFIG_HOME/wayfaraway (~/.config/wayfaraway).
)";

template <typename T>
bool parse_number(std::string_view text, T& out)
{
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

bool parse_size(std::string_view text, int& width, int& height)
{
    auto x = text.find('x');
    if (x == std::string_view::npos)
        return false;
    return parse_number(text.substr(0, x), width) && parse_number(text.substr(x + 1), height)
        && width >= 320 && height >= 240 && width <= 8192 && height <= 8192;
}

} // namespace

std::filesystem::path config_dir()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "wayfaraway";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "wayfaraway";
    return "wayfaraway";
}

std::expected<Config, std::string> parse_args(int argc, char** argv)
{
    Config config;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        auto value = [&]() -> std::expected<std::string_view, std::string> {
            if (i + 1 >= argc)
                return std::unexpected(std::format("{} requires an argument", arg));
            return argv[++i];
        };

        auto take = [&](auto&& apply) -> std::expected<void, std::string> {
            auto v = value();
            if (!v)
                return std::unexpected(v.error());
            if (!apply(*v))
                return std::unexpected(std::format("invalid value for {}: '{}'", arg, *v));
            return {};
        };

        std::expected<void, std::string> result;
        if (arg == "-h" || arg == "--help") {
            std::print("{}", std::format(usage, WAYFARAWAY_DEFAULT_PAM_SERVICE));
            return std::unexpected(std::string{});
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "-a" || arg == "--address") {
            result = take([&](std::string_view v) { config.rdp_address = v; return !v.empty(); });
        } else if (arg == "-p" || arg == "--port") {
            result = take([&](std::string_view v) { return parse_number(v, config.rdp_port) && config.rdp_port != 0; });
        } else if (arg == "--cert") {
            result = take([&](std::string_view v) { config.tls_cert = v; return !v.empty(); });
        } else if (arg == "--key") {
            result = take([&](std::string_view v) { config.tls_key = v; return !v.empty(); });
        } else if (arg == "--size") {
            result = take([&](std::string_view v) { return parse_size(v, config.width, config.height); });
        } else if (arg == "--pam-service") {
            result = take([&](std::string_view v) { config.pam_service = v; return !v.empty(); });
        } else if (arg == "-t" || arg == "--terminal") {
            result = take([&](std::string_view v) { config.terminal = v; return !v.empty(); });
        } else if (arg == "-s" || arg == "--startup") {
            result = take([&](std::string_view v) { config.startup_commands.emplace_back(v); return !v.empty(); });
        } else {
            return std::unexpected(std::format("unknown option '{}' (see --help)", arg));
        }

        if (!result)
            return std::unexpected(result.error());
    }

    if (config.tls_cert.empty())
        config.tls_cert = config_dir() / "tls.crt";
    if (config.tls_key.empty())
        config.tls_key = config_dir() / "tls.key";

    return config;
}

} // namespace wfa
