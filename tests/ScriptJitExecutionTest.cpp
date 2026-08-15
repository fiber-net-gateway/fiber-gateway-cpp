#include <gtest/gtest.h>

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/script/Library.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/std/StdLibrary.h>

namespace {

fiber::script::ScriptCompileOptions require_jit() {
    fiber::script::ScriptCompileOptions options;
    options.backend = fiber::script::ScriptBackendMode::RequireJit;
    return options;
}

#if FIBER_ENABLE_SCRIPT_JIT

class DelayedValue final {
public:
    struct Awaiter {
        DelayedValue *owner = nullptr;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> continuation) const noexcept { owner->continuation_ = continuation; }
        fiber::script::JsValue await_resume() const noexcept { return owner->value_; }
    };

    [[nodiscard]] Awaiter awaiter() noexcept { return Awaiter{this}; }
    [[nodiscard]] bool suspended() const noexcept { return continuation_ != nullptr; }

    void complete(fiber::script::JsValue value) noexcept {
        value_ = value;
        std::coroutine_handle<> continuation = continuation_;
        continuation_ = nullptr;
        if (continuation) {
            continuation.resume();
        }
    }

private:
    fiber::script::JsValue value_ = fiber::script::JsValue::make_undefined();
    std::coroutine_handle<> continuation_ = nullptr;
};

fiber::script::AbiResult collect_now(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                     fiber::script::Library::Arguments arguments) noexcept {
    (void) userdata;
    (void) arguments;
    frame.runtime.collect();
    frame.runtime.collect();
    return fiber::script::AbiResult::success(fiber::script::JsValue::make_undefined());
}

fiber::script::AbiResult inspect_after_collect(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                               fiber::script::Library::Arguments arguments) noexcept {
    (void) userdata;
    if (arguments.argc != 1) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::InvalidState);
    }
    frame.runtime.collect();
    frame.runtime.collect();
    fiber::script::ValueHandle out = frame.runtime.local_value();
    if (!out) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::OutOfMemory);
    }
    if (!fiber::script::gc_object_get_key(&frame.runtime, arguments.args, "a", 1, out)) {
        return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::OutOfMemory);
    }
    return fiber::script::AbiResult::success(*out);
}

fiber::script::AsyncTask async_sum(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                   fiber::script::Library::Arguments arguments) noexcept {
    (void) userdata;
    frame.runtime.collect();
    frame.runtime.collect();
    std::int64_t sum = 0;
    for (std::uint32_t i = 0; i < arguments.argc; ++i) {
        sum += js_value_int64(arguments.args[i]);
    }
    co_return fiber::script::AbiResult::success(fiber::script::JsValue::make_integer(sum));
}

fiber::script::AsyncTask async_boom(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                    fiber::script::Library::Arguments arguments) noexcept {
    (void) userdata;
    (void) frame;
    (void) arguments;
    co_return fiber::script::AbiResult::exception(fiber::script::JsValue::make_integer(41));
}

fiber::script::AsyncTask async_abort(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                     fiber::script::Library::Arguments arguments) noexcept {
    (void) userdata;
    (void) frame;
    (void) arguments;
    co_return fiber::script::AbiResult::abort(fiber::script::ScriptAbortReason::HostFault);
}

fiber::script::AsyncTask async_wait(void *userdata, const fiber::script::Library::HostCallFrame &frame,
                                    fiber::script::Library::Arguments arguments) noexcept {
    (void) frame;
    (void) arguments;
    auto *delayed = static_cast<DelayedValue *>(userdata);
    fiber::script::JsValue value = co_await delayed->awaiter();
    co_return fiber::script::AbiResult::success(value);
}

fiber::script::AbiResult sync_constant(void *userdata, const fiber::script::Library::HostCallFrame &frame) noexcept {
    (void) userdata;
    (void) frame;
    return fiber::script::AbiResult::success(fiber::script::JsValue::make_integer(7));
}

fiber::script::AsyncTask async_constant(void *userdata, const fiber::script::Library::HostCallFrame &frame) noexcept {
    (void) userdata;
    (void) frame;
    co_return fiber::script::AbiResult::success(fiber::script::JsValue::make_integer(8));
}

class GcLibrary final : public fiber::script::Library {
public:
    explicit GcLibrary(DelayedValue *delayed = nullptr) {
        collect_.kind = HostCallable::Kind::SyncFunction;
        collect_.function = &collect_now;
        collect_.debug_name = "collect";
        inspect_.kind = HostCallable::Kind::SyncFunction;
        inspect_.function = &inspect_after_collect;
        inspect_.debug_name = "inspect";
        async_sum_.kind = HostCallable::Kind::AsyncFunction;
        async_sum_.async_function = &async_sum;
        async_sum_.debug_name = "asyncSum";
        async_boom_.kind = HostCallable::Kind::AsyncFunction;
        async_boom_.async_function = &async_boom;
        async_boom_.debug_name = "asyncBoom";
        async_abort_.kind = HostCallable::Kind::AsyncFunction;
        async_abort_.async_function = &async_abort;
        async_abort_.debug_name = "asyncAbort";
        async_wait_.kind = HostCallable::Kind::AsyncFunction;
        async_wait_.userdata = delayed;
        async_wait_.async_function = &async_wait;
        async_wait_.debug_name = "asyncWait";
        constant_.kind = HostCallable::Kind::SyncConstant;
        constant_.constant = &sync_constant;
        constant_.debug_name = "$env.value";
        async_constant_.kind = HostCallable::Kind::AsyncConstant;
        async_constant_.async_constant = &async_constant;
        async_constant_.debug_name = "$env.asyncValue";
    }

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override {
        const HostCallable *callable = nullptr;
        std::uint16_t argc = 0;
        if (name == "collect") {
            callable = &collect_;
        } else if (name == "inspect") {
            callable = &inspect_;
            argc = 1;
        } else {
            return FunctionMatchResult::not_found();
        }
        if (request.has_spread || request.known_argc != argc) {
            return FunctionMatchResult::arity_mismatch();
        }
        FunctionSignature signature;
        signature.required_argc = argc;
        signature.fixed_argc = argc;
        signature.variadic = false;
        return FunctionMatchResult::found(callable, signature, nullptr, 0);
    }

    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override {
        const HostCallable *callable = nullptr;
        FunctionSignature signature;
        if (name == "asyncSum") {
            callable = &async_sum_;
            signature.variadic = true;
        } else if (name == "asyncBoom") {
            if (request.has_spread || request.known_argc != 0) {
                return FunctionMatchResult::arity_mismatch();
            }
            callable = &async_boom_;
            signature.variadic = false;
        } else if (name == "asyncAbort") {
            if (request.has_spread || request.known_argc != 0) {
                return FunctionMatchResult::arity_mismatch();
            }
            callable = &async_abort_;
            signature.variadic = false;
        } else if (name == "asyncWait" && async_wait_.userdata) {
            if (request.has_spread || request.known_argc != 0) {
                return FunctionMatchResult::arity_mismatch();
            }
            callable = &async_wait_;
            signature.variadic = false;
        } else {
            return FunctionMatchResult::not_found();
        }
        return FunctionMatchResult::found(callable, signature, nullptr, 0);
    }

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override {
        return namespace_name == "$env" && key == "value" ? &constant_ : nullptr;
    }

    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override {
        return namespace_name == "$env" && key == "asyncValue" ? &async_constant_ : nullptr;
    }

    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals) const override {
        (void) type;
        (void) name;
        (void) literals;
        return nullptr;
    }

private:
    HostCallable collect_{};
    HostCallable inspect_{};
    HostCallable async_sum_{};
    HostCallable async_boom_{};
    HostCallable async_abort_{};
    HostCallable async_wait_{};
    HostCallable constant_{};
    HostCallable async_constant_{};
};

class ManualTask final {
public:
    struct promise_type {
        ManualTask get_return_object() noexcept { return ManualTask{handle_type::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit ManualTask(handle_type handle) noexcept : handle_(handle) {}
    ManualTask(const ManualTask &) = delete;
    ManualTask &operator=(const ManualTask &) = delete;
    ManualTask(ManualTask &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    ~ManualTask() {
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
    handle_type handle_ = nullptr;
};

ManualTask run_async_script(fiber::script::Script *script, fiber::script::GcHeap *heap,
                            fiber::script::ScriptResult *result, bool *done) {
    *result = co_await script->exec_async(fiber::script::JsValue::make_undefined(), nullptr, *heap);
    *done = true;
}

TEST(ScriptJitExecutionTest, ExecutesArithmetic) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(library, "return 1 + 2 * 3;", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;
    ASSERT_TRUE(script->uses_jit());
    EXPECT_EQ(script->jit_compile_error(), nullptr);

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 7);
}

TEST(ScriptJitExecutionTest, PreservesVoidAndUndefinedReturnKinds) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto void_script = fiber::script::compile_script(library, "return;", require_jit());
    auto undefined_script = fiber::script::compile_script(library, "let x; return x;", require_jit());
    ASSERT_TRUE(void_script.has_value()) << void_script.error().message;
    ASSERT_TRUE(undefined_script.has_value()) << undefined_script.error().message;

    fiber::script::GcHeap heap;
    auto void_result = void_script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    auto undefined_result = undefined_script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    EXPECT_TRUE(void_result.is_void());
    ASSERT_TRUE(undefined_result.has_value());
    EXPECT_TRUE(fiber::script::js_value_is_undefined(undefined_result.value()));
}

TEST(ScriptJitExecutionTest, ExecutesBranchAndPhi) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(
            library, "let a = 2; let x = 1; if (a > 1) { x = 5; } else { x = 7; } return x + 1;", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 6);
}

TEST(ScriptJitExecutionTest, LoadsRootValue) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(library, "return $ + 1;", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_integer(8), nullptr, heap);
    ASSERT_TRUE(result.has_value()) << "result kind=" << static_cast<int>(result.kind);
    EXPECT_EQ(js_value_int64(result.value()), 9);
}

TEST(ScriptJitExecutionTest, ExecutesIteratorLoop) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(
            library, "let sum = 0; for (let k, v of [10, 20, 30]) { sum = sum + v; } return sum;", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 60);
}

TEST(ScriptJitExecutionTest, ExecutesObjectArrayAndSpreadOperations) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(
            library,
            "let o = {a: 1, ...{b: 2}}; let a = [1, ...[2, 3]]; o.a = 5; a[1] = 4; return o.a + o.b + a[1] + a[2];",
            require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 14);
}

TEST(ScriptJitExecutionTest, CatchesRuntimeOperationException) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(
            library, "try { let d = 0; let x = 5 / d; return 0; } catch (e) { return 9; }", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 9);
}

TEST(ScriptJitExecutionTest, CatchesIteratorMutationException) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(
            library, "let o = {a: 1, b: 2}; try { for (let k, v of o) { o.c = 3; } return 0; } catch (e) { return e; }",
            require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(fiber::script::js_value_type(result.value()), fiber::script::JsNodeType::Exception);
    EXPECT_EQ(fiber::script::js_value_exception_kind(result.value()), fiber::script::ExceptionKind::IterationError);
}

TEST(ScriptJitExecutionTest, StackMapKeepsSsaHeapValueAlive) {
    GcLibrary library;
    auto script = fiber::script::compile_script(library, "let x = {a: 7}; collect(); return x.a;", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 7);
}

TEST(ScriptJitExecutionTest, StackMapKeepsCallOnlyArgumentAlive) {
    GcLibrary library;
    auto script = fiber::script::compile_script(library, "return inspect({a: 23});", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 23);
}

TEST(ScriptJitExecutionTest, CatchesThrownValue) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(library, "try { throw 4; } catch (e) { return e + 1; }", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 5);
}

TEST(ScriptJitExecutionTest, ResumesAsyncWithPersistentValuesAndArguments) {
    GcLibrary library;
    auto script = fiber::script::compile_script(library, "let x = {a: 7}; let y = asyncSum(2, 3); return x.a + y;",
                                                require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    fiber::script::ScriptResult result;
    bool done = false;
    ManualTask task = run_async_script(&*script, &heap, &result, &done);
    task.resume();
    EXPECT_TRUE(done);
    EXPECT_TRUE(task.done());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 12);
}

TEST(ScriptJitExecutionTest, ResumesAsyncExceptionIntoCatch) {
    GcLibrary library;
    auto script = fiber::script::compile_script(library, "try { asyncBoom(); return 0; } catch (e) { return e + 1; }",
                                                require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    fiber::script::ScriptResult result;
    bool done = false;
    ManualTask task = run_async_script(&*script, &heap, &result, &done);
    task.resume();
    EXPECT_TRUE(done);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 42);
}

TEST(ScriptJitExecutionTest, PreservesAsyncAbortAndAddsSourcePosition) {
    GcLibrary library;
    auto script = fiber::script::compile_script(library, "return asyncAbort();", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    fiber::script::ScriptResult result;
    bool done = false;
    ManualTask task = run_async_script(&*script, &heap, &result, &done);
    task.resume();
    EXPECT_TRUE(done);
    ASSERT_TRUE(result.is_abort());
    EXPECT_EQ(result.abort().reason, fiber::script::ScriptAbortReason::HostFault);
    EXPECT_GE(result.abort().position, 0);
}

TEST(ScriptJitExecutionTest, ActiveVmOwnsCodeAfterScriptDestruction) {
    DelayedValue delayed;
    GcLibrary library(&delayed);
    auto compiled = fiber::script::compile_script(library, "return asyncWait();", require_jit());
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message;
    auto script = std::make_unique<fiber::script::Script>(std::move(*compiled));

    fiber::script::GcHeap heap;
    fiber::script::ScriptResult result;
    bool done = false;
    ManualTask task = run_async_script(script.get(), &heap, &result, &done);
    task.resume();
    ASSERT_FALSE(done);
    ASSERT_TRUE(delayed.suspended());

    script.reset();
    delayed.complete(fiber::script::JsValue::make_integer(17));
    EXPECT_TRUE(done);
    EXPECT_TRUE(task.done());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 17);
}

TEST(ScriptJitExecutionTest, OwnsSpreadArgumentsAcrossAsyncSuspend) {
    GcLibrary library;
    auto script = fiber::script::compile_script(library, "return asyncSum(...[2, 3, 4]);", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    fiber::script::ScriptResult result;
    bool done = false;
    ManualTask task = run_async_script(&*script, &heap, &result, &done);
    task.resume();
    EXPECT_TRUE(done);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 9);
}

TEST(ScriptJitExecutionTest, ResumesMultipleAsyncSitesInsideLoop) {
    GcLibrary library;
    auto script = fiber::script::compile_script(
            library, "let sum = asyncSum(1); for (let k, v of [2, 3, 4]) { sum = asyncSum(sum, v); } return sum;",
            require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    fiber::script::ScriptResult result;
    bool done = false;
    ManualTask task = run_async_script(&*script, &heap, &result, &done);
    task.resume();
    EXPECT_TRUE(done);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 10);
}

TEST(ScriptJitExecutionTest, ExecutesSyncAndAsyncConstants) {
    GcLibrary library;
    auto script = fiber::script::compile_script(library, "return $env.value + $env.asyncValue;", require_jit());
    ASSERT_TRUE(script.has_value()) << script.error().message;

    fiber::script::GcHeap heap;
    fiber::script::ScriptResult result;
    bool done = false;
    ManualTask task = run_async_script(&*script, &heap, &result, &done);
    task.resume();
    EXPECT_TRUE(done);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 15);
}

#else

TEST(ScriptJitExecutionTest, RequireJitReportsDisabledBuild) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto script = fiber::script::compile_script(library, "return 1;", require_jit());
    ASSERT_FALSE(script.has_value());
    EXPECT_NE(script.error().message.find("disabled"), std::string::npos);
}

TEST(ScriptJitExecutionTest, PreferJitFallsBackWhenDisabled) {
    auto &library = fiber::script::std_lib::StdLibrary::instance();
    auto options = require_jit();
    options.backend = fiber::script::ScriptBackendMode::PreferJit;
    auto script = fiber::script::compile_script(library, "return 3;", options);
    ASSERT_TRUE(script.has_value()) << script.error().message;
    EXPECT_FALSE(script->uses_jit());
    EXPECT_NE(script->jit_compile_error(), nullptr);

    fiber::script::GcHeap heap;
    auto result = script->exec_sync(fiber::script::JsValue::make_undefined(), nullptr, heap);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(js_value_int64(result.value()), 3);
}

#endif

} // namespace
