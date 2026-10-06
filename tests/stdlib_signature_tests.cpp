#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "profile/capabilities.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool okay, const std::string &message) {
  if (!okay) {
    std::cerr << "stdlib signature test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<signature-test>");
  auto tokens = lexer.lex();
  expect(tokens.ok(), amber::lexer::diagnostics_to_json(tokens.diagnostics));
  amber::parser::Parser parser(tokens.tokens);
  auto parsed = parser.parse_module_unit();
  expect(parsed.ok(), amber::lexer::diagnostics_to_json(parsed.diagnostics));
  auto bound = amber::binder::bind_module(parsed.items, parsed.module_name);
  expect(bound.ok(), amber::lexer::diagnostics_to_json(bound.diagnostics));
  auto hir =
      amber::hir::lower_module(parsed.items, parsed.module_name, bound.graph);
  auto emitted = amber::bytecode::emit_program(hir, parsed.module_name);
  expect(emitted.ok(), amber::lexer::diagnostics_to_json(emitted.diagnostics));
  auto decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(emitted.module));
  expect(decoded.ok(), amber::bytecode::verify_errors_to_json(decoded.errors));
  return std::move(decoded.module);
}

amber::runtime::ExecutionResult execute(const std::string &source) {
  auto module = compile(source);
  return amber::runtime::execute_code(module, module.init.entry_code_id);
}

void expect_true(const std::string &source, const std::string &name) {
  const auto result = execute(source);
  if (!result.ok() && result.fault)
    std::cerr << name << ": " << result.fault->error_name << " / "
              << result.fault->message << "\n";
  expect(result.ok(), name + " should succeed");
  expect(result.value.is_bool() && result.value.as_bool(),
         name + " should return true");
}

void test_ed25519_rfc8032_vector() {
  const std::string prefix =
      "secret = Hex.decode(\"9d61b19deffd5a60ba844af492ec2cc4"
      "4449c5697b326919703bac031cae7f60\")\n"
      "public = Hex.decode(\"d75a980182b10ab7d54bfed3c964073a"
      "0ee172f3daa62325af021a68f707511a\")\n"
      "expected = "
      "\"e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b\"\n"
      "sig = Signature.sign(:ed25519, secret, Bytes.new(\"\"))\n";
  expect_true(
      prefix + "Signature.public_key(:ed25519, secret).hex() == public.hex()\n",
      "RFC 8032 Ed25519 public key");
  expect_true(prefix + "sig.hex() == expected\n", "RFC 8032 Ed25519 signature");
  expect_true(prefix +
                  "Signature.verify(:ed25519, public, Bytes.new(\"\"), sig)\n",
              "RFC 8032 Ed25519 verification");
  expect_true(
      prefix +
          "not Signature.verify(:ed25519, public, Bytes.new(\"x\"), sig)\n",
      "RFC 8032 Ed25519 tampering");
}

void test_roundtrips() {
  const char *algorithms[] = {
      "ed25519",           "ed448",          "ecdsa_p256_sha256",
      "ecdsa_p384_sha384", "rsa_pss_sha256", "rsa_pss_sha384",
      "ml_dsa_44",         "ml_dsa_65",      "ml_dsa_87",
      "gost2012_256",      "gost2012_512"};
  for (const char *algorithm : algorithms) {
    const std::string name(algorithm);
    if (name.find("ml_dsa") == 0U) {
      const auto available = execute("Signature.available?(:" + name + ")\n");
      expect(available.ok() && available.value.is_bool(),
             name + " availability should be a Bool");
      if (!available.value.as_bool())
        continue;
    }
    if (name.find("gost") == 0U) {
#ifndef AMBER_HAVE_NETTLE_GOST
      expect_true("not Signature.available?(:" + name + ")\n",
                  name + " unavailable without Nettle");
      continue;
#endif
    }
    expect_true(
        "algorithm = :" + name +
            "\n"
            "keys = Signature.generate(algorithm)\n"
            "secret = keys[\"private_key\"]\n"
            "public = keys[\"public_key\"]\n"
            "message = Bytes.new(\"amber signature test\")\n"
            "sig = Signature.sign(algorithm, secret, message)\n"
            "Signature.available?(algorithm) and "
            "Signature.public_key(algorithm, secret).hex() == public.hex() and "
            "Signature.verify(algorithm, public, message, sig) and "
            "not Signature.verify(algorithm, public, Bytes.new(\"tampered\"), "
            "sig)\n",
        name + " roundtrip and tampering");
  }
}

void test_rejections() {
  expect_true("not Signature.available?(:unknown)\n", "unknown availability");
  const auto unknown = execute("Signature.generate(:unknown)\n");
  expect(!unknown.ok() && unknown.fault &&
             unknown.fault->error_name == "ArgumentError",
         "unknown algorithm rejected");
  const auto bad_key = execute(
      "Signature.sign(:ed25519, Bytes.new(\"short\"), Bytes.new(\"x\"))\n");
  expect(!bad_key.ok() && bad_key.fault &&
             bad_key.fault->error_name == "ArgumentError",
         "malformed private key rejected");
#ifdef AMBER_HAVE_NETTLE_GOST
  const auto bad_gost_256 =
      execute("Signature.public_key(:gost2012_256, Hex.decode(\""
              "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF6C611070995AD10045841B09B761B893"
              "\"))\n");
  expect(!bad_gost_256.ok() && bad_gost_256.fault &&
             bad_gost_256.fault->error_name == "ArgumentError",
         "GOST 256 curve order is not a private scalar");
  const auto bad_gost_512 =
      execute("Signature.public_key(:gost2012_512, Hex.decode(\""
              "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
              "27E69532F48D89116FF22B8D4E0560609B4B38ABFAD2B85DCACDB1411F10B275"
              "\"))\n");
  expect(!bad_gost_512.ok() && bad_gost_512.fault &&
             bad_gost_512.fault->error_name == "ArgumentError",
         "GOST 512 curve order is not a private scalar");
#endif
}

void test_random_capability() {
  auto module = compile("Signature.generate(:ed25519)\n");
  module.capabilities.push_back(
      amber::capability::make_capability("random.secure"));
  amber::runtime::RuntimeWorldOptions denied_options;
  amber::runtime::RuntimeWorld denied_world(module, denied_options);
  const auto denied = denied_world.execute(module.init.entry_code_id);
  expect(!denied.ok() && denied.fault &&
             denied.fault->error_name == "CapabilityError",
         "key generation requires random.secure capability");

  amber::runtime::RuntimeWorldOptions allowed_options;
  allowed_options.capability_grants.push_back(
      amber::capability::make_capability("random.secure"));
  amber::runtime::RuntimeWorld allowed_world(module, allowed_options);
  const auto allowed = allowed_world.execute(module.init.entry_code_id);
  expect(allowed.ok(), "key generation with random.secure grant succeeds");

  amber::runtime::RuntimeWorldOptions replay_options = allowed_options;
  replay_options.enforce_replay = true;
  amber::runtime::RuntimeWorld replay_world(module, replay_options);
  const auto replay = replay_world.execute(module.init.entry_code_id);
  expect(!replay.ok() && replay.fault &&
             replay.fault->error_name == "DeterminismError",
         "key generation rejects deterministic replay");

  auto pure_module =
      compile("secret = Hex.decode(\"9d61b19deffd5a60ba844af492ec2cc4"
              "4449c5697b326919703bac031cae7f60\")\n"
              "Signature.sign(:ed25519, secret, Bytes.new(\"message\"))\n");
  amber::runtime::RuntimeWorld pure_world(pure_module, replay_options);
  const auto pure = pure_world.execute(pure_module.init.entry_code_id);
  expect(pure.ok(), "pure Ed25519 signing needs no random capability");
}

} // namespace

int main() {
  test_ed25519_rfc8032_vector();
  test_roundtrips();
  test_rejections();
  test_random_capability();
  std::cout << "stdlib_signature_tests: ok\n";
}
