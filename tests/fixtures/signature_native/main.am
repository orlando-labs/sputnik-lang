def main():
  secret = Hex.decode("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
  public = Hex.decode("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
  message = Bytes.new("")
  signature = Signature.sign(:ed25519, secret, message)
  ok = Signature.public_key(:ed25519, secret).hex() == public.hex()
  ok = ok and Signature.verify(:ed25519, public, message, signature)
  ok = ok and not Signature.verify(:ed25519, public, Bytes.new("altered"), signature)

  ed25519_keys = Signature.generate(:ed25519)
  ed25519_signature = Signature.sign(:ed25519, ed25519_keys["private_key"], message)
  ok = ok and Signature.public_key(:ed25519, ed25519_keys["private_key"]).hex() == ed25519_keys["public_key"].hex()
  ok = ok and Signature.verify(:ed25519, ed25519_keys["public_key"], message, ed25519_signature)
  ok = ok and not Signature.verify(:ed25519, ed25519_keys["public_key"], Bytes.new("altered"), ed25519_signature)

  ed448_keys = Signature.generate(:ed448)
  ed448_signature = Signature.sign(:ed448, ed448_keys["private_key"], message)
  ok = ok and Signature.public_key(:ed448, ed448_keys["private_key"]).hex() == ed448_keys["public_key"].hex()
  ok = ok and Signature.verify(:ed448, ed448_keys["public_key"], message, ed448_signature)
  ok = ok and not Signature.verify(:ed448, ed448_keys["public_key"], Bytes.new("altered"), ed448_signature)

  p256_keys = Signature.generate(:ecdsa_p256_sha256)
  p256_signature = Signature.sign(:ecdsa_p256_sha256, p256_keys["private_key"], message)
  ok = ok and Signature.public_key(:ecdsa_p256_sha256, p256_keys["private_key"]).hex() == p256_keys["public_key"].hex()
  ok = ok and Signature.verify(:ecdsa_p256_sha256, p256_keys["public_key"], message, p256_signature)
  ok = ok and not Signature.verify(:ecdsa_p256_sha256, p256_keys["public_key"], Bytes.new("altered"), p256_signature)

  p384_keys = Signature.generate(:ecdsa_p384_sha384)
  p384_signature = Signature.sign(:ecdsa_p384_sha384, p384_keys["private_key"], message)
  ok = ok and Signature.public_key(:ecdsa_p384_sha384, p384_keys["private_key"]).hex() == p384_keys["public_key"].hex()
  ok = ok and Signature.verify(:ecdsa_p384_sha384, p384_keys["public_key"], message, p384_signature)
  ok = ok and not Signature.verify(:ecdsa_p384_sha384, p384_keys["public_key"], Bytes.new("altered"), p384_signature)

  rsa256_keys = Signature.generate(:rsa_pss_sha256)
  rsa256_signature = Signature.sign(:rsa_pss_sha256, rsa256_keys["private_key"], message)
  ok = ok and Signature.public_key(:rsa_pss_sha256, rsa256_keys["private_key"]).hex() == rsa256_keys["public_key"].hex()
  ok = ok and Signature.verify(:rsa_pss_sha256, rsa256_keys["public_key"], message, rsa256_signature)
  ok = ok and not Signature.verify(:rsa_pss_sha256, rsa256_keys["public_key"], Bytes.new("altered"), rsa256_signature)

  rsa384_keys = Signature.generate(:rsa_pss_sha384)
  rsa384_signature = Signature.sign(:rsa_pss_sha384, rsa384_keys["private_key"], message)
  ok = ok and Signature.public_key(:rsa_pss_sha384, rsa384_keys["private_key"]).hex() == rsa384_keys["public_key"].hex()
  ok = ok and Signature.verify(:rsa_pss_sha384, rsa384_keys["public_key"], message, rsa384_signature)
  ok = ok and not Signature.verify(:rsa_pss_sha384, rsa384_keys["public_key"], Bytes.new("altered"), rsa384_signature)

  if Signature.available?(:ml_dsa_44):
    ml44_keys = Signature.generate(:ml_dsa_44)
    ml44_signature = Signature.sign(:ml_dsa_44, ml44_keys["private_key"], message)
    ok = ok and Signature.public_key(:ml_dsa_44, ml44_keys["private_key"]).hex() == ml44_keys["public_key"].hex()
    ok = ok and Signature.verify(:ml_dsa_44, ml44_keys["public_key"], message, ml44_signature)
    ok = ok and not Signature.verify(:ml_dsa_44, ml44_keys["public_key"], Bytes.new("altered"), ml44_signature)

    ml65_keys = Signature.generate(:ml_dsa_65)
    ml65_signature = Signature.sign(:ml_dsa_65, ml65_keys["private_key"], message)
    ok = ok and Signature.public_key(:ml_dsa_65, ml65_keys["private_key"]).hex() == ml65_keys["public_key"].hex()
    ok = ok and Signature.verify(:ml_dsa_65, ml65_keys["public_key"], message, ml65_signature)
    ok = ok and not Signature.verify(:ml_dsa_65, ml65_keys["public_key"], Bytes.new("altered"), ml65_signature)

    ml87_keys = Signature.generate(:ml_dsa_87)
    ml87_signature = Signature.sign(:ml_dsa_87, ml87_keys["private_key"], message)
    ok = ok and Signature.public_key(:ml_dsa_87, ml87_keys["private_key"]).hex() == ml87_keys["public_key"].hex()
    ok = ok and Signature.verify(:ml_dsa_87, ml87_keys["public_key"], message, ml87_signature)
    ok = ok and not Signature.verify(:ml_dsa_87, ml87_keys["public_key"], Bytes.new("altered"), ml87_signature)
  if Signature.available?(:gost2012_256):
    gost256_keys = Signature.generate(:gost2012_256)
    gost256_signature = Signature.sign(:gost2012_256, gost256_keys["private_key"], message)
    ok = ok and Signature.public_key(:gost2012_256, gost256_keys["private_key"]).hex() == gost256_keys["public_key"].hex()
    ok = ok and Signature.verify(:gost2012_256, gost256_keys["public_key"], message, gost256_signature)
    ok = ok and not Signature.verify(:gost2012_256, gost256_keys["public_key"], Bytes.new("altered"), gost256_signature)

    gost512_keys = Signature.generate(:gost2012_512)
    gost512_signature = Signature.sign(:gost2012_512, gost512_keys["private_key"], message)
    ok = ok and Signature.public_key(:gost2012_512, gost512_keys["private_key"]).hex() == gost512_keys["public_key"].hex()
    ok = ok and Signature.verify(:gost2012_512, gost512_keys["public_key"], message, gost512_signature)
    ok = ok and not Signature.verify(:gost2012_512, gost512_keys["public_key"], Bytes.new("altered"), gost512_signature)
  if ok:
    42
  else:
    0
