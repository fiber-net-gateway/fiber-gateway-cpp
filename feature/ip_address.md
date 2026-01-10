# IP/SocketAddress Design (IPv4/IPv6 + Port)

## Goals
- Represent IPv4/IPv6 addresses and ports as value types.
- Avoid direct use of `sockaddr_*` outside the net layer.
- Provide conversion helpers for system calls (`bind`, `connect`, `accept`).
- Keep port in host byte order; convert only at boundaries.

## API Sketch
```cpp
namespace fiber::net {

enum class IpFamily : uint8_t { V4, V6 };

class IpAddress {
public:
    static IpAddress v4(std::array<uint8_t, 4> bytes);
    static IpAddress v6(std::array<uint8_t, 16> bytes, uint32_t scope_id = 0);

    static IpAddress any_v4();
    static IpAddress any_v6();
    static IpAddress loopback_v4();
    static IpAddress loopback_v6();

    [[nodiscard]] IpFamily family() const noexcept;
    [[nodiscard]] bool is_v4() const noexcept;
    [[nodiscard]] bool is_v6() const noexcept;
    [[nodiscard]] uint32_t scope_id() const noexcept;
    [[nodiscard]] const std::array<uint8_t, 4> &v4_bytes() const;
    [[nodiscard]] const std::array<uint8_t, 16> &v6_bytes() const;

    static bool parse(std::string_view text, IpAddress &out);
    std::string to_string() const;

    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_unspecified() const noexcept;
    [[nodiscard]] bool is_multicast() const noexcept;
};

class SocketAddress {
public:
    SocketAddress();
    SocketAddress(IpAddress ip, uint16_t port);

    static SocketAddress any_v4(uint16_t port = 0);
    static SocketAddress any_v6(uint16_t port = 0);

    [[nodiscard]] const IpAddress &ip() const noexcept;
    [[nodiscard]] uint16_t port() const noexcept;
    [[nodiscard]] IpFamily family() const noexcept;

    std::string to_string() const; // "127.0.0.1:80" or "[::1]:80"

    bool to_sockaddr(sockaddr_storage &out, socklen_t &len) const noexcept;
    static bool from_sockaddr(const sockaddr *addr, socklen_t len, SocketAddress &out);
};

} // namespace fiber::net
```

## Semantics
- `IpAddress` stores raw bytes plus a family tag; IPv6 includes `scope_id`.
- `SocketAddress` stores `IpAddress` + `port` in host byte order.
- `parse()` uses `inet_pton`, `to_string()` uses `inet_ntop`.
- `to_sockaddr()` fills `sockaddr_in`/`sockaddr_in6` and converts port with `htons`.
- `from_sockaddr()` reverses `ntohs` and extracts bytes from the sockaddr.
- `SocketAddress::any_v4/any_v6` are convenience factories for unspecified binds.
- `is_loopback/is_unspecified/is_multicast` allow quick address classification.

## Integration Points
- `TcpListener::bind(SocketAddress, ListenOptions, int*)`
- `AcceptResult.peer` becomes `SocketAddress`.
