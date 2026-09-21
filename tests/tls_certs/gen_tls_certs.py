#!/usr/bin/env python3
"""Generate tests/TlsCertFixtures.h and tests/TlsSignatureVectors.h.

Machine-generated test material (the 02 §12.2 pipeline discipline): no hex
or PEM constant in the test files is transcribed by hand. Certificates,
private keys and signature KAT vectors are produced here, verified once in
Python (sign -> verify round trip), and emitted as C++ constants.

Run from the repository root:  python3 tests/tls_certs/gen_tls_certs.py
Requires: python3-cryptography (2.8+).
"""

import datetime
import ipaddress

from cryptography import x509
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519, padding, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

BACKEND = default_backend()
NOW = datetime.datetime.utcnow() - datetime.timedelta(days=1)  # ref time anchor

certs = {}  # name -> (cert_pem, )
keys = {}  # name -> private key object


def gen_keys():
    keys["rsa2048"] = rsa.generate_private_key(65537, 2048, BACKEND)
    keys["rsa1024"] = rsa.generate_private_key(65537, 1024, BACKEND)
    keys["p256"] = ec.generate_private_key(ec.SECP256R1(), BACKEND)
    keys["p384"] = ec.generate_private_key(ec.SECP384R1(), BACKEND)
    keys["p521"] = ec.generate_private_key(ec.SECP521R1(), BACKEND)
    keys["ed25519"] = ed25519.Ed25519PrivateKey.generate()


def key_pem(name, fmt=serialization.PrivateFormat.PKCS8, encryption=serialization.NoEncryption()):
    return keys[name].private_bytes(
        serialization.Encoding.PEM, fmt, encryption).decode()


def sign_hash_for(key_name, scheme):
    """(hash algo or None, is_pss) for a scheme id, or None if the key type
    cannot produce that scheme family at all."""
    if scheme.startswith("RsaPkcs1") or scheme.startswith("RsaPssRsae"):
        if not key_name.startswith("rsa"):
            return None
        h = {"256": hashes.SHA256, "384": hashes.SHA384, "512": hashes.SHA512}[scheme[-3:]]
        return h, scheme.startswith("RsaPss")
    if scheme.startswith("Ecdsa"):
        if not key_name.startswith("p"):
            return None
        return {"EcdsaSecp256r1Sha256": hashes.SHA256, "EcdsaSecp384r1Sha384": hashes.SHA384,
                "EcdsaSecp521r1Sha512": hashes.SHA512}[scheme], False
    if scheme == "Ed25519":
        return (None, False) if key_name == "ed25519" else None
    raise AssertionError(scheme)


def make_cert(subject_cn, issuer_cert, issuer_key_name, key_name, *, ca=False, eku=None, san=None,
              not_before=NOW, not_after=NOW + datetime.timedelta(days=3650)):
    """issuer_cert=None (with issuer_key_name=None) means self-signed."""
    public_key = keys[key_name].public_key()
    issuer = issuer_cert.subject if issuer_cert is not None else None
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject_cn)])
    builder = (x509.CertificateBuilder()
               .subject_name(subject)
               .issuer_name(issuer if issuer is not None else subject)
               .public_key(public_key)
               .serial_number(x509.random_serial_number())
               .not_valid_before(not_before)
               .not_valid_after(not_after))
    if ca:
        builder = builder.add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
    if san is not None:
        builder = builder.add_extension(x509.SubjectAlternativeName(san), critical=False)
    if eku is not None:
        builder = builder.add_extension(x509.ExtendedKeyUsage(eku), critical=False)
    signing_key = keys[issuer_key_name if issuer_key_name is not None else key_name]
    if isinstance(signing_key, ed25519.Ed25519PrivateKey):
        cert = builder.sign(signing_key, None, BACKEND)
    else:
        cert = builder.sign(signing_key, hashes.SHA256(), BACKEND)
    return cert


DNS_EXAMPLE = x509.DNSName("example.com")
DNS_WILDCARD = x509.DNSName("*.example.com")
IP_LEAF = x509.IPAddress(ipaddress.ip_address("192.168.7.1"))


def build_tree():
    certs["root"] = make_cert("Test Root CA", None, None, "rsa2048", ca=True)
    certs["intermediate"] = make_cert("Test Intermediate CA", certs["root"], "rsa2048", "rsa2048", ca=True)
    certs["root_unrelated"] = make_cert("Unrelated Root CA", None, None, "rsa2048", ca=True)

    leaf_san = [DNS_EXAMPLE, DNS_WILDCARD, IP_LEAF]
    certs["leaf_rsa"] = make_cert("leaf.rsa.example.com", certs["intermediate"], "rsa2048", "rsa2048",
                                  san=leaf_san, eku=[ExtendedKeyUsageOID.SERVER_AUTH])
    certs["leaf_ec_p256"] = make_cert("leaf.ec256.example.com", certs["intermediate"], "rsa2048", "p256",
                                      san=[DNS_EXAMPLE], eku=[ExtendedKeyUsageOID.SERVER_AUTH])
    certs["leaf_ec_p384"] = make_cert("leaf.ec384.example.com", certs["intermediate"], "rsa2048", "p384",
                                      san=[DNS_EXAMPLE], eku=[ExtendedKeyUsageOID.SERVER_AUTH])
    certs["leaf_ed25519"] = make_cert("leaf.ed25519.example.com", certs["intermediate"], "rsa2048", "ed25519",
                                      san=[DNS_EXAMPLE], eku=[ExtendedKeyUsageOID.SERVER_AUTH])
    certs["cert_p521"] = make_cert("cert.p521.example.com", certs["root"], "rsa2048", "p521", san=[DNS_EXAMPLE])
    certs["cert_rsa1024"] = make_cert("cert.rsa1024.example.com", certs["root"], "rsa2048", "rsa1024",
                                      san=[DNS_EXAMPLE])
    certs["client_rsa"] = make_cert("client.example.com", certs["intermediate"], "rsa2048", "rsa2048",
                                    san=[DNS_EXAMPLE], eku=[ExtendedKeyUsageOID.CLIENT_AUTH])
    certs["leaf_expired"] = make_cert("expired.example.com", certs["intermediate"], "rsa2048", "rsa2048",
                                      san=[DNS_EXAMPLE], not_before=NOW - datetime.timedelta(days=10),
                                      not_after=NOW - datetime.timedelta(days=5))
    certs["leaf_future"] = make_cert("future.example.com", certs["intermediate"], "rsa2048", "rsa2048",
                                     san=[DNS_EXAMPLE], not_before=NOW + datetime.timedelta(days=365),
                                     not_after=NOW + datetime.timedelta(days=730))
    certs["leaf_wrongname"] = make_cert("wrongname.example.com", certs["intermediate"], "rsa2048", "rsa2048",
                                        san=[x509.DNSName("other.example.com")])
    # CN only, no SAN — pins the SAN-only matching semantics.
    certs["leaf_cnonly"] = make_cert("example.com", certs["intermediate"], "rsa2048", "rsa2048")
    certs["leaf_selfsigned"] = make_cert("selfsigned.example.com", None, None, "rsa2048", san=[DNS_EXAMPLE])
    certs["leaf_unrelated"] = make_cert("unrelated.example.com", certs["root_unrelated"], "rsa2048", "rsa2048",
                                        san=[DNS_EXAMPLE])


def cert_pem(name):
    return certs[name].public_bytes(serialization.Encoding.PEM).decode()


def as_bytes(hex_str):
    return bytes.fromhex(hex_str)


# ---- signature vectors ------------------------------------------------------

CONTENT_A = bytes(range(64))  # arbitrary
CONTENT_B = (b"\x20" * 64 + b"TLS 1.3, server CertificateVerify" + b"\x00" + bytes(range(48)))
CONTENT_C = bytes((i * 7) & 0xFF for i in range(48))  # TLS 1.2 digest-sized content

SCHEME_IDS = ["RsaPkcs1Sha256", "RsaPkcs1Sha384", "RsaPkcs1Sha512", "EcdsaSecp256r1Sha256",
              "EcdsaSecp384r1Sha384", "EcdsaSecp521r1Sha512", "Ed25519", "RsaPssRsaeSha256",
              "RsaPssRsaeSha384", "RsaPssRsaeSha512"]

KEY_ORDER = ["rsa2048", "rsa1024", "p256", "p384", "p521", "ed25519"]
# Certificate whose public key pairs with each signing key (for verify).
KEY_CERT = {"rsa2048": "leaf_rsa", "rsa1024": "cert_rsa1024", "p256": "leaf_ec_p256",
            "p384": "leaf_ec_p384", "p521": "cert_p521", "ed25519": "leaf_ed25519"}


def supports(key_name, scheme, version):
    """Mirror of the C++ matching table (02b §3.2) — used only to pick the
    version tag for each vector; the authoritative matrix lives in the C++
    test."""
    digest_len = {"256": 32, "384": 48, "512": 64}
    if key_name.startswith("rsa"):
        if not (scheme.startswith("RsaPkcs1") or scheme.startswith("RsaPssRsae")):
            return False
        if scheme.startswith("RsaPss"):
            mod = 2048 if key_name == "rsa2048" else 1024
            if mod // 8 < 2 * digest_len[scheme[-3:]] + 2:
                return False
        if scheme.startswith("RsaPkcs1") and version == "1.3":
            return False  # never signs a 1.3 handshake
        return True
    if key_name.startswith("p"):
        if not scheme.startswith("Ecdsa"):
            return False
        if version == "1.3":
            curve = {"p256": "256r1", "p384": "384r1", "p521": "521r1"}[key_name]
            if curve not in scheme:
                return False
        return True
    return scheme == "Ed25519"


def python_sign(key_name, scheme, content):
    key = keys[key_name]
    info = sign_hash_for(key_name, scheme)
    if info is None:
        return None
    hash_cls, is_pss = info
    if key_name == "ed25519":
        return key.sign(content)
    if is_pss:
        pad = padding.PSS(mgf=padding.MGF1(hash_cls()), salt_length=hash_cls().digest_size)
        return key.sign(content, pad, hash_cls())
    if key_name.startswith("rsa"):
        return key.sign(content, padding.PKCS1v15(), hash_cls())
    return key.sign(content, ec.ECDSA(hash_cls()))


def python_verify(cert, key_name, scheme, content, sig):
    pub = cert.public_key()
    info = sign_hash_for(key_name, scheme)
    hash_cls, is_pss = info
    if key_name == "ed25519":
        pub.verify(sig, content)
    elif is_pss:
        pub.verify(sig, content, padding.PSS(mgf=padding.MGF1(hash_cls()), salt_length=hash_cls().digest_size),
                   hash_cls())
    elif key_name.startswith("rsa"):
        pub.verify(sig, content, padding.PKCS1v15(), hash_cls())
    else:
        pub.verify(sig, content, ec.ECDSA(hash_cls()))


def build_vectors():
    vectors = []  # dicts with emitted constant names resolved
    key_const = {"rsa2048": "certfix::kRsa2048KeyPem", "rsa1024": "certfix::kRsa1024KeyPem",
                 "p256": "certfix::kP256KeyPem", "p384": "certfix::kP384KeyPem",
                 "p521": "certfix::kP521KeyPem", "ed25519": "certfix::kEd25519KeyPem"}
    cert_const = {"leaf_rsa": "certfix::kLeafRsaPem", "cert_rsa1024": "certfix::kCertRsa1024Pem",
                  "leaf_ec_p256": "certfix::kLeafEcP256Pem", "leaf_ec_p384": "certfix::kLeafEcP384Pem",
                  "cert_p521": "certfix::kCertP521Pem", "leaf_ed25519": "certfix::kLeafEd25519Pem"}
    for key_name in KEY_ORDER:
        for scheme in SCHEME_IDS:
            for version in ("1.2", "1.3"):
                if not supports(key_name, scheme, version):
                    continue
                # 1.3 vectors carry the context-string shape, 1.2 vectors the
                # digest-sized shape (02b §3.4 forms).
                content_name, content = ("B", CONTENT_B) if version == "1.3" else ("C", CONTENT_C)
                sig = python_sign(key_name, scheme, content)
                assert sig is not None
                python_verify(certs[KEY_CERT[key_name]], key_name, scheme, content, sig)
                vectors.append({"key_pem": key_const[key_name], "cert_pem": cert_const[KEY_CERT[key_name]],
                                "scheme": scheme, "version": version, "content": content,
                                "content_name": content_name, "sig": sig})
    return vectors


# ---- emission ---------------------------------------------------------------

def emit_cert_fixtures(f):
    w = f.write
    w("// MACHINE-GENERATED by tests/tls_certs/gen_tls_certs.py — do not edit.\n")
    w("// PEM test material for TlsCertificateTest / TlsSignatureTest.\n")
    w("#ifndef FIBER_TESTS_TLS_CERT_FIXTURES_H\n")
    w("#define FIBER_TESTS_TLS_CERT_FIXTURES_H\n\n")
    w("#include <cstdint>\n\n")
    w("namespace fiber::tls::certfix {\n\n")
    w("// Reference instant the tree was generated around (verification-time\n")
    w("// injection anchor: expired/future leaves are relative to this).\n")
    epoch = int((NOW - datetime.datetime(1970, 1, 1)).total_seconds() * 1000)
    w(f"inline constexpr std::int64_t kRefNowMs = {epoch}LL;\n")
    w("inline constexpr std::int64_t kDayMs = 86400000;\n\n")

    pem_entries = [
        ("kRootRsaPem", "root"), ("kIntermediateRsaPem", "intermediate"),
        ("kLeafRsaPem", "leaf_rsa"), ("kLeafEcP256Pem", "leaf_ec_p256"),
        ("kLeafEcP384Pem", "leaf_ec_p384"), ("kLeafEd25519Pem", "leaf_ed25519"),
        ("kCertP521Pem", "cert_p521"), ("kCertRsa1024Pem", "cert_rsa1024"),
        ("kClientRsaPem", "client_rsa"), ("kLeafExpiredPem", "leaf_expired"),
        ("kLeafFuturePem", "leaf_future"), ("kLeafWrongNamePem", "leaf_wrongname"),
        ("kLeafCnOnlyPem", "leaf_cnonly"), ("kLeafSelfSignedPem", "leaf_selfsigned"),
        ("kRootUnrelatedPem", "root_unrelated"), ("kLeafUnrelatedPem", "leaf_unrelated"),
    ]
    for const, name in pem_entries:
        pem = cert_pem(name)
        w(f"// {name}\ninline constexpr char {const}[] =\n")
        for line in pem.splitlines():
            w(f'    "{line}\\n"\n')
        w(";\n\n")

    key_entries = [
        ("kRsa2048KeyPem", key_pem("rsa2048")),
        ("kRsa1024KeyPem", key_pem("rsa1024")),
        ("kP256KeyPem", key_pem("p256")),
        ("kP384KeyPem", key_pem("p384")),
        ("kP521KeyPem", key_pem("p521")),
        ("kEd25519KeyPem", key_pem("ed25519")),
        ("kRsa2048KeyPkcs1Pem", key_pem("rsa2048", serialization.PrivateFormat.TraditionalOpenSSL)),
        ("kP256KeySec1Pem", key_pem("p256", serialization.PrivateFormat.TraditionalOpenSSL)),
        ("kRsa2048KeyEncryptedPem",
         key_pem("rsa2048", serialization.PrivateFormat.PKCS8, serialization.BestAvailableEncryption(b"pw"))),
    ]
    for const, pem in key_entries:
        w(f"inline constexpr char {const}[] =\n")
        for line in pem.splitlines():
            w(f'    "{line}\\n"\n')
        w(";\n\n")
    w("} // namespace fiber::tls::certfix\n\n")
    w("#endif // FIBER_TESTS_TLS_CERT_FIXTURES_H\n")


def emit_sig_vectors(f, vectors):
    w = f.write
    w("// MACHINE-GENERATED by tests/tls_certs/gen_tls_certs.py — do not edit.\n")
    w("// Signature KAT vectors: sign(content) with the fixture key, verified\n")
    w("// once in Python before emission. Consumed by TlsSignatureTest.\n")
    w("#ifndef FIBER_TESTS_TLS_SIGNATURE_VECTORS_H\n")
    w("#define FIBER_TESTS_TLS_SIGNATURE_VECTORS_H\n\n")
    w("#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <span>\n\n")
    w("#include <fiber/tls/TlsVersion.h>\n")
    w("#include <fiber/tls/handshake/TlsCipherSuites.h>\n\n")
    w("namespace fiber::tls::sigvec {\n\n")

    w("inline constexpr std::array<std::uint8_t, 64> kContentA{")
    w(",".join(str(b) for b in CONTENT_A))
    w("};\n")
    w(f"inline constexpr std::array<std::uint8_t, {len(CONTENT_B)}> kContentB{{")
    w(",".join(str(b) for b in CONTENT_B))
    w("};\n")
    w(f"inline constexpr std::array<std::uint8_t, {len(CONTENT_C)}> kContentC{{")
    w(",".join(str(b) for b in CONTENT_C))
    w("};\n\n")

    w("struct KatVector {\n")
    w("    const char *key_pem;      // certfix constant\n")
    w("    const char *cert_pem;     // certificate pairing with key_pem\n")
    w("    fiber::tls::TlsSignatureScheme scheme;\n")
    w("    fiber::tls::TlsProtocolVersion version;\n")
    w("    std::span<const std::uint8_t> content;\n")
    w("    std::span<const std::uint8_t> signature;\n")
    w("};\n\n")

    lines = []
    for idx, vec in enumerate(vectors):
        const = f"kSig{idx:02d}"
        w(f"inline constexpr std::array<std::uint8_t, {len(vec['sig'])}> {const}{{")
        w(",".join(str(b) for b in vec["sig"]))
        w("};\n")
        cpp_version = "Tls13" if vec["version"] == "1.3" else "Tls12"
        lines.append(f"    KatVector{{{vec['key_pem']}, {vec['cert_pem']}, "
                     f"fiber::tls::TlsSignatureScheme::{vec['scheme']}, "
                     f"fiber::tls::TlsProtocolVersion::{cpp_version}, "
                     f"kContent{vec['content_name']}, {const}}},")
    w(f"inline constexpr std::array<KatVector, {len(vectors)}> kVectors{{\n")
    w("\n".join(lines))
    w("\n};\n\n")
    w("} // namespace fiber::tls::sigvec\n\n")
    w("#endif // FIBER_TESTS_TLS_SIGNATURE_VECTORS_H\n")


def main():
    gen_keys()
    build_tree()
    vectors = build_vectors()
    with open("tests/TlsCertFixtures.h", "w") as f:
        emit_cert_fixtures(f)
    with open("tests/TlsSignatureVectors.h", "w") as f:
        emit_sig_vectors(f, vectors)
    print(f"certs: {len(certs)}, keys: {len(keys)}, vectors: {len(vectors)}")


if __name__ == "__main__":
    main()
