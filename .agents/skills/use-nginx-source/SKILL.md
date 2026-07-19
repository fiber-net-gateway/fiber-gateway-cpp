---
name: use-nginx-source
description: Inspect and compare fiber-gateway-cpp with the repository-managed Nginx source and build. Use for implementation, diagnosis, design, testing, protocol behavior, configuration semantics, performance, or compatibility work that mentions Nginx, requests Nginx-aligned behavior, or would benefit from checking how Nginx implements the same feature. Ensure the pinned Nginx source and binary exist through scripts/build_nginx.sh before relying on them.
---

# Use Nginx Source

Use the repository-pinned Nginx checkout as an implementation reference. Keep the project build script as the source of truth for the Nginx version, source layout, configuration, and dependencies.

## Prepare Nginx

1. Work from the repository root.
2. Read `scripts/build_nginx.sh` and obtain `nginx_version` from it. Resolve the source as `temp/nginx-<nginx_version>/` and the installed binary as `temp/nginx-install/sbin/nginx`.
3. Treat Nginx as ready only when the pinned source contains an executable `configure` and the installed binary is executable. Do not assume `temp/nginx/`, another checkout, a system package, or a cached archive matches the pinned version.
4. If either artifact is missing, run `scripts/build_nginx.sh`. If its required zlib source is absent, first run `cmake -S . -B build`, then rerun `scripts/build_nginx.sh`.
5. If preparation fails, report the exact missing tool, dependency, network, configure, or build error. Do not silently switch to another Nginx version or source tree.

## Investigate Nginx

- Search the pinned tree with `rg` before opening broad files. Start under `src/`, then follow relevant declarations, configuration directives, callbacks, and call sites.
- Inspect generated files under the pinned tree's `objs/` only when build-time feature selection or generated configuration affects the question.
- Record the pinned Nginx version and cite exact local source paths and line numbers for conclusions derived from Nginx.
- Distinguish observed Nginx behavior from inference. For protocol work, treat Nginx as an implementation reference rather than the protocol specification.
- Do not modify anything under `temp/` unless the user explicitly asks to patch or experiment with the Nginx checkout. It is ignored, generated workspace state.

## Apply the Comparison

- Extract the relevant behavior, state transitions, ownership rules, error handling, and performance choices instead of mechanically porting code.
- Adapt findings to this repository's C++23, fiber/event-loop, memory-management, no-exception, and performance constraints.
- Preserve intentional project differences. State whether a result matches Nginx, deliberately diverges, or remains uncertain.
- When changing project code, add focused coverage for the behavior and run the repository-required formatting, build, and tests. Use `temp/nginx-install/sbin/nginx` for runtime comparison only when the task needs behavioral validation.
