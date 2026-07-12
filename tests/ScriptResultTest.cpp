#include <gtest/gtest.h>

#include <string_view>

#include "script/JsValue.h"
#include "script/ScriptResult.h"

using fiber::script::ExceptionKind;
using fiber::script::ScriptAbortReason;

TEST(ScriptResultTest, ExceptionKindNameCoversAllKinds) {
    using fiber::script::exception_kind_name;
    EXPECT_EQ(exception_kind_name(ExceptionKind::TypeError), "TypeError");
    EXPECT_EQ(exception_kind_name(ExceptionKind::RangeError), "RangeError");
    EXPECT_EQ(exception_kind_name(ExceptionKind::ReferenceError), "ReferenceError");
    EXPECT_EQ(exception_kind_name(ExceptionKind::IterationError), "IterationError");
}

TEST(ScriptResultTest, AbortReasonNameCoversAllReasons) {
    using fiber::script::abort_reason_name;
    EXPECT_EQ(abort_reason_name(ScriptAbortReason::None), "None");
    EXPECT_EQ(abort_reason_name(ScriptAbortReason::OutOfMemory), "OutOfMemory");
    EXPECT_EQ(abort_reason_name(ScriptAbortReason::InvalidState), "InvalidState");
    EXPECT_EQ(abort_reason_name(ScriptAbortReason::InvalidOpcode), "InvalidOpcode");
    EXPECT_EQ(abort_reason_name(ScriptAbortReason::HostFault), "HostFault");
    EXPECT_EQ(abort_reason_name(ScriptAbortReason::Timeout), "Timeout");
    EXPECT_EQ(abort_reason_name(ScriptAbortReason::Cancelled), "Cancelled");
    EXPECT_EQ(abort_reason_name(ScriptAbortReason::Internal), "Internal");
}
