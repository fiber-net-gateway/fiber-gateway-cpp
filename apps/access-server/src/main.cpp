#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view kUsage = "usage: access-server [--help]\n";
constexpr std::string_view kNotImplemented = "access-server migration scaffold: runtime is not implemented; see "
                                             "apps/access-server/docs/migration-plan.md\n";

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << kUsage << kNotImplemented;
        return EXIT_SUCCESS;
    }

    if (argc != 1) {
        std::cerr << kUsage;
    }
    std::cerr << kNotImplemented;
    return EXIT_FAILURE;
}
