// Digital signatures. Keys and signatures are Bytes; no implicit text encoding.
#include "runtime/digest.h"
#include "runtime/signature.h"
#include "runtime/stdlib_registry.h"

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#ifdef SPUTNIK_HAVE_NETTLE_GOST
#include <nettle/ecc-curve.h>
#include <nettle/gostdsa.h>
#endif

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace sputnik::runtime {
namespace {

enum class Algorithm {
  Ed25519,
  Ed448,
  EcdsaP256,
  EcdsaP384,
  RsaPssSha256,
  RsaPssSha384,
  MlDsa44,
  MlDsa65,
  MlDsa87,
  Gost2012_256,
  Gost2012_512
};

std::optional<Algorithm> parse_algorithm(const std::string &name) {
  if (name == "ed25519")
    return Algorithm::Ed25519;
  if (name == "ed448")
    return Algorithm::Ed448;
  if (name == "ecdsa_p256_sha256")
    return Algorithm::EcdsaP256;
  if (name == "ecdsa_p384_sha384")
    return Algorithm::EcdsaP384;
  if (name == "rsa_pss_sha256")
    return Algorithm::RsaPssSha256;
  if (name == "rsa_pss_sha384")
    return Algorithm::RsaPssSha384;
  if (name == "ml_dsa_44")
    return Algorithm::MlDsa44;
  if (name == "ml_dsa_65")
    return Algorithm::MlDsa65;
  if (name == "ml_dsa_87")
    return Algorithm::MlDsa87;
  if (name == "gost2012_256")
    return Algorithm::Gost2012_256;
  if (name == "gost2012_512")
    return Algorithm::Gost2012_512;
  return std::nullopt;
}

bool is_gost(Algorithm algorithm) {
  return algorithm == Algorithm::Gost2012_256 ||
         algorithm == Algorithm::Gost2012_512;
}

bool is_ed(Algorithm algorithm) {
  return algorithm == Algorithm::Ed25519 || algorithm == Algorithm::Ed448;
}

bool is_rsa(Algorithm algorithm) {
  return algorithm == Algorithm::RsaPssSha256 ||
         algorithm == Algorithm::RsaPssSha384;
}

bool is_ml_dsa(Algorithm algorithm) {
  return algorithm == Algorithm::MlDsa44 || algorithm == Algorithm::MlDsa65 ||
         algorithm == Algorithm::MlDsa87;
}

const char *ml_dsa_name(Algorithm algorithm) {
  if (algorithm == Algorithm::MlDsa44)
    return "ML-DSA-44";
  if (algorithm == Algorithm::MlDsa65)
    return "ML-DSA-65";
  return "ML-DSA-87";
}

bool openssl_available(Algorithm algorithm) {
  EVP_PKEY_CTX *context =
      is_ml_dsa(algorithm)
          ? EVP_PKEY_CTX_new_from_name(nullptr, ml_dsa_name(algorithm), nullptr)
          : EVP_PKEY_CTX_new_id(
                is_ed(algorithm)
                    ? (algorithm == Algorithm::Ed25519 ? EVP_PKEY_ED25519
                                                       : EVP_PKEY_ED448)
                    : (is_rsa(algorithm) ? EVP_PKEY_RSA : EVP_PKEY_EC),
                nullptr);
  const bool found = context != nullptr;
  EVP_PKEY_CTX_free(context);
  return found;
}

const EVP_MD *message_digest(Algorithm algorithm) {
  if (algorithm == Algorithm::EcdsaP256 || algorithm == Algorithm::RsaPssSha256)
    return EVP_sha256();
  if (algorithm == Algorithm::EcdsaP384 || algorithm == Algorithm::RsaPssSha384)
    return EVP_sha384();
  return nullptr; // Pure EdDSA and ML-DSA must use a null digest.
}

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

bool key_matches(EVP_PKEY *key, Algorithm algorithm) {
  if (key == nullptr)
    return false;
  if (is_ml_dsa(algorithm))
    return EVP_PKEY_is_a(key, ml_dsa_name(algorithm)) == 1;
  const int kind = EVP_PKEY_base_id(key);
  if (is_ed(algorithm))
    return kind == (algorithm == Algorithm::Ed25519 ? EVP_PKEY_ED25519
                                                    : EVP_PKEY_ED448);
  if (is_rsa(algorithm))
    return kind == EVP_PKEY_RSA && EVP_PKEY_get_bits(key) >= 2048;
  if (kind != EVP_PKEY_EC)
    return false;
  char group[80]{};
  std::size_t length = 0;
  if (EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group,
                                     sizeof(group), &length) != 1)
    return false;
  return algorithm == Algorithm::EcdsaP256
             ? std::strcmp(group, "prime256v1") == 0
             : std::strcmp(group, "secp384r1") == 0;
}

Key load_key(Algorithm algorithm, const std::string &bytes, bool private_key) {
  if (bytes.empty() || bytes.size() > 65536U || bytes.size() > LONG_MAX)
    return Key(nullptr, EVP_PKEY_free);
  EVP_PKEY *raw = nullptr;
  if (is_ed(algorithm)) {
    const int kind =
        algorithm == Algorithm::Ed25519 ? EVP_PKEY_ED25519 : EVP_PKEY_ED448;
    raw = private_key
              ? EVP_PKEY_new_raw_private_key(
                    kind, nullptr,
                    reinterpret_cast<const unsigned char *>(bytes.data()),
                    bytes.size())
              : EVP_PKEY_new_raw_public_key(
                    kind, nullptr,
                    reinterpret_cast<const unsigned char *>(bytes.data()),
                    bytes.size());
  } else {
    const auto *cursor = reinterpret_cast<const unsigned char *>(bytes.data());
    raw = private_key
              ? d2i_AutoPrivateKey(nullptr, &cursor,
                                   static_cast<long>(bytes.size()))
              : d2i_PUBKEY(nullptr, &cursor, static_cast<long>(bytes.size()));
    if (raw != nullptr && cursor != reinterpret_cast<const unsigned char *>(
                                        bytes.data() + bytes.size())) {
      EVP_PKEY_free(raw);
      raw = nullptr;
    }
  }
  if (!key_matches(raw, algorithm)) {
    EVP_PKEY_free(raw);
    raw = nullptr;
  }
  return Key(raw, EVP_PKEY_free);
}

bool encode_key(EVP_PKEY *key, Algorithm algorithm, bool private_key,
                std::string *out) {
  if (is_ed(algorithm)) {
    std::size_t length = 0;
    int ok = private_key ? EVP_PKEY_get_raw_private_key(key, nullptr, &length)
                         : EVP_PKEY_get_raw_public_key(key, nullptr, &length);
    if (ok != 1)
      return false;
    out->resize(length);
    ok = private_key
             ? EVP_PKEY_get_raw_private_key(
                   key, reinterpret_cast<unsigned char *>(&(*out)[0]), &length)
             : EVP_PKEY_get_raw_public_key(
                   key, reinterpret_cast<unsigned char *>(&(*out)[0]), &length);
    return ok == 1;
  }
  if (private_key) {
    std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)>
        info(EVP_PKEY2PKCS8(key), PKCS8_PRIV_KEY_INFO_free);
    if (!info)
      return false;
    const int length = i2d_PKCS8_PRIV_KEY_INFO(info.get(), nullptr);
    if (length <= 0)
      return false;
    out->resize(static_cast<std::size_t>(length));
    auto *cursor = reinterpret_cast<unsigned char *>(&(*out)[0]);
    return i2d_PKCS8_PRIV_KEY_INFO(info.get(), &cursor) == length;
  }
  const int length = i2d_PUBKEY(key, nullptr);
  if (length <= 0)
    return false;
  out->resize(static_cast<std::size_t>(length));
  auto *cursor = reinterpret_cast<unsigned char *>(&(*out)[0]);
  return i2d_PUBKEY(key, &cursor) == length;
}

bool openssl_generate(Algorithm algorithm, std::string *private_key,
                      std::string *public_key) {
  const int kind = is_ed(algorithm)
                       ? (algorithm == Algorithm::Ed25519 ? EVP_PKEY_ED25519
                                                          : EVP_PKEY_ED448)
                       : (is_rsa(algorithm) ? EVP_PKEY_RSA : EVP_PKEY_EC);
  KeyContext context(
      is_ml_dsa(algorithm)
          ? EVP_PKEY_CTX_new_from_name(nullptr, ml_dsa_name(algorithm), nullptr)
          : EVP_PKEY_CTX_new_id(kind, nullptr),
      EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1)
    return false;
  if (is_rsa(algorithm) &&
      EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 3072) != 1)
    return false;
  if (!is_ed(algorithm) && !is_rsa(algorithm) && !is_ml_dsa(algorithm)) {
    const int curve = algorithm == Algorithm::EcdsaP256 ? NID_X9_62_prime256v1
                                                        : NID_secp384r1;
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), curve) != 1)
      return false;
  }
  EVP_PKEY *raw = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw) != 1)
    return false;
  Key key(raw, EVP_PKEY_free);
  return encode_key(key.get(), algorithm, true, private_key) &&
         encode_key(key.get(), algorithm, false, public_key);
}

bool openssl_sign(Algorithm algorithm, EVP_PKEY *key,
                  const std::string &message, std::string *signature) {
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  EVP_PKEY_CTX *key_context = nullptr;
  if (!context ||
      EVP_DigestSignInit(context.get(), &key_context, message_digest(algorithm),
                         nullptr, key) != 1)
    return false;
  if (is_rsa(algorithm) &&
      (EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) != 1 ||
       EVP_PKEY_CTX_set_rsa_mgf1_md(key_context, message_digest(algorithm)) !=
           1 ||
       EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_DIGEST) !=
           1))
    return false;
  std::size_t length = 0;
  const auto *data = reinterpret_cast<const unsigned char *>(message.data());
  if (EVP_DigestSign(context.get(), nullptr, &length, data, message.size()) !=
      1)
    return false;
  signature->resize(length);
  if (EVP_DigestSign(context.get(),
                     reinterpret_cast<unsigned char *>(&(*signature)[0]),
                     &length, data, message.size()) != 1)
    return false;
  signature->resize(length);
  return true;
}

bool openssl_verify(Algorithm algorithm, EVP_PKEY *key,
                    const std::string &message, const std::string &signature) {
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  EVP_PKEY_CTX *key_context = nullptr;
  if (!context ||
      EVP_DigestVerifyInit(context.get(), &key_context,
                           message_digest(algorithm), nullptr, key) != 1)
    return false;
  if (is_rsa(algorithm) &&
      (EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) != 1 ||
       EVP_PKEY_CTX_set_rsa_mgf1_md(key_context, message_digest(algorithm)) !=
           1 ||
       EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_DIGEST) !=
           1))
    return false;
  return EVP_DigestVerify(
             context.get(),
             reinterpret_cast<const unsigned char *>(signature.data()),
             signature.size(),
             reinterpret_cast<const unsigned char *>(message.data()),
             message.size()) == 1;
}

#ifdef SPUTNIK_HAVE_NETTLE_GOST
const ecc_curve *gost_curve(Algorithm algorithm) {
  return algorithm == Algorithm::Gost2012_256 ? nettle_get_gost_gc256b()
                                              : nettle_get_gost_gc512a();
}

std::size_t gost_width(Algorithm algorithm) {
  return algorithm == Algorithm::Gost2012_256 ? 32U : 64U;
}

std::string gost_digest(Algorithm algorithm, const std::string &message) {
  return algorithm == Algorithm::Gost2012_256 ? digest_streebog256(message)
                                              : digest_streebog512(message);
}

struct Scalar {
  explicit Scalar(const ecc_curve *curve) { ecc_scalar_init(&value, curve); }
  ~Scalar() { ecc_scalar_clear(&value); }
  ecc_scalar value;
};
struct Point {
  explicit Point(const ecc_curve *curve) { ecc_point_init(&value, curve); }
  ~Point() { ecc_point_clear(&value); }
  ecc_point value;
};
struct BigInt {
  BigInt() { mpz_init(value); }
  ~BigInt() { mpz_clear(value); }
  mpz_t value;
};
struct GostSignature {
  GostSignature() { dsa_signature_init(&value); }
  ~GostSignature() { dsa_signature_clear(&value); }
  dsa_signature value;
};

void import_integer(mpz_t out, const char *bytes, std::size_t length,
                    int order = 1) {
  mpz_import(out, length, order, 1, 1, 0, bytes);
}

bool export_integer(const mpz_t value, std::size_t width, std::string *out) {
  if (mpz_sgn(value) < 0 || mpz_sizeinbase(value, 2) > width * 8U)
    return false;
  out->assign(width, '\0');
  std::size_t length = 0;
  std::string buffer(width, '\0');
  mpz_export(&buffer[0], &length, 1, 1, 1, 0, value);
  std::copy_n(buffer.data(), length, out->begin() + (width - length));
  return true;
}

// Nettle <= 4.0 does not normalize e=0 to 1 before GOST signing. That can
// disclose the private key. Reject both byte-order interpretations of any
// digest equivalent to zero modulo q before calling the backend.
bool gost_digest_safe(Algorithm algorithm, const std::string &digest) {
  constexpr const char *q256 =
      "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF6C611070995AD10045841B09B761B893";
  constexpr const char *q512 =
      "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF27E6"
      "9532F48D89116FF22B8D4E0560609B4B38ABFAD2B85DCACDB1411F10B275";
  BigInt q, e;
  if (mpz_set_str(q.value, algorithm == Algorithm::Gost2012_256 ? q256 : q512,
                  16) != 0)
    return false;
  for (int order : {1, -1}) {
    import_integer(e.value, digest.data(), digest.size(), order);
    if (mpz_divisible_p(e.value, q.value))
      return false;
  }
  return true;
}

struct RandomContext {
  const SignatureEntropy &entropy;
  bool failed = false;
};

void gost_random(void *context, std::size_t length, std::uint8_t *out) {
  auto &state = *static_cast<RandomContext *>(context);
  std::string bytes;
  if (!state.entropy(length, &bytes) || bytes.size() != length) {
    state.failed = true;
    std::fill_n(out, length, static_cast<std::uint8_t>(1));
    return;
  }
  std::memcpy(out, bytes.data(), length);
}

bool gost_load_private(Algorithm algorithm, const std::string &bytes,
                       Scalar *private_key) {
  if (bytes.size() != gost_width(algorithm))
    return false;
  BigInt n;
  import_integer(n.value, bytes.data(), bytes.size());
  return ecc_scalar_set(&private_key->value, n.value) != 0;
}

bool gost_public(Algorithm algorithm, const std::string &private_bytes,
                 std::string *public_bytes) {
  Scalar secret(gost_curve(algorithm));
  if (!gost_load_private(algorithm, private_bytes, &secret))
    return false;
  Point public_key(gost_curve(algorithm));
  ecc_point_mul_g(&public_key.value, &secret.value);
  BigInt x, y;
  ecc_point_get(&public_key.value, x.value, y.value);
  std::string x_bytes, y_bytes;
  if (!export_integer(x.value, gost_width(algorithm), &x_bytes) ||
      !export_integer(y.value, gost_width(algorithm), &y_bytes))
    return false;
  *public_bytes = x_bytes + y_bytes;
  return true;
}

bool gost_generate(const SignatureEntropy &entropy, Algorithm algorithm,
                   std::string *private_key, std::string *public_key) {
  Scalar secret(gost_curve(algorithm));
  for (int attempt = 0; attempt < 128; ++attempt) {
    std::string candidate;
    if (!entropy(gost_width(algorithm), &candidate))
      return false;
    if (gost_load_private(algorithm, candidate, &secret)) {
      *private_key = std::move(candidate);
      return gost_public(algorithm, *private_key, public_key);
    }
  }
  return false;
}

bool gost_sign(const SignatureEntropy &entropy, Algorithm algorithm,
               const std::string &private_bytes, const std::string &message,
               std::string *signature) {
  Scalar secret(gost_curve(algorithm));
  if (!gost_load_private(algorithm, private_bytes, &secret))
    return false;
  const std::string digest = gost_digest(algorithm, message);
  if (!gost_digest_safe(algorithm, digest))
    return false;
  GostSignature result;
  RandomContext random{entropy};
  gostdsa_sign(&secret.value, &random, gost_random, digest.size(),
               reinterpret_cast<const std::uint8_t *>(digest.data()),
               &result.value);
  if (random.failed)
    return false;
  std::string r, s;
  if (!export_integer(result.value.r, gost_width(algorithm), &r) ||
      !export_integer(result.value.s, gost_width(algorithm), &s))
    return false;
  *signature = r + s;
  return true;
}

bool gost_load_public(Algorithm algorithm, const std::string &bytes,
                      Point *public_key) {
  const std::size_t width = gost_width(algorithm);
  if (bytes.size() != width * 2U)
    return false;
  BigInt x, y;
  import_integer(x.value, bytes.data(), width);
  import_integer(y.value, bytes.data() + width, width);
  return ecc_point_set(&public_key->value, x.value, y.value) != 0;
}

bool gost_verify(Algorithm algorithm, const std::string &public_bytes,
                 const std::string &message, const std::string &signature,
                 bool *valid) {
  Point public_key(gost_curve(algorithm));
  if (!gost_load_public(algorithm, public_bytes, &public_key))
    return false;
  const std::size_t width = gost_width(algorithm);
  *valid = false;
  if (signature.size() != 2U * width)
    return true;
  GostSignature sig;
  import_integer(sig.value.r, signature.data(), width);
  import_integer(sig.value.s, signature.data() + width, width);
  const std::string digest = gost_digest(algorithm, message);
  if (!gost_digest_safe(algorithm, digest))
    return true;
  *valid = gostdsa_verify(&public_key.value, digest.size(),
                          reinterpret_cast<const std::uint8_t *>(digest.data()),
                          &sig.value) != 0;
  return true;
}
#endif

} // namespace

bool signature_algorithm_available(std::string_view name) {
  const auto algorithm = parse_algorithm(std::string(name));
  if (!algorithm)
    return false;
#ifdef SPUTNIK_HAVE_NETTLE_GOST
  return is_gost(*algorithm) || openssl_available(*algorithm);
#else
  return !is_gost(*algorithm) && openssl_available(*algorithm);
#endif
}

SignatureResult signature_generate(std::string_view name,
                                   const SignatureEntropy &entropy,
                                   std::string *private_key,
                                   std::string *public_key) {
  const auto algorithm = parse_algorithm(std::string(name));
  if (!algorithm)
    return SignatureResult::UnknownAlgorithm;
  if (!signature_algorithm_available(name))
    return SignatureResult::Unavailable;
  if (private_key == nullptr || public_key == nullptr)
    return SignatureResult::OperationFailure;
  std::string probe;
  if (!entropy || !entropy(1U, &probe) || probe.size() != 1U)
    return SignatureResult::EntropyFailure;
#ifdef SPUTNIK_HAVE_NETTLE_GOST
  if (is_gost(*algorithm)) {
    return gost_generate(entropy, *algorithm, private_key, public_key)
               ? SignatureResult::Success
               : SignatureResult::OperationFailure;
  }
#endif
  return openssl_generate(*algorithm, private_key, public_key)
             ? SignatureResult::Success
             : SignatureResult::OperationFailure;
}

SignatureResult signature_public_key(std::string_view name,
                                     std::string_view private_key,
                                     std::string *public_key) {
  const auto algorithm = parse_algorithm(std::string(name));
  if (!algorithm)
    return SignatureResult::UnknownAlgorithm;
  if (!signature_algorithm_available(name))
    return SignatureResult::Unavailable;
  if (public_key == nullptr)
    return SignatureResult::OperationFailure;
  const std::string private_bytes(private_key);
#ifdef SPUTNIK_HAVE_NETTLE_GOST
  if (is_gost(*algorithm))
    return gost_public(*algorithm, private_bytes, public_key)
               ? SignatureResult::Success
               : SignatureResult::InvalidKey;
#endif
  Key key = load_key(*algorithm, private_bytes, true);
  if (!key)
    return SignatureResult::InvalidKey;
  return encode_key(key.get(), *algorithm, false, public_key)
             ? SignatureResult::Success
             : SignatureResult::OperationFailure;
}

SignatureResult signature_sign(std::string_view name,
                               std::string_view private_key,
                               std::string_view message,
                               const SignatureEntropy &entropy,
                               std::string *signature) {
  const auto algorithm = parse_algorithm(std::string(name));
  if (!algorithm)
    return SignatureResult::UnknownAlgorithm;
  if (!signature_algorithm_available(name))
    return SignatureResult::Unavailable;
  if (signature == nullptr)
    return SignatureResult::OperationFailure;
  const std::string private_bytes(private_key);
  const std::string message_bytes(message);
  if (!is_ed(*algorithm)) {
    std::string probe;
    if (!entropy || !entropy(1U, &probe) || probe.size() != 1U)
      return SignatureResult::EntropyFailure;
  }
#ifdef SPUTNIK_HAVE_NETTLE_GOST
  if (is_gost(*algorithm)) {
    Scalar key(gost_curve(*algorithm));
    if (!gost_load_private(*algorithm, private_bytes, &key))
      return SignatureResult::InvalidKey;
    return gost_sign(entropy, *algorithm, private_bytes, message_bytes,
                     signature)
               ? SignatureResult::Success
               : SignatureResult::OperationFailure;
  }
#endif
  Key key = load_key(*algorithm, private_bytes, true);
  if (!key)
    return SignatureResult::InvalidKey;
  return openssl_sign(*algorithm, key.get(), message_bytes, signature)
             ? SignatureResult::Success
             : SignatureResult::OperationFailure;
}

SignatureResult signature_verify(std::string_view name,
                                 std::string_view public_key,
                                 std::string_view message,
                                 std::string_view signature, bool *valid) {
  const auto algorithm = parse_algorithm(std::string(name));
  if (!algorithm)
    return SignatureResult::UnknownAlgorithm;
  if (!signature_algorithm_available(name))
    return SignatureResult::Unavailable;
  if (valid == nullptr)
    return SignatureResult::OperationFailure;
  const std::string public_bytes(public_key);
  const std::string message_bytes(message);
  const std::string signature_bytes(signature);
#ifdef SPUTNIK_HAVE_NETTLE_GOST
  if (is_gost(*algorithm))
    return gost_verify(*algorithm, public_bytes, message_bytes, signature_bytes,
                       valid)
               ? SignatureResult::Success
               : SignatureResult::InvalidKey;
#endif
  Key key = load_key(*algorithm, public_bytes, false);
  if (!key)
    return SignatureResult::InvalidKey;
  *valid =
      openssl_verify(*algorithm, key.get(), message_bytes, signature_bytes);
  return SignatureResult::Success;
}

namespace {

SendStatus signature_result_fault(NativeStdlibCall &call,
                                  SignatureResult result,
                                  const std::string &operation) {
  if (result == SignatureResult::EntropyFailure)
    return SendStatus::Faulted;
  if (result == SignatureResult::UnknownAlgorithm)
    return call.fault("ArgumentError", "unknown signature algorithm");
  if (result == SignatureResult::Unavailable)
    return call.fault("ArgumentError", "signature algorithm is unavailable");
  if (result == SignatureResult::InvalidKey)
    return call.fault("ArgumentError", "invalid signature key");
  return call.fault("SignatureError", operation + " failed");
}

SendStatus signature_dispatch(NativeStdlibCall &call) {
  if (call.kind != RuntimeNativeTypeKind::Signature)
    return SendStatus::NotHandled;
  const bool available = call.selector == "available?";
  const bool generate = call.selector == "generate";
  const bool public_key = call.selector == "public_key";
  const bool sign = call.selector == "sign";
  const bool verify = call.selector == "verify";
  if (!available && !generate && !public_key && !sign && !verify)
    return SendStatus::NotHandled;
  const std::size_t arity = verify ? 4U : sign ? 3U : public_key ? 2U : 1U;
  if (!call.require_no_block() || !call.reject_unknown_keywords({}) ||
      !call.require_arity(arity))
    return SendStatus::Faulted;
  if (!call.args[0].is_string() && !call.args[0].is_symbol())
    return call.fault("TypeError",
                      "signature algorithm must be a Str or Symbol");
  const auto name = call.text_of(call.args[0]);
  if (!name)
    return SendStatus::Faulted;
  if (available) {
    *call.out = Value::boolean(signature_algorithm_available(*name));
    return SendStatus::Matched;
  }
  std::optional<std::string> key_bytes;
  std::optional<std::string> message;
  std::optional<std::string> signature;
  if (!generate) {
    key_bytes = call.bytes_of(call.args[1]);
    if (!key_bytes)
      return SendStatus::Faulted;
  }
  if (sign || verify) {
    message = call.bytes_of(call.args[2]);
    if (!message)
      return SendStatus::Faulted;
  }
  if (verify) {
    signature = call.bytes_of(call.args[3]);
    if (!signature)
      return SendStatus::Faulted;
  }
  const SignatureEntropy entropy = [&call](std::size_t count,
                                           std::string *out) {
    return call.secure_random_bytes(count, out);
  };
  std::string private_result, public_result, signature_result;
  if (generate) {
    const SignatureResult result =
        signature_generate(*name, entropy, &private_result, &public_result);
    if (result != SignatureResult::Success)
      return signature_result_fault(call, result, "signature key generation");
    *call.out = call.make_object(
        {{"private_key", call.bytes_value(std::move(private_result))},
         {"public_key", call.bytes_value(std::move(public_result))}});
    return SendStatus::Matched;
  }
  if (public_key) {
    const SignatureResult result =
        signature_public_key(*name, *key_bytes, &public_result);
    if (result != SignatureResult::Success)
      return signature_result_fault(call, result, "public key export");
    *call.out = call.bytes_value(std::move(public_result));
    return SendStatus::Matched;
  }
  if (sign) {
    const SignatureResult result =
        signature_sign(*name, *key_bytes, *message, entropy, &signature_result);
    if (result != SignatureResult::Success)
      return signature_result_fault(call, result, "signature creation");
    *call.out = call.bytes_value(std::move(signature_result));
    return SendStatus::Matched;
  }
  bool valid = false;
  const SignatureResult result =
      signature_verify(*name, *key_bytes, *message, *signature, &valid);
  if (result != SignatureResult::Success)
    return signature_result_fault(call, result, "signature verification");
  *call.out = Value::boolean(valid);
  return SendStatus::Matched;
}

RuntimeNativeModuleDescriptor signature_module_descriptor() {
  return {{{"Signature", RuntimeNativeTypeKind::Signature}},
          {{RuntimeNativeTypeKind::Signature, &signature_dispatch}},
          {},
          {},
          {{"SignatureError", "Exception"}}};
}

} // namespace

void register_signature(NativeRegistry &registry) {
  register_native_module_descriptor(registry, signature_module_descriptor());
}

void register_signature_runtime_module(RuntimeModuleRegistry &modules,
                                       RuntimeDispatchRegistry &dispatch,
                                       RuntimeTypeRegistry &types,
                                       RuntimeErrorRegistry *errors) {
  const auto descriptor = signature_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
  if (errors != nullptr)
    register_runtime_error_descriptor(*errors, descriptor);
}

} // namespace sputnik::runtime
