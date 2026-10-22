package sdk.pkg

export inspect

native def _inspect(file, required_profile) -> List from "sppkg_inspect" blocking

# Inspection verifies archive framing, inventories, metadata and binary headers.
# It does not compile, load or execute package code.
def inspect(file, required_profile: ""):
  facts = _inspect(file.raw, required_profile)
  manifest = Json.parse(facts[1], duplicate_keys: :reject)
  manifest[:kind] = facts[3]
  manifest[:limit_profile] = facts[4]
  manifest[:bundle] = facts[5]
  target = facts[6]
  manifest[:target] = {os: target[0], arch: target[1], min_os: target[2], libc: target[3], min_libc: target[4], compute_backend: target[5], cpu_features: target[6], compute_backends: target[7], cuda_version: target[8], min_cuda_driver: target[9], cuda_architectures: target[10]}
  manifest[:artifact_dependencies] = facts[7]
  manifest[:artifact_build_dependencies] = facts[8]
  manifest[:module_representations] = facts[9]
  manifest[:native_kinds] = facts[10]
  manifest[:native_payload] = facts[12]
  distribution = Json.parse(facts[0], duplicate_keys: :reject)
  manifest[:native_requirements] = distribution[:native_requirements]
  manifest[:native_providers] = distribution[:native_providers]
  build = Json.parse(facts[11], duplicate_keys: :reject)
  versions = []
  profiles = []
  bytecode_abis = []
  build[:bytecode].each |path, item|:
    versions.push!(item[:format]) unless versions.contains?(item[:format])
    profiles.push!(item[:profile_flags]) unless profiles.contains?(item[:profile_flags])
    bytecode_abis.push!(item[:abi_hash]) unless bytecode_abis.contains?(item[:abi_hash])
  native_abis = []
  build[:native].each |item|:
    native_abis.push!(item[:sputnik_ext_abi]) unless native_abis.contains?(item[:sputnik_ext_abi])
  compatibility = {bytecode_versions: versions, profiles: profiles, bytecode_abi_hashes: bytecode_abis, sputnik_ext_abi: if native_abis.empty? then null else native_abis[0], sputnik_ext_abis: native_abis, native_requirements: manifest[:native_requirements], native_providers: manifest[:native_providers]}
  {distribution: distribution, manifest: manifest, manifest_toml: facts[2], compatibility: compatibility}
