#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace amber::runtime {

// The signature backend is shared by VM stdlib dispatch and direct-native
// executables. Entropy callbacks must enforce their caller's capability and
// replay policy before returning OS-backed random bytes.
using SignatureEntropy =
    std::function<bool(std::size_t count, std::string *out)>;

enum class SignatureResult {
  Success,
  UnknownAlgorithm,
  Unavailable,
  InvalidKey,
  EntropyFailure,
  OperationFailure
};

bool signature_algorithm_available(std::string_view algorithm);

SignatureResult signature_generate(std::string_view algorithm,
                                   const SignatureEntropy &entropy,
                                   std::string *private_key,
                                   std::string *public_key);

SignatureResult signature_public_key(std::string_view algorithm,
                                     std::string_view private_key,
                                     std::string *public_key);

SignatureResult signature_sign(std::string_view algorithm,
                               std::string_view private_key,
                               std::string_view message,
                               const SignatureEntropy &entropy,
                               std::string *signature);

SignatureResult signature_verify(std::string_view algorithm,
                                 std::string_view public_key,
                                 std::string_view message,
                                 std::string_view signature, bool *valid);

} // namespace amber::runtime
