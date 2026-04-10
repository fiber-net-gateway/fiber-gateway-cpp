# sample_app

## Overview

Describe the app in one paragraph. Explain the protocol, service role, or workflow it is meant to demonstrate.

## Build

```bash
cmake -S . -B build
cmake --build build --target fiber_app_sample_app
```

The output binary is written to `build/apps/sample_app`.

## Run

```bash
./build/apps/sample_app
```

Document the required arguments, config files, ports, and environment variables here.

## Layout

- `src/main.cpp`: process entry point and top-level wiring.
- `src/`: implementation files that belong only to this app.

Add more entries if the app has `config/`, `scripts/`, or generated assets.

## Notes

Call out anything easy to get wrong: cert generation, required kernel features, expected traffic shape, or current limitations.
