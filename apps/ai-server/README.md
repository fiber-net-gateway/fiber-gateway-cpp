# AI Server Migration

The `ai-server` application has moved to the
[`fiber-net-gateway/ai-gateway`](https://github.com/fiber-net-gateway/ai-gateway)
repository. That repository is the source of truth for the application, owns
its builds, tests, releases, deployment documentation, and accepts all
application issues and feature requests through its
[`issue tracker`](https://github.com/fiber-net-gateway/ai-gateway/issues).

The original implementation was migrated from this repository at revision
`465dc942bf05eda6cdfef5855c03964d436ff9f0`. Its provenance is recorded in the
ai-gateway repository's
[`native/ai-server/UPSTREAM.md`](https://github.com/fiber-net-gateway/ai-gateway/blob/master/native/ai-server/UPSTREAM.md).
The removed files remain available in this repository's Git history under the
MIT license.

This directory is only a migration pointer. It contains no build target, and
the legacy application is no longer part of `FIBER_BUILD_APPS` or supported in
this repository. The reusable Fiber runtime and the `fiber::nacos`,
`fiber::cat`, and `fiber::prometheus` components remain maintained here for
downstream consumers including ai-gateway.
