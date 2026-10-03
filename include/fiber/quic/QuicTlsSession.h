#ifndef FIBER_QUIC_QUIC_TLS_SESSION_H
#define FIBER_QUIC_QUIC_TLS_SESSION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../net/TlsCredential.h"
#include "../net/TlsParams.h"
#include "../tls/TlsConfig.h"
#include "../tls/TlsTypes.h"
#include "../tls/crypto/TlsSecret.h"

namespace fiber::tls {
// src-side engines (src/tls/handshake): the session holds them by pointer.
class TlsClientHandshakeEngine;
class TlsServerHandshakeEngine;
} // namespace fiber::tls

namespace fiber::quic {

enum class QuicEncryptionLevel : std::uint8_t;
class QuicConnection;

// The QUIC-side TLS driver (10 §8): one in-tree handshake engine per role,
// run in its QUIC mode (TlsQuicCallbacks on the config). Records stay
// plaintext forever — every traffic secret is exported to the QUIC layer via
// quic_set_encryption_secret, outbound handshake messages land directly in
// CRYPTO frames, and the handshake tail is handed over through
// take_quic_result() instead of a TlsConnectedState — at which point the
// engine is released. After the handshake, app-level CRYPTO bytes feed the
// post-handshake consumer (10 §7):
// NewSessionTicket receipts on the client, fatal unexpected_message for
// everything else (KeyUpdate is packet-layer-only in QUIC, RFC 9001 §6).
//
// Contract face (init/provide/drive/post-handshake/alpn/alert stash) is the
// pre-10 shape; the engines must run and die on the connection's loop
// (IoBufChain node-pool affinity), which is where every entry point runs.
class QuicTlsSession : public common::NonCopyable, public common::NonMovable {
public:
    QuicTlsSession() noexcept = default;
    ~QuicTlsSession();

    [[nodiscard]] common::IoResult<void> init_server(const net::TlsServerParam &options,
                                                     QuicConnection &connection) noexcept;
    [[nodiscard]] common::IoResult<void> init_client(const net::TlsClientParam &param, QuicConnection &connection,
                                                     bool allow_insecure,
                                                     const tls::TlsSessionState *session = nullptr) noexcept;
    [[nodiscard]] common::IoResult<void> provide_crypto_data(QuicEncryptionLevel level, const std::uint8_t *data,
                                                             std::size_t len) noexcept;
    [[nodiscard]] common::IoResult<void> drive_handshake() noexcept;
    [[nodiscard]] common::IoResult<void> process_post_handshake() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool handshake_done() const noexcept;
    [[nodiscard]] bool session_reused() const noexcept;
    [[nodiscard]] std::string_view selected_alpn() const noexcept;
    [[nodiscard]] long peer_verify_result() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t> last_alert() const noexcept { return last_alert_; }
    // Capture a fatal alert reported by the engine's send_alert callback
    // during the handshake. drive_handshake() consumes it via
    // take_pending_alert() to emit a CRYPTO_ERROR close (RFC 9000 §20.1) —
    // never re-entrantly from inside the engine.
    void record_alert(std::uint8_t alert) noexcept;
    [[nodiscard]] std::optional<std::uint8_t> take_pending_alert() noexcept;

private:
    // TlsQuicCallbacks trampolines — static, ctx is the QuicTlsSession.
    static bool quic_set_secret_thunk(void *ctx, tls::TlsQuicLevel level, bool write_secret,
                                      tls::TlsCipherSuiteId suite, std::span<const std::uint8_t> secret) noexcept;
    static bool quic_add_data_thunk(void *ctx, tls::TlsQuicLevel level, std::span<const std::uint8_t> data) noexcept;
    static void quic_peer_params_thunk(void *ctx, std::span<const std::uint8_t> params) noexcept;
    static void quic_send_alert_thunk(void *ctx, tls::TlsAlertDesc alert) noexcept;

    // Callback bodies (the thunks forward here).
    [[nodiscard]] bool on_quic_set_secret(tls::TlsQuicLevel level, bool write_secret, tls::TlsCipherSuiteId suite,
                                          std::span<const std::uint8_t> secret) noexcept;
    [[nodiscard]] bool on_quic_add_data(tls::TlsQuicLevel level, std::span<const std::uint8_t> data) noexcept;
    void on_quic_peer_params(std::span<const std::uint8_t> params) noexcept;
    void on_quic_alert(tls::TlsAlertDesc alert) noexcept;
    // The 09 §4.1 selector shim: stages credentials per ClientHello through
    // the net configure callback.
    [[nodiscard]] const tls::TlsServerConfig *select_server_config(const tls::TlsClientHello &client_hello) noexcept;

    [[nodiscard]] common::IoResult<void> finish_handshake() noexcept; // done-transition handoff + gates
    [[nodiscard]] common::IoResult<void> fail_terminal() noexcept; // Failed event → error mapping (+ close)
    [[nodiscard]] common::IoResult<void> apply_peer_transport_params() noexcept;
    void take_engine_leftover() noexcept;
    [[nodiscard]] common::IoResult<void> pump_post_handshake() noexcept;
    void handle_new_session_ticket(std::span<const std::uint8_t> body) noexcept;

    [[nodiscard]] tls::TlsClientHandshakeEngine &client() noexcept;
    [[nodiscard]] tls::TlsServerHandshakeEngine &server() noexcept;
    [[nodiscard]] const tls::TlsClientHandshakeEngine &client() const noexcept;
    [[nodiscard]] const tls::TlsServerHandshakeEngine &server() const noexcept;

    QuicConnection *connection_ = nullptr; // borrowed; the session is a member
    // The role's engine, from init_* until the done-transition (or the dtor
    // on a failed/abandoned handshake).
    tls::TlsClientHandshakeEngine *client_ = nullptr;
    tls::TlsServerHandshakeEngine *server_ = nullptr;

    // The callbacks and configs borrow these members, so both must stay
    // address-stable for the engine's lifetime (built in init_*, never moved).
    tls::TlsQuicCallbacks quic_cb_{};
    tls::TlsClientConfig client_cfg_{};
    tls::TlsServerConfig server_cfg_{};
    tls::TlsServerConfigSource selector_{};
    tls::TlsTicketMinter minter_{};
    tls::TlsResumptionLookup lookup_{};
    const net::TlsServerParam *server_param_ = nullptr; // selector re-stage source
    // A credential the configure callback handed over through add_credential:
    // held until the done-transition (released after the engine) or a
    // terminal failure (the engine stops reading chain/key there), else
    // released after the engine in the dtor.
    net::TlsCredential credential_owner_;
    common::IoErr callback_error_ = common::IoErr::None;
    std::array<std::uint8_t, 16> ip_bytes_{}; // client verify_ip backing
    tls::TlsSessionOffer offer_{}; // staged resumption offer (spans borrow members)
    tls::TlsSessionState session_state_{}; // the offer's owning backing (identity + psk)
    std::array<std::uint8_t, 512> local_transport_params_{}; // 0x39 we send
    std::size_t local_transport_params_len_ = 0;
    std::array<std::uint8_t, 1024> peer_transport_params_{}; // 0x39 we received
    std::size_t peer_transport_params_len_ = 0;
    bool peer_transport_params_overflow_ = false;

    // Handshake tail (take_quic_result), latched once at the done-transition.
    tls::TlsSecret resumption_master_{}; // NST→PSK base for the receipt path
    std::array<std::uint8_t, 256> alpn_{};
    std::uint16_t alpn_len_ = 0;
    tls::TlsCipherSuiteId negotiated_suite_ = tls::TlsCipherSuiteId::TlsAes128GcmSha256;
    bool session_resumed_ = false;
    bool early_data_accepted_ = false;
    bool result_taken_ = false; // the done-transition ran
    // Highest level CRYPTO data was fed to the engine at; the engine never
    // goes back a level (RFC 9001 §4.1.3), so provide_crypto_data gates on it.
    tls::TlsQuicLevel provided_level_ = tls::TlsQuicLevel::Initial;
    bool verify_failed_ = false; // a certificate alert ended the handshake

    // Post-handshake consumer (10 §7): app-level CRYPTO bytes after the
    // engine is done, as a raw handshake-message stream.
    std::vector<std::uint8_t> post_buf_{};

    // Alert stashed by record_alert() (set from inside the engine) and
    // drained by drive_handshake() once the engine call returns, so
    // connection close state is never mutated re-entrantly.
    std::optional<std::uint8_t> pending_alert_;
    std::optional<std::uint8_t> last_alert_;
    bool client_mode_ = false;
    bool verify_peer_ = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_TLS_SESSION_H
