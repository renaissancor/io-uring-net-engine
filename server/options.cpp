// server/options.cpp

#include "options.h"

namespace srv {

const std::string_view k_usage =
    "usage: iouring_net-server [--dry-run] [--help]\n"
    "  --dry-run   verify the library seam and exit\n"
    "  --help      print this message\n";

expected<options, std::string_view> parse_options(int argc,
                                                  const char* const* argv) {
    options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--dry-run") {
            opt.dry_run = true;
        } else if (arg == "--help") {
            opt.help = true;
        } else {
            return unexpected<std::string_view>(arg);
        }
    }
    return opt;
}

}  // namespace srv
