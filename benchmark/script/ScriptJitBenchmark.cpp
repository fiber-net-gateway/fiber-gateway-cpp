#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fiber/script/JsGc.h>
#include <fiber/script/Library.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/jit/JitCompiler.h>

#ifndef FIBER_SCRIPT_BENCHMARK_BUILD_TYPE
#define FIBER_SCRIPT_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using fiber::script::AbiResult;
using fiber::script::GcHeap;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::Script;
using fiber::script::ScriptBackendMode;
using fiber::script::ScriptCompileOptions;
using fiber::script::ScriptResult;
using fiber::script::ValueHandle;

constexpr std::uint64_t kMaxBatchIterations = 1'000'000'000ull;

struct Options {
    std::uint64_t target_ms = 200;
    std::uint32_t rounds = 7;
    std::uint32_t compile_samples = 3;
    std::uint32_t input_size = 64;
    std::string_view filter;
    bool help = false;
    bool list = false;
};

enum class InputKind : std::uint8_t {
    NumberArray,
    RouteObject,
};

struct BenchmarkCase {
    std::string_view name;
    std::string_view source;
    InputKind input_kind = InputKind::NumberArray;
    bool async = false;
};

constexpr std::array<BenchmarkCase, 8> kCases{{
        {
                "route_branch",
                R"(
                    if ($.method == "GET") {
                        if ($.path == "/health") { return $.tenant + 1; }
                        if ($.path == "/api/items") { return $.tenant + 2; }
                    }
                    return 0;
                )",
                InputKind::RouteObject,
                false,
        },
        {
                "foreach_arithmetic",
                R"(
                    let acc = 1;
                    for (let k, v of $) {
                        acc = (acc + v) * 3;
                        acc = acc - k;
                        acc = acc % 1000003;
                    }
                    return acc;
                )",
                InputKind::NumberArray,
                false,
        },
        {
                "branch_heavy",
                R"(
                    let acc = 0;
                    for (let k, v of $) {
                        let m = v % 4;
                        if (m == 0) {
                            acc = acc + v;
                        } else if (m == 1) {
                            acc = acc - k;
                        } else if (m == 2) {
                            acc = acc + k * 2;
                        } else {
                            acc = acc - v;
                        }
                    }
                    return acc;
                )",
                InputKind::NumberArray,
                false,
        },
        {
                "mixed_float_arithmetic",
                R"(
                    let acc = 1.25;
                    for (let k, v of $) {
                        acc = (acc * 1.000001 - v) / 1.0000001;
                    }
                    if (acc < 0) { return 1; }
                    return 2;
                )",
                InputKind::NumberArray,
                false,
        },
        {
                "object_property",
                R"(
                    let state = {sum: 0, even: 0, odd: 0};
                    for (let k, v of $) {
                        state.sum = state.sum + v;
                        if (v % 2 == 0) {
                            state.even = state.even + 1;
                        } else {
                            state.odd = state.odd + 1;
                        }
                    }
                    return state.sum + state.even * 3 + state.odd * 7;
                )",
                InputKind::NumberArray,
                false,
        },
        {
                "array_build",
                R"(
                    let out = [...$];
                    let last = 0;
                    for (let k, v of $) {
                        let item = v * 2 + k;
                        out[k] = item;
                        last = out[k];
                    }
                    return out[0] + last;
                )",
                InputKind::NumberArray,
                false,
        },
        {
                "sync_host_call",
                R"(
                    let acc = 0;
                    for (let k, v of $) {
                        acc = mix(acc, v + k);
                    }
                    return acc;
                )",
                InputKind::NumberArray,
                false,
        },
        {
                "async_host_call",
                R"(
                    let acc = 0;
                    for (let k, v of $) {
                        acc = asyncMix(acc, v + k);
                    }
                    return acc;
                )",
                InputKind::NumberArray,
                true,
        },
}};

void print_usage(const char *program) noexcept {
    std::printf("usage: %s [options]\n", program);
    std::printf("  --target-ms N       target duration per measured sample (default 200)\n");
    std::printf("  --rounds N          measured rounds per backend and case (default 7)\n");
    std::printf("  --compile-samples N compilation samples per backend and case (default 3)\n");
    std::printf("  --input-size N      number-array elements used by loop cases (default 64)\n");
    std::printf("  --filter TEXT       run cases whose names contain TEXT\n");
    std::printf("  --list              list case names and exit\n");
    std::printf("  --help              show this message\n");
}

template<typename T>
bool parse_integer(std::string_view input, T minimum, T maximum, T &out) noexcept {
    T value = 0;
    const char *begin = input.data();
    const char *end = begin + input.size();
    auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum || value > maximum) {
        return false;
    }
    out = value;
    return true;
}

bool parse_options(int argc, char **argv, Options &options) noexcept {
    for (int index = 1; index < argc; ++index) {
        std::string_view argument(argv[index]);
        if (argument == "--help") {
            options.help = true;
            print_usage(argv[0]);
            return false;
        }
        if (argument == "--list") {
            options.list = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::fprintf(stderr, "missing value for %.*s\n", static_cast<int>(argument.size()), argument.data());
            return false;
        }
        std::string_view value(argv[++index]);
        if (argument == "--target-ms") {
            if (!parse_integer(value, std::uint64_t{10}, std::uint64_t{60'000}, options.target_ms)) {
                std::fprintf(stderr, "invalid --target-ms value\n");
                return false;
            }
        } else if (argument == "--rounds") {
            if (!parse_integer(value, std::uint32_t{1}, std::uint32_t{101}, options.rounds)) {
                std::fprintf(stderr, "invalid --rounds value\n");
                return false;
            }
        } else if (argument == "--compile-samples") {
            if (!parse_integer(value, std::uint32_t{1}, std::uint32_t{20}, options.compile_samples)) {
                std::fprintf(stderr, "invalid --compile-samples value\n");
                return false;
            }
        } else if (argument == "--input-size") {
            if (!parse_integer(value, std::uint32_t{1}, std::uint32_t{4096}, options.input_size)) {
                std::fprintf(stderr, "invalid --input-size value\n");
                return false;
            }
        } else if (argument == "--filter") {
            options.filter = value;
        } else {
            std::fprintf(stderr, "unknown option: %.*s\n", static_cast<int>(argument.size()), argument.data());
            return false;
        }
    }
    return true;
}

AbiResult mix_result(Library::Arguments arguments) noexcept {
    if (arguments.argc != 2 || fiber::script::js_value_type(arguments.args[0]) != JsNodeType::Integer ||
        fiber::script::js_value_type(arguments.args[1]) != JsNodeType::Integer) {
        return AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }
    std::uint64_t lhs = static_cast<std::uint64_t>(fiber::script::js_value_int64(arguments.args[0]));
    std::uint64_t rhs = static_cast<std::uint64_t>(fiber::script::js_value_int64(arguments.args[1]));
    std::int64_t mixed = static_cast<std::int64_t>((lhs * 33u + rhs * 17u + 11u) % 1'000'003u);
    return AbiResult::success(JsValue::make_integer(mixed));
}

AbiResult mix_function(void *userdata, const Library::HostCallFrame &frame, Library::Arguments arguments) noexcept {
    (void) userdata;
    (void) frame;
    return mix_result(arguments);
}

fiber::script::AsyncTask async_mix_function(void *userdata, const Library::HostCallFrame &frame,
                                            Library::Arguments arguments) noexcept {
    (void) userdata;
    (void) frame;
    co_return mix_result(arguments);
}

class BenchmarkLibrary final : public Library {
public:
    BenchmarkLibrary() noexcept {
        mix_.kind = HostCallable::Kind::SyncFunction;
        mix_.function = &mix_function;
        mix_.debug_name = "mix";
        async_mix_.kind = HostCallable::Kind::AsyncFunction;
        async_mix_.async_function = &async_mix_function;
        async_mix_.debug_name = "asyncMix";
    }

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override {
        if (name != "mix") {
            return FunctionMatchResult::not_found();
        }
        if (request.has_spread || request.known_argc != 2) {
            return FunctionMatchResult::arity_mismatch();
        }
        FunctionSignature signature;
        signature.required_argc = 2;
        signature.fixed_argc = 2;
        signature.variadic = false;
        return FunctionMatchResult::found(&mix_, signature, nullptr, 0);
    }

    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override {
        if (name != "asyncMix") {
            return FunctionMatchResult::not_found();
        }
        if (request.has_spread || request.known_argc != 2) {
            return FunctionMatchResult::arity_mismatch();
        }
        FunctionSignature signature;
        signature.required_argc = 2;
        signature.fixed_argc = 2;
        signature.variadic = false;
        return FunctionMatchResult::found(&async_mix_, signature, nullptr, 0);
    }

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override {
        (void) namespace_name;
        (void) key;
        return nullptr;
    }

    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<JsValue> &literals) const override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    HostCallable mix_{};
    HostCallable async_mix_{};
};

double percentile(const std::vector<double> &sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0;
    }
    double position = fraction * static_cast<double>(sorted.size() - 1u);
    std::size_t lower = static_cast<std::size_t>(position);
    std::size_t upper = std::min(lower + 1u, sorted.size() - 1u);
    double weight = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * weight;
}

struct SampleStats {
    double median = 0;
    double p25 = 0;
    double p75 = 0;
};

SampleStats sample_stats(std::vector<double> samples) {
    std::ranges::sort(samples);
    return SampleStats{
            .median = percentile(samples, 0.5),
            .p25 = percentile(samples, 0.25),
            .p75 = percentile(samples, 0.75),
    };
}

struct CompiledBackend {
    Script script;
    double compile_us = 0;
};

bool compile_backend(BenchmarkLibrary &library, const BenchmarkCase &benchmark_case, ScriptBackendMode backend,
                     std::uint32_t samples, CompiledBackend &out) {
    std::vector<double> compile_times;
    compile_times.reserve(samples);
    Script retained;
    for (std::uint32_t index = 0; index < samples; ++index) {
        ScriptCompileOptions options;
        options.backend = backend;
        const auto start = Clock::now();
        auto compiled = fiber::script::compile_script(library, benchmark_case.source, options);
        const auto elapsed = Clock::now() - start;
        if (!compiled) {
            std::fprintf(stderr, "%.*s: %s compile failed: %s\n", static_cast<int>(benchmark_case.name.size()),
                         benchmark_case.name.data(), backend == ScriptBackendMode::Interpreter ? "interpreter" : "JIT",
                         compiled.error().message.c_str());
            return false;
        }
        if (compiled->uses_jit() != (backend == ScriptBackendMode::RequireJit)) {
            std::fprintf(stderr, "%.*s: backend selection mismatch\n", static_cast<int>(benchmark_case.name.size()),
                         benchmark_case.name.data());
            return false;
        }
        compile_times.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(elapsed).count());
        retained = std::move(*compiled);
    }
    out.script = std::move(retained);
    out.compile_us = sample_stats(std::move(compile_times)).median;
    return true;
}

class ExecutionContext final {
public:
    explicit ExecutionContext(Script script) : script_(std::move(script)) {}

    bool initialize(InputKind kind, std::uint32_t input_size) {
        root_ = heap_.global_value();
        if (!root_) {
            return false;
        }
        if (kind == InputKind::RouteObject) {
            return initialize_route();
        }
        if (!fiber::script::gc_make_array(&heap_, root_, input_size)) {
            return false;
        }
        for (std::uint32_t index = 0; index < input_size; ++index) {
            std::int64_t value = static_cast<std::int64_t>((index * 17u + 13u) % 101u + 1u);
            if (!fiber::script::gc_array_push(&heap_, root_, JsValue::make_integer(value))) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] Script &script() noexcept { return script_; }
    [[nodiscard]] GcHeap &heap() noexcept { return heap_; }
    [[nodiscard]] JsValue root() const noexcept { return root_ ? *root_ : JsValue::make_undefined(); }

private:
    bool initialize_route() {
        static constexpr char kMethod[] = "GET";
        static constexpr char kPath[] = "/api/items";
        if (!fiber::script::gc_make_object(&heap_, root_, 3)) {
            return false;
        }
        return fiber::script::gc_object_set_key(&heap_, root_, "method", 6,
                                                JsValue::make_native_string(kMethod, sizeof(kMethod) - 1u)) &&
               fiber::script::gc_object_set_key(&heap_, root_, "path", 4,
                                                JsValue::make_native_string(kPath, sizeof(kPath) - 1u)) &&
               fiber::script::gc_object_set_key(&heap_, root_, "tenant", 6, JsValue::make_integer(7));
    }

    Script script_;
    GcHeap heap_;
    ValueHandle root_ = nullptr;
};

class BatchTask final {
public:
    struct promise_type {
        BatchTask get_return_object() noexcept {
            return BatchTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

    using Handle = std::coroutine_handle<promise_type>;

    explicit BatchTask(Handle handle) noexcept : handle_(handle) {}
    BatchTask(const BatchTask &) = delete;
    BatchTask &operator=(const BatchTask &) = delete;
    BatchTask(BatchTask &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    ~BatchTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    void resume() noexcept {
        if (handle_) {
            handle_.resume();
        }
    }
    [[nodiscard]] bool done() const noexcept { return !handle_ || handle_.done(); }

private:
    Handle handle_ = nullptr;
};

bool consume_result(const ScriptResult &result, std::uint64_t &checksum, std::int64_t &last_value) noexcept {
    if (!result.has_value() || fiber::script::js_value_type(result.value()) != JsNodeType::Integer) {
        return false;
    }
    last_value = fiber::script::js_value_int64(result.value());
    checksum ^= static_cast<std::uint64_t>(last_value) + 0x9e3779b97f4a7c15ull + (checksum << 6u) + (checksum >> 2u);
    return true;
}

BatchTask run_async_iterations(ExecutionContext &context, std::uint64_t iterations, std::uint64_t &checksum,
                               std::int64_t &last_value, bool &ok) {
    for (std::uint64_t index = 0; index < iterations; ++index) {
        ScriptResult result = co_await context.script().exec_async(context.root(), nullptr, context.heap());
        if (!consume_result(result, checksum, last_value)) {
            ok = false;
            co_return;
        }
    }
}

template<typename Value>
inline void keep_observable(Value &value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : "+r"(value) : : "memory");
#else
    (void) value;
#endif
}

struct BatchMeasurement {
    Duration elapsed{};
    std::uint64_t checksum = 0;
    std::int64_t last_value = 0;
    bool ok = false;
};

BatchMeasurement run_batch(ExecutionContext &context, bool async, std::uint64_t iterations) {
    BatchMeasurement measurement;
    bool ok = true;
    const auto start = Clock::now();
    if (async) {
        BatchTask task = run_async_iterations(context, iterations, measurement.checksum, measurement.last_value, ok);
        task.resume();
        ok = ok && task.done();
    } else {
        for (std::uint64_t index = 0; index < iterations; ++index) {
            ScriptResult result = context.script().exec_sync(context.root(), nullptr, context.heap());
            if (!consume_result(result, measurement.checksum, measurement.last_value)) {
                ok = false;
                break;
            }
        }
    }
    keep_observable(measurement.checksum);
    measurement.elapsed = Clock::now() - start;
    measurement.ok = ok;
    return measurement;
}

std::uint64_t calibrate_iterations(ExecutionContext &context, bool async, Duration target, bool &ok) {
    const auto target_ns =
            std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(target).count());
    const std::int64_t calibration_ns = std::max<std::int64_t>(1, target_ns / 5);
    std::uint64_t iterations = 1;
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        BatchMeasurement measurement = run_batch(context, async, iterations);
        if (!measurement.ok) {
            ok = false;
            return 0;
        }
        std::int64_t elapsed_ns = std::max<std::int64_t>(
                1, std::chrono::duration_cast<std::chrono::nanoseconds>(measurement.elapsed).count());
        if (elapsed_ns >= calibration_ns) {
            long double scaled = static_cast<long double>(iterations) * static_cast<long double>(target_ns) /
                                 static_cast<long double>(elapsed_ns);
            scaled = std::clamp(scaled, 1.0L, static_cast<long double>(kMaxBatchIterations));
            return static_cast<std::uint64_t>(scaled);
        }
        std::uint64_t factor = static_cast<std::uint64_t>(calibration_ns / elapsed_ns);
        factor = std::clamp<std::uint64_t>(factor, 2, 16);
        if (iterations > kMaxBatchIterations / factor) {
            return kMaxBatchIterations;
        }
        iterations *= factor;
    }
    return iterations;
}

bool add_sample(ExecutionContext &context, bool async, std::uint64_t iterations, std::vector<double> &samples) {
    BatchMeasurement measurement = run_batch(context, async, iterations);
    if (!measurement.ok) {
        return false;
    }
    double elapsed_ns =
            std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(measurement.elapsed).count();
    samples.push_back(elapsed_ns / static_cast<double>(iterations));
    return true;
}

struct CaseResult {
    double speedup = 0;
    bool ran = false;
};

CaseResult run_case(BenchmarkLibrary &library, const BenchmarkCase &benchmark_case, const Options &options) {
    CompiledBackend interpreter;
    CompiledBackend jit;
    if (!compile_backend(library, benchmark_case, ScriptBackendMode::Interpreter, options.compile_samples,
                         interpreter) ||
        !compile_backend(library, benchmark_case, ScriptBackendMode::RequireJit, options.compile_samples, jit)) {
        return {};
    }

    ExecutionContext interpreter_context(std::move(interpreter.script));
    ExecutionContext jit_context(std::move(jit.script));
    if (!interpreter_context.initialize(benchmark_case.input_kind, options.input_size) ||
        !jit_context.initialize(benchmark_case.input_kind, options.input_size)) {
        std::fprintf(stderr, "%.*s: input initialization failed\n", static_cast<int>(benchmark_case.name.size()),
                     benchmark_case.name.data());
        return {};
    }

    BatchMeasurement interpreter_check = run_batch(interpreter_context, benchmark_case.async, 1);
    BatchMeasurement jit_check = run_batch(jit_context, benchmark_case.async, 1);
    if (!interpreter_check.ok || !jit_check.ok || interpreter_check.last_value != jit_check.last_value) {
        std::fprintf(stderr, "%.*s: interpreter/JIT result mismatch (interp_ok=%u jit_ok=%u interp=%lld jit=%lld)\n",
                     static_cast<int>(benchmark_case.name.size()), benchmark_case.name.data(),
                     interpreter_check.ok ? 1u : 0u, jit_check.ok ? 1u : 0u,
                     static_cast<long long>(interpreter_check.last_value),
                     static_cast<long long>(jit_check.last_value));
        return {};
    }

    Duration target = std::chrono::milliseconds(options.target_ms);
    bool calibration_ok = true;
    std::uint64_t interpreter_iterations =
            calibrate_iterations(interpreter_context, benchmark_case.async, target, calibration_ok);
    std::uint64_t jit_iterations = calibrate_iterations(jit_context, benchmark_case.async, target, calibration_ok);
    if (!calibration_ok || interpreter_iterations == 0 || jit_iterations == 0) {
        std::fprintf(stderr, "%.*s: iteration calibration failed\n", static_cast<int>(benchmark_case.name.size()),
                     benchmark_case.name.data());
        return {};
    }
    if (!run_batch(interpreter_context, benchmark_case.async, interpreter_iterations).ok ||
        !run_batch(jit_context, benchmark_case.async, jit_iterations).ok) {
        std::fprintf(stderr, "%.*s: warmup failed\n", static_cast<int>(benchmark_case.name.size()),
                     benchmark_case.name.data());
        return {};
    }

    std::vector<double> interpreter_samples;
    std::vector<double> jit_samples;
    interpreter_samples.reserve(options.rounds);
    jit_samples.reserve(options.rounds);
    for (std::uint32_t round = 0; round < options.rounds; ++round) {
        bool success = false;
        if ((round & 1u) == 0) {
            success = add_sample(interpreter_context, benchmark_case.async, interpreter_iterations,
                                 interpreter_samples) &&
                      add_sample(jit_context, benchmark_case.async, jit_iterations, jit_samples);
        } else {
            success =
                    add_sample(jit_context, benchmark_case.async, jit_iterations, jit_samples) &&
                    add_sample(interpreter_context, benchmark_case.async, interpreter_iterations, interpreter_samples);
        }
        if (!success) {
            std::fprintf(stderr, "%.*s: measured execution failed\n", static_cast<int>(benchmark_case.name.size()),
                         benchmark_case.name.data());
            return {};
        }
    }

    SampleStats interpreter_stats = sample_stats(std::move(interpreter_samples));
    SampleStats jit_stats = sample_stats(std::move(jit_samples));
    double speedup = interpreter_stats.median / jit_stats.median;
    double jit_delta = (jit_stats.median / interpreter_stats.median - 1.0) * 100.0;
    double interpreter_iqr = (interpreter_stats.p75 - interpreter_stats.p25) / interpreter_stats.median * 100.0;
    double jit_iqr = (jit_stats.p75 - jit_stats.p25) / jit_stats.median * 100.0;
    std::uint32_t work_units = benchmark_case.input_kind == InputKind::NumberArray ? options.input_size : 1u;
    double interpreter_munits = static_cast<double>(work_units) * 1000.0 / interpreter_stats.median;
    double jit_munits = static_cast<double>(work_units) * 1000.0 / jit_stats.median;

    std::printf("RESULT name=%.*s async=%u units=%u jit_inlined_helpers=%u "
                "interp_compile_us=%.2f jit_compile_us=%.2f "
                "interp_ns_per_exec=%.2f jit_ns_per_exec=%.2f speedup=%.3f jit_delta_pct=%+.2f "
                "interp_munits_per_s=%.3f jit_munits_per_s=%.3f interp_iqr_pct=%.2f jit_iqr_pct=%.2f "
                "interp_batch=%llu jit_batch=%llu value=%lld\n",
                static_cast<int>(benchmark_case.name.size()), benchmark_case.name.data(),
                benchmark_case.async ? 1u : 0u, work_units, jit_context.script().jit_inlined_operator_helper_count(),
                interpreter.compile_us, jit.compile_us, interpreter_stats.median, jit_stats.median, speedup, jit_delta,
                interpreter_munits, jit_munits, interpreter_iqr, jit_iqr,
                static_cast<unsigned long long>(interpreter_iterations),
                static_cast<unsigned long long>(jit_iterations), static_cast<long long>(interpreter_check.last_value));
    return CaseResult{.speedup = speedup, .ran = true};
}

bool case_selected(const BenchmarkCase &benchmark_case, const Options &options) noexcept {
    return options.filter.empty() || benchmark_case.name.find(options.filter) != std::string_view::npos;
}

double warm_jit_engine(BenchmarkLibrary &library) {
    BenchmarkCase warmup{"jit_engine_warmup", "return 0;", InputKind::RouteObject, false};
    CompiledBackend compiled;
    if (!compile_backend(library, warmup, ScriptBackendMode::RequireJit, 1, compiled)) {
        return -1;
    }
    GcHeap heap;
    ScriptResult result = compiled.script.exec_sync(JsValue::make_undefined(), nullptr, heap);
    if (!result.has_value() || fiber::script::js_value_type(result.value()) != JsNodeType::Integer ||
        fiber::script::js_value_int64(result.value()) != 0) {
        return -1;
    }
    return compiled.compile_us;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return options.help ? 0 : 2;
    }
    if (options.list) {
        for (const BenchmarkCase &benchmark_case: kCases) {
            std::printf("%.*s\n", static_cast<int>(benchmark_case.name.size()), benchmark_case.name.data());
        }
        return 0;
    }
    if (!fiber::script::jit::jit_is_available()) {
        std::fprintf(stderr, "LLVM script JIT is unavailable in this build\n");
        return 2;
    }

    std::printf("script_jit_benchmark build_type=%s compiler=", FIBER_SCRIPT_BENCHMARK_BUILD_TYPE);
#if defined(__clang__)
    std::printf("clang-%s", __clang_version__);
#elif defined(__GNUC__)
    std::printf("gcc-%s", __VERSION__);
#else
    std::printf("unknown");
#endif
    std::printf(" target_ms=%llu rounds=%u compile_samples=%u input_size=%u\n",
                static_cast<unsigned long long>(options.target_ms), options.rounds, options.compile_samples,
                options.input_size);

    BenchmarkLibrary library;
    double engine_warmup_us = warm_jit_engine(library);
    if (engine_warmup_us < 0) {
        std::fprintf(stderr, "JIT engine warmup failed\n");
        return 1;
    }
    std::printf("JIT_ENGINE warmup_compile_us=%.2f\n", engine_warmup_us);

    std::size_t executed = 0;
    long double log_speedup_sum = 0;
    for (const BenchmarkCase &benchmark_case: kCases) {
        if (!case_selected(benchmark_case, options)) {
            continue;
        }
        CaseResult result = run_case(library, benchmark_case, options);
        if (!result.ran) {
            return 1;
        }
        log_speedup_sum += std::log(result.speedup);
        ++executed;
    }
    if (executed == 0) {
        std::fprintf(stderr, "no benchmark case matched the filter\n");
        return 2;
    }
    double geometric_mean = std::exp(static_cast<double>(log_speedup_sum / executed));
    std::printf("SUMMARY cases=%zu geometric_mean_speedup=%.3f\n", executed, geometric_mean);
    return 0;
}
