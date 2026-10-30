#include "package/selection.h"
#include <iostream>
#include <stdexcept>
using namespace sputnik::pkg;
void expect(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--rocm-host") {
    const auto host = probe_package_host(true);
    std::cout << "rocm=" << host.rocm << '\n';
    return host.rocm ? 0 : 1;
  }
  if (argc == 2 && std::string(argv[1]) == "--cuda-host") {
    const auto host = probe_package_host(true);
    std::cout << "driver=" << host.cuda_driver << " api=" << host.cuda_api_version << " visible=";
    for (const auto &arch : host.cuda_architectures) std::cout << arch << ',';
    std::cout << '\n';
    return host.cuda_architectures.empty() ? 1 : 0;
  }
  PackageHost h; h.os="linux"; h.arch="x86_64"; h.libc="gnu"; h.libc_version="2.28";
  h.cpu_features={"avx2"}; h.cuda_driver="580.105.08"; h.cuda_api_version="13.0"; h.cuda_architectures={"8.6"};
  PackageManifest cpu; cpu.variant="linux-cpu"; cpu.target.os="linux"; cpu.target.arch="x86_64";
  cpu.target.libc="gnu"; cpu.target.min_libc="2.28"; cpu.target.compute_backend="cpu";
  PackageNativeExtension n; n.kind="prebuilt"; cpu.native_extensions.push_back(n);
  auto cuda=cpu; cuda.variant="linux-cu128"; cuda.target.compute_backend="cuda";
  cuda.target.cuda_version="12.8"; cuda.target.min_cuda_driver="570.26"; cuda.target.cuda_architectures={"8.6","8.9"};
  auto cuda_old=cuda; cuda_old.variant="linux-cu126"; cuda_old.target.cuda_version="12.6"; cuda_old.target.min_cuda_driver="560.28.03";
  VariantPolicy p;
  expect(select_package_variant({cpu,cuda},h,p)==-1,"native trust required");
  p.allow_native=true;
  expect(select_package_variant({cpu,cuda},h,p)==0,"CPU remains default");
  auto rocm=cpu; rocm.variant="linux-rocm643"; rocm.target.compute_backend="rocm";
  expect(select_package_variant({rocm},h,p)==-1,"ROCm never substitutes for a CPU install");
  p.backend="rocm";
  expect(select_package_variant({rocm},h,p)==-1,"unavailable HIP runtime/GPU rejected");
  auto amd=h; amd.cuda_architectures.clear(); amd.rocm=true;
  expect(select_package_variant({cpu,cuda,rocm},amd,p)==2,"ROCm backend selects AMD variant");
  p.variant=rocm.variant;
  expect(select_package_variant({rocm},h,p)==-1,"explicit ROCm variant retains GPU checks");
  expect(select_package_variant({rocm},amd,p)==0,"explicit ROCm variant on AMD host");
  auto wrong_os=rocm; wrong_os.target.os="macos";
  bool invalid_rocm=false;
  try { validate_package_target(wrong_os.target); } catch(const std::runtime_error &) { invalid_rocm=true; }
  expect(invalid_rocm,"ROCm metadata requires Linux");
  p.variant.clear();
  p.backend="cuda";
  expect(select_package_variant({cpu,cuda_old,cuda},h,p)==2,"newest compatible CUDA runtime");
  auto old=h; old.cuda_driver="565.0";
  expect(select_package_variant({cuda,cuda_old},old,p)==1,"older driver chooses compatible CUDA build");
  p.variant="linux-cu128";
  expect(select_package_variant({cuda},old,p)==-1,"explicit variant keeps driver checks");
  auto foreign=h; foreign.arch="aarch64";
  expect(select_package_variant({cuda},foreign,p)==-1,"explicit variant keeps architecture checks");
  auto hidden=h; hidden.cuda_architectures.clear();
  expect(select_package_variant({cuda},hidden,p)==-1,"hidden/unavailable GPU rejected");
  auto old_api=h; old_api.cuda_api_version="12.0";
  expect(select_package_variant({cuda},old_api,p)==-1,"driver API must support bundled CUDA runtime");
  auto pascal=h; pascal.cuda_architectures={"6.1"};
  expect(select_package_variant({cuda},pascal,p)==-1,"unsupported compute capability rejected");
  p.variant.clear();
  auto equal=cuda; equal.variant="also-cu128";
  bool ambiguous=false; try { select_package_variant({cuda,equal},h,p); } catch(const std::runtime_error &e) { ambiguous=std::string(e.what()).find("AMBIGUOUS")!=std::string::npos; }
  expect(ambiguous,"equivalent selectors require explicit variant");
  auto source=cuda; source.native_extensions[0].kind="source";
  expect(select_package_variant({source},h,p)==-1,"no hidden compilation fallback");
  p.build_from_source=true;
  expect(select_package_variant({source},h,p)==0,"explicit source compilation");
  PackageManifest pure; pure.variant="source";
  expect(select_package_variant({pure},h,p)==0,"source builds retain pure package dependencies");
  p.build_from_source=false;
  auto abi=cuda; abi.native_extensions[0].sputnik_ext_abi_version=999;
  expect(select_package_variant({abi},h,p)==-1,"extension ABI checked");
  auto modern=cuda; modern.target.min_libc="2.35";
  expect(select_package_variant({modern},h,p)==-1,"minimum glibc checked");
  auto feature=cuda; feature.target.cpu_features={"avx512f"};
  expect(select_package_variant({feature},h,p)==-1,"CPU instructions checked");
  PackageManifest runtime; runtime.kind="native"; runtime.variant="libtorch";
  PackageNativeProvider provider; provider.id="libtorch"; provider.kind="runtime"; provider.version="2.11.0";
  provider.abi="libtorch-2.14.0-cxx11-cuda13"; provider.build_id="sha256:"+std::string(64,'a');
  runtime.native_providers.push_back(provider);
  PackageNativeRequirement required; required.id=provider.id; required.kind=provider.kind; required.version="2.14.0";
  required.abi=provider.abi; required.build_id=provider.build_id;
  p.backend="cpu"; p.native_requirements={required};
  expect(select_package_variant({runtime},h,p)==-1,"hard LibTorch version cannot be downgraded");
  runtime.native_providers[0].version="2.14.0";
  expect(select_package_variant({runtime},h,p)==0,"exact technical runtime selected");
  runtime.native_providers[0].abi="other-cxx-abi";
  expect(select_package_variant({runtime},h,p)==-1,"same version with wrong C++ ABI rejected");
  runtime.native_providers[0].abi=required.abi; runtime.native_providers[0].build_id="sha256:"+std::string(64,'b');
  expect(select_package_variant({runtime},h,p)==-1,"same version with different producer build rejected");
  p.native_requirements.clear();
  PackageManifest text; text.variant="source";
  PackageManifest mixed=text; mixed.variant="mixed"; mixed.has_bytecode_delivery=true;
  PackageManifest bc=mixed; bc.variant="bytecode";
  expect(select_package_variant({mixed,bc,text},h,p)==2,"source wins over mixed and bytecode module delivery");
  p.variant="mixed";
  expect(select_package_variant({mixed,bc,text},h,p)==0,"explicit mixed selection retained");
  std::cout<<"package_selection_tests: PASS\n";
}
