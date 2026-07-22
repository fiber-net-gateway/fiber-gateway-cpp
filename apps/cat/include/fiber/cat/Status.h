#ifndef FIBER_CAT_STATUS_H
#define FIBER_CAT_STATUS_H

#include <string_view>

namespace fiber::cat::status {

inline constexpr std::string_view Success = "0";
inline constexpr std::string_view Fail = "-1";
inline constexpr std::string_view Error = "ERROR";
inline constexpr std::string_view Incomplete = "CAT_CLIENT_INCOMPLETE";

} // namespace fiber::cat::status

#endif // FIBER_CAT_STATUS_H
