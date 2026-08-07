#include <gtest/gtest.h>

#include <string>
#include <unistd.h>

#include <fiber/common/IoError.h>
#include <fiber/net/TlsContext.h>
#include <fiber/net/TlsOptions.h>

namespace {

// system_ca_bundle_path() must return either an empty string (nothing found)
// or a path that is actually readable on this host.
TEST(TlsContextSystemCa, ReturnsEmptyOrReadablePath) {
    const std::string &path = fiber::net::TlsContext::system_ca_bundle_path();
    if (path.empty()) {
        SUCCEED() << "no system CA bundle discovered on this host";
        return;
    }
    ASSERT_EQ(::access(path.c_str(), R_OK), 0) << "discovered path not readable: " << path;
}

// A client context with verify_peer enabled but no explicit ca_file must init
// successfully: the system CA probe (or the set_default_verify_paths fallback)
// has to yield a usable trust store rather than IoErr::Invalid. This is the
// regression guard for the RHEL-like / minimal-host failure (io_error=invalid).
TEST(TlsContextSystemCa, ClientVerifyPeerInitsWithoutExplicitCaFile) {
    fiber::net::TlsOptions options;
    options.enabled = true;
    options.verify_peer = true; // ca_file intentionally left empty
    fiber::net::TlsContext context(options, /*is_server=*/false, /*require_server_identity=*/false);
    const auto result = context.init();
    ASSERT_TRUE(result.has_value()) << "init failed with io_error=" << fiber::common::io_err_name(result.error());
}

} // namespace
