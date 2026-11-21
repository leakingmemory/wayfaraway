#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace wfa {

struct Config {
    std::string rdp_address = "0.0.0.0";
    std::uint16_t rdp_port = 3389;
    std::filesystem::path tls_cert;
    std::filesystem::path tls_key;

    // Initial virtual output size; replaced by the RDP client's desktop size
    // when one connects.
    int width = 1920;
    int height = 1080;

    std::string pam_service = WAYFARAWAY_DEFAULT_PAM_SERVICE;
    std::string terminal = "foot";
    std::vector<std::string> startup_commands;

    bool verbose = false;
};

// Returns the parsed config, or an error message. An empty error string means
// the program should exit successfully (e.g. after --help).
std::expected<Config, std::string> parse_args(int argc, char** argv);

std::filesystem::path config_dir();

} // namespace wfa
