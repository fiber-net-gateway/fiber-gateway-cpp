#include <fiber/http/ClientHttpExchange.h>

namespace fiber::http {

fiber::async::Task<common::IoResult<void>> ClientHttpExchange::invalid_void() noexcept {
    co_return std::unexpected(common::IoErr::Invalid);
}

fiber::async::Task<common::IoResult<std::size_t>> ClientHttpExchange::invalid_size() noexcept {
    co_return std::unexpected(common::IoErr::Invalid);
}

fiber::async::Task<common::IoResult<const ClientResponseHead *>> ClientHttpExchange::invalid_head() noexcept {
    co_return std::unexpected(common::IoErr::Invalid);
}

fiber::async::Task<common::IoResult<mem::IoBufChain>> ClientHttpExchange::invalid_body() noexcept {
    co_return std::unexpected(common::IoErr::Invalid);
}

} // namespace fiber::http
