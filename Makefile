.DEFAULT_GOAL := all
include mk/config.mk

LEXER_SRCS := frontend/lexer/lexer.cpp frontend/lexer/token.cpp
AST_SRCS := frontend/ast/expr.cpp
PARSER_SRCS := frontend/parser/parser.cpp
PATTERN_SRCS := frontend/pattern/pattern.cpp
BINDER_SRCS := frontend/binder/binder.cpp
CHECKER_SRCS := frontend/checker/checker.cpp
HIR_SRCS := frontend/hir/hir.cpp
MIR_SRCS := optimizer/mir.cpp
NATIVE_SRCS := optimizer/native.cpp
FROZEN_SRCS := frozen/image.cpp
PROFILE_SRCS := profile/capabilities.cpp profile/effects.cpp profile/replay.cpp profile/data.cpp profile/wasm_accel.cpp profile/modern.cpp
BUILD_SRCS := buildsys/build.cpp
BYTECODE_SRCS := bytecode/format.cpp bytecode/emitter.cpp bytecode/graph_linker.cpp
IO_SRCS := runtime/io.cpp runtime/reactor.cpp runtime/tls.cpp
DIGEST_SRCS := runtime/digest.cpp
HTTP_SRCS := runtime/http_codec.cpp runtime/net_http.cpp runtime/net_http_server.cpp runtime/net_http_transport.cpp
STDLIB_SRCS := runtime/stdlib_registry.cpp runtime/stdlib_bool.cpp runtime/stdlib_io.cpp runtime/stdlib_fs.cpp runtime/stdlib_net.cpp runtime/stdlib_net_http.cpp runtime/stdlib_task.cpp runtime/stdlib_math.cpp runtime/stdlib_json.cpp runtime/stdlib_codecs.cpp runtime/stdlib_digest.cpp runtime/stdlib_signature.cpp runtime/stdlib_benchmark.cpp runtime/stdlib_secure_random.cpp runtime/stdlib_argparser.cpp runtime/stdlib_regexp.cpp runtime/stdlib_uuid.cpp runtime/stdlib_time.cpp runtime/stdlib_url.cpp runtime/stdlib_yaml.cpp
RUNTIME_SRCS := runtime/context.cpp runtime/text.cpp runtime/watch.cpp runtime/value.cpp runtime/value_display.cpp runtime/errors.cpp runtime/numeric.cpp runtime/objects.cpp runtime/heap.cpp runtime/concurrency.cpp runtime/world.cpp $(IO_SRCS) $(DIGEST_SRCS) $(HTTP_SRCS) runtime/system.cpp runtime/vm.cpp $(STDLIB_SRCS) runtime/sputnik_ext.cpp runtime/module_loader.cpp runtime/native_bridge.cpp runtime/macro_expander.cpp
FROZEN_RUNTIME_SRCS := runtime/frozen_image.cpp
PACKAGE_SRCS := package/package.cpp
FRONTEND_SRCS := $(LEXER_SRCS) $(AST_SRCS) $(PARSER_SRCS) $(PATTERN_SRCS) $(BINDER_SRCS) $(CHECKER_SRCS) $(HIR_SRCS)
CORE_SRCS := $(PROFILE_SRCS) $(BUILD_SRCS) $(FRONTEND_SRCS) $(MIR_SRCS) $(NATIVE_SRCS) $(BYTECODE_SRCS)
SPUTNIK_SRCS := tools/sputnik/main.cpp $(CORE_SRCS) $(RUNTIME_SRCS) $(PACKAGE_SRCS) $(FROZEN_SRCS)
SPUTNIKTEST_SRCS := tools/sputniktest/main.cpp $(CORE_SRCS) $(RUNTIME_SRCS) $(PACKAGE_SRCS)

LIB_SRCS := $(sort $(CORE_SRCS) $(RUNTIME_SRCS) $(PACKAGE_SRCS) $(FROZEN_SRCS) $(FROZEN_RUNTIME_SRCS))
LIB_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/obj/%.o,$(LIB_SRCS))
LIBRARY := $(BUILD_DIR)/libsputnik.a
runtime: $(LIBRARY)
$(BUILD_DIR)/obj/%.o: %.cpp
	@mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(LIBRARY): $(LIB_OBJS)
	$(AR) rcs $@ $^
-include $(LIB_OBJS:.o=.d) $(wildcard $(BUILD_DIR)/obj/tools/*/*.d) $(wildcard $(BUILD_DIR)/obj/tests/*.d)
LEXER_TEST_SRCS := tests/lexer_tests.cpp $(LEXER_SRCS)
PARSER_TEST_SRCS := tests/parser_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
BINDER_TEST_SRCS := tests/binder_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
CHECKER_TEST_SRCS := tests/checker_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
WASM_ACCEL_TEST_SRCS := tests/wasm_accel_tests.cpp $(PROFILE_SRCS) $(LEXER_SRCS)
MODERN_PROFILE_TEST_SRCS := tests/modern_profile_tests.cpp $(PROFILE_SRCS) $(LEXER_SRCS)
BUILD_TEST_SRCS := tests/build_tests.cpp $(BUILD_SRCS)
HIR_TEST_SRCS := tests/hir_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS)
MIR_TEST_SRCS := tests/mir_tests.cpp $(PROFILE_SRCS) $(FRONTEND_SRCS) $(MIR_SRCS)
NATIVE_TEST_SRCS := tests/native_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
FROZEN_IMAGE_TEST_SRCS := tests/frozen_image_tests.cpp $(CORE_SRCS) $(PACKAGE_SRCS) $(FROZEN_SRCS) $(RUNTIME_SRCS) $(FROZEN_RUNTIME_SRCS)
BYTECODE_TEST_SRCS := tests/bytecode_tests.cpp $(PROFILE_SRCS) $(BYTECODE_SRCS) $(LEXER_SRCS) $(AST_SRCS)
GRAPH_LINKER_TEST_SRCS := tests/graph_linker_tests.cpp bytecode/graph_linker.cpp bytecode/format.cpp $(PROFILE_SRCS) $(LEXER_SRCS)
EMITTER_TEST_SRCS := tests/emitter_tests.cpp $(CORE_SRCS)
VM_TEST_SRCS := tests/vm_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_COLLECTIONS_TEST_SRCS := tests/stdlib_collections_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_TASK_TEST_SRCS := tests/stdlib_task_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_REGISTRY_TEST_SRCS := tests/stdlib_registry_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_JSON_TEST_SRCS := tests/stdlib_json_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_CODECS_TEST_SRCS := tests/stdlib_codecs_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_DIGEST_TEST_SRCS := tests/stdlib_digest_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_SIGNATURE_TEST_SRCS := tests/stdlib_signature_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_BENCHMARK_TEST_SRCS := tests/stdlib_benchmark_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_SECURE_RANDOM_TEST_SRCS := tests/stdlib_secure_random_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_BOOL_TEST_SRCS := tests/stdlib_bool_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_ARGPARSER_TEST_SRCS := tests/stdlib_argparser_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_REGEXP_TEST_SRCS := tests/stdlib_regexp_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_UUID_TEST_SRCS := tests/stdlib_uuid_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_TIME_TEST_SRCS := tests/stdlib_time_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_URL_TEST_SRCS := tests/stdlib_url_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
STDLIB_YAML_TEST_SRCS := tests/stdlib_yaml_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
SPUTNIK_EXT_TEST_SRCS := tests/sputnik_ext_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
IO_TEST_SRCS := tests/io_tests.cpp runtime/context.cpp runtime/text.cpp $(IO_SRCS)
HTTP_CODEC_TEST_SRCS := tests/http_codec_tests.cpp runtime/http_codec.cpp
NET_HTTP_TEST_SRCS := tests/net_http_tests.cpp runtime/net_http.cpp runtime/http_codec.cpp
NET_HTTP_TCP_TEST_SRCS := tests/net_http_tcp_tests.cpp runtime/net_http_transport.cpp runtime/net_http.cpp runtime/http_codec.cpp runtime/context.cpp runtime/text.cpp $(IO_SRCS)
VM_NET_HTTP_TEST_SRCS := tests/vm_net_http_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
MODULE_LOADER_TEST_SRCS := tests/module_loader_tests.cpp $(CORE_SRCS) $(RUNTIME_SRCS)
PACKAGE_TEST_SRCS := tests/package_tests.cpp $(PROFILE_SRCS) $(PACKAGE_SRCS) $(LEXER_SRCS)

FORMAT_FILES := \
	frontend/lexer/lexer.cpp \
	frontend/lexer/lexer.h \
	frontend/lexer/token.cpp \
	frontend/lexer/token.h \
	frontend/ast/expr.cpp \
	frontend/ast/expr.h \
	frontend/parser/parser.cpp \
	frontend/parser/parser.h \
	frontend/pattern/pattern.cpp \
	frontend/pattern/pattern.h \
	frontend/binder/binder.cpp \
	frontend/binder/binder.h \
	frontend/checker/checker.cpp \
	frontend/checker/checker.h \
	frontend/hir/hir.cpp \
	frontend/hir/hir.h \
	optimizer/mir.cpp \
	optimizer/mir.h \
	optimizer/native.cpp \
	optimizer/native.h \
	profile/capabilities.cpp \
	profile/capabilities.h \
	profile/effects.cpp \
	profile/effects.h \
	profile/replay.cpp \
	profile/replay.h \
	profile/data.cpp \
	profile/data.h \
	profile/wasm_accel.cpp \
	profile/wasm_accel.h \
	profile/modern.cpp \
	profile/modern.h \
	buildsys/build.cpp \
	buildsys/build.h \
	frozen/image.cpp \
	frozen/image.h \
	bytecode/format.cpp \
	bytecode/format.h \
	bytecode/emitter.cpp \
	bytecode/emitter.h \
	runtime/module_loader.cpp \
	runtime/module_loader.h \
	runtime/native_bridge.cpp \
	runtime/native_bridge.h \
	runtime/context.cpp \
	runtime/context.h \
	runtime/io.cpp \
	runtime/io.h \
	runtime/reactor.cpp \
	runtime/reactor.h \
	runtime/http_codec.cpp \
	runtime/http_codec.h \
	runtime/net_http.cpp \
	runtime/net_http.h \
	runtime/net_http_transport.cpp \
	runtime/net_http_transport.h \
	runtime/tls.cpp \
	runtime/tls.h \
	runtime/net_http_client.h \
	runtime/net_http_client_native.inc \
	runtime/digest.cpp \
	runtime/digest.h \
	runtime/signature.h \
	runtime/text.cpp \
	runtime/value.cpp \
	runtime/value.h \
	runtime/value_display.cpp \
	runtime/value_display.h \
	runtime/errors.cpp \
	runtime/errors.h \
	runtime/numeric.cpp \
	runtime/numeric.h \
	runtime/objects.cpp \
	runtime/objects.h \
	runtime/heap.cpp \
	runtime/heap.h \
	runtime/concurrency.cpp \
	runtime/concurrency.h \
	runtime/world.cpp \
	runtime/world.h \
	runtime/frozen_image.cpp \
	runtime/frozen_image.h \
	runtime/vm.cpp \
	runtime/vm.h \
	runtime/stdlib_registry.cpp \
	runtime/stdlib_registry.h \
	runtime/stdlib_io.cpp \
	runtime/stdlib_fs.cpp \
	runtime/stdlib_net.cpp \
	runtime/stdlib_net_http.cpp \
	runtime/stdlib_task.cpp \
	runtime/stdlib_math.cpp \
	runtime/stdlib_json.cpp \
	runtime/stdlib_codecs.cpp \
	runtime/stdlib_digest.cpp \
	runtime/stdlib_signature.cpp \
	runtime/stdlib_benchmark.cpp \
	runtime/stdlib_secure_random.cpp \
	runtime/stdlib_bool.cpp \
	runtime/stdlib_bool.h \
	runtime/stdlib_argparser.cpp \
	runtime/stdlib_regexp.h \
	runtime/stdlib_regexp.cpp \
	runtime/stdlib_uuid.h \
	runtime/stdlib_uuid.cpp \
	runtime/stdlib_time.cpp \
	runtime/stdlib_url.h \
	runtime/stdlib_url.cpp \
	runtime/stdlib_yaml.cpp \
	runtime/sputnik_ext.h \
	runtime/sputnik_ext_runtime.h \
	runtime/sputnik_ext.cpp \
	package/package.cpp \
	package/package.h \
	tools/sputnik/main.cpp \
	tools/sputniktest/main.cpp \
	tests/lexer_tests.cpp \
	tests/parser_tests.cpp \
	tests/binder_tests.cpp \
	tests/checker_tests.cpp \
	tests/wasm_accel_tests.cpp \
	tests/modern_profile_tests.cpp \
	tests/build_tests.cpp \
	tests/hir_tests.cpp \
	tests/mir_tests.cpp \
	tests/native_tests.cpp \
	tests/frozen_image_tests.cpp \
	tests/bytecode_tests.cpp \
	tests/emitter_tests.cpp \
	tests/module_loader_tests.cpp \
	tests/package_tests.cpp \
	tests/vm_tests.cpp \
	tests/stdlib_collections_tests.cpp \
	tests/stdlib_task_tests.cpp \
	tests/stdlib_registry_tests.cpp \
	tests/stdlib_json_tests.cpp \
	tests/stdlib_codecs_tests.cpp \
	tests/stdlib_digest_tests.cpp \
	tests/stdlib_signature_tests.cpp \
	tests/stdlib_benchmark_tests.cpp \
	tests/stdlib_secure_random_tests.cpp \
	tests/stdlib_bool_tests.cpp \
	tests/stdlib_argparser_tests.cpp \
	tests/stdlib_regexp_tests.cpp \
	tests/stdlib_uuid_tests.cpp \
	tests/stdlib_time_tests.cpp \
	tests/stdlib_yaml_tests.cpp \
	tests/io_tests.cpp \
	tests/http_codec_tests.cpp \
	tests/net_http_tests.cpp \
	tests/net_http_tcp_tests.cpp

.PHONY: all build test conformance backend-equivalence spec-sync-check fmt clean


all: build

build: $(BUILD_DIR)/sputnik

.PHONY: build-tests runtime
build-tests: build $(BUILD_DIR)/sputniktest $(BUILD_DIR)/system_tests $(BUILD_DIR)/stdlib_system_tests $(BUILD_DIR)/lexer_tests $(BUILD_DIR)/parser_tests $(BUILD_DIR)/binder_tests $(BUILD_DIR)/checker_tests $(BUILD_DIR)/wasm_accel_tests $(BUILD_DIR)/modern_profile_tests $(BUILD_DIR)/build_tests $(BUILD_DIR)/hir_tests $(BUILD_DIR)/mir_tests $(BUILD_DIR)/native_tests $(BUILD_DIR)/frozen_image_tests $(BUILD_DIR)/bytecode_tests $(BUILD_DIR)/emitter_tests $(BUILD_DIR)/vm_tests $(BUILD_DIR)/stdlib_collections_tests $(BUILD_DIR)/stdlib_task_tests $(BUILD_DIR)/stdlib_registry_tests $(BUILD_DIR)/stdlib_json_tests $(BUILD_DIR)/stdlib_codecs_tests $(BUILD_DIR)/stdlib_digest_tests $(BUILD_DIR)/stdlib_signature_tests $(BUILD_DIR)/stdlib_benchmark_tests $(BUILD_DIR)/stdlib_secure_random_tests $(BUILD_DIR)/stdlib_bool_tests $(BUILD_DIR)/stdlib_argparser_tests $(BUILD_DIR)/stdlib_regexp_tests $(BUILD_DIR)/stdlib_uuid_tests $(BUILD_DIR)/stdlib_time_tests $(BUILD_DIR)/stdlib_url_tests $(BUILD_DIR)/stdlib_yaml_tests $(BUILD_DIR)/sputnik_ext_tests $(BUILD_DIR)/io_tests $(BUILD_DIR)/http_codec_tests $(BUILD_DIR)/net_http_tests $(BUILD_DIR)/net_http_tcp_tests $(BUILD_DIR)/module_loader_tests $(BUILD_DIR)/package_tests $(BUILD_DIR)/graph_linker_tests


$(BUILD_DIR)/system_tests: $(BUILD_DIR)/obj/tests/system_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_system_tests: $(BUILD_DIR)/obj/tests/stdlib_system_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@



# These adapters are included from the VM rather than compiled separately.
# Keep all runtime consumers current when their implementation changes.

$(BUILD_DIR)/.dir:
	mkdir -p $(BUILD_DIR)
	touch $(BUILD_DIR)/.dir


$(BUILD_DIR)/sputnik: $(BUILD_DIR)/obj/tools/sputnik/main.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/sputniktest: $(BUILD_DIR)/obj/tools/sputniktest/main.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/lexer_tests: $(BUILD_DIR)/obj/tests/lexer_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/parser_tests: $(BUILD_DIR)/obj/tests/parser_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/binder_tests: $(BUILD_DIR)/obj/tests/binder_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/checker_tests: $(BUILD_DIR)/obj/tests/checker_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/wasm_accel_tests: $(BUILD_DIR)/obj/tests/wasm_accel_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/modern_profile_tests: $(BUILD_DIR)/obj/tests/modern_profile_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/build_tests: $(BUILD_DIR)/obj/tests/build_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/hir_tests: $(BUILD_DIR)/obj/tests/hir_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/mir_tests: $(BUILD_DIR)/obj/tests/mir_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/native_tests: $(BUILD_DIR)/obj/tests/native_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/frozen_image_tests: $(BUILD_DIR)/obj/tests/frozen_image_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/bytecode_tests: $(BUILD_DIR)/obj/tests/bytecode_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/graph_linker_tests: $(BUILD_DIR)/obj/tests/graph_linker_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/emitter_tests: $(BUILD_DIR)/obj/tests/emitter_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/vm_tests: $(BUILD_DIR)/obj/tests/vm_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_collections_tests: $(BUILD_DIR)/obj/tests/stdlib_collections_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_task_tests: $(BUILD_DIR)/obj/tests/stdlib_task_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_registry_tests: $(BUILD_DIR)/obj/tests/stdlib_registry_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_json_tests: $(BUILD_DIR)/obj/tests/stdlib_json_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_codecs_tests: $(BUILD_DIR)/obj/tests/stdlib_codecs_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_digest_tests: $(BUILD_DIR)/obj/tests/stdlib_digest_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_signature_tests: $(BUILD_DIR)/obj/tests/stdlib_signature_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_benchmark_tests: $(BUILD_DIR)/obj/tests/stdlib_benchmark_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_secure_random_tests: $(BUILD_DIR)/obj/tests/stdlib_secure_random_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_bool_tests: $(BUILD_DIR)/obj/tests/stdlib_bool_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_argparser_tests: $(BUILD_DIR)/obj/tests/stdlib_argparser_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_regexp_tests: $(BUILD_DIR)/obj/tests/stdlib_regexp_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_uuid_tests: $(BUILD_DIR)/obj/tests/stdlib_uuid_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_time_tests: $(BUILD_DIR)/obj/tests/stdlib_time_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_url_tests: $(BUILD_DIR)/obj/tests/stdlib_url_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/stdlib_yaml_tests: $(BUILD_DIR)/obj/tests/stdlib_yaml_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/sputnik_ext_tests: $(BUILD_DIR)/obj/tests/sputnik_ext_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/sputnik $(BUILD_DIR)/sputnik_ext_tests: runtime/native_call_buffer.h

$(BUILD_DIR)/io_tests: $(BUILD_DIR)/obj/tests/io_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/http_codec_tests: $(BUILD_DIR)/obj/tests/http_codec_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/net_http_tests: $(BUILD_DIR)/obj/tests/net_http_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/net_http_tcp_tests: $(BUILD_DIR)/obj/tests/net_http_tcp_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/vm_net_http_tests: $(BUILD_DIR)/obj/tests/vm_net_http_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/module_loader_tests: $(BUILD_DIR)/obj/tests/module_loader_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

$(BUILD_DIR)/package_tests: $(BUILD_DIR)/obj/tests/package_tests.o $(LIBRARY) | $(BUILD_DIR)/.dir
	$(CXX) $(CXXFLAGS) $< $(LIBRARY) $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

test: build-tests
	python3 tests/source_extensions_test.py $(BUILD_DIR)/sputnik
	$(BUILD_DIR)/system_tests
	$(BUILD_DIR)/stdlib_system_tests
	python3 tests/system_backend_test.py $(BUILD_DIR)/sputnik
	python3 tests/threaded_backend_test.py $(BUILD_DIR)/sputnik
	$(BUILD_DIR)/lexer_tests
	$(BUILD_DIR)/parser_tests
	$(BUILD_DIR)/binder_tests
	$(BUILD_DIR)/checker_tests
	$(BUILD_DIR)/wasm_accel_tests
	$(BUILD_DIR)/modern_profile_tests
	$(BUILD_DIR)/build_tests
	$(BUILD_DIR)/hir_tests
	$(BUILD_DIR)/mir_tests
	$(BUILD_DIR)/native_tests
	$(BUILD_DIR)/frozen_image_tests
	$(BUILD_DIR)/bytecode_tests
	$(BUILD_DIR)/graph_linker_tests
	python3 tests/graph_linker_cli_smoke.py $(BUILD_DIR)/sputnik
	$(BUILD_DIR)/emitter_tests
	$(BUILD_DIR)/vm_tests
	$(BUILD_DIR)/stdlib_collections_tests
	$(BUILD_DIR)/stdlib_task_tests
	$(BUILD_DIR)/stdlib_registry_tests
	$(BUILD_DIR)/stdlib_json_tests
	$(BUILD_DIR)/stdlib_codecs_tests
	$(BUILD_DIR)/stdlib_digest_tests
	$(BUILD_DIR)/stdlib_signature_tests
	$(BUILD_DIR)/stdlib_benchmark_tests
	$(BUILD_DIR)/stdlib_secure_random_tests
	$(BUILD_DIR)/stdlib_bool_tests
	$(BUILD_DIR)/stdlib_argparser_tests
	$(BUILD_DIR)/stdlib_regexp_tests
	$(BUILD_DIR)/stdlib_uuid_tests
	$(BUILD_DIR)/stdlib_time_tests
	$(BUILD_DIR)/stdlib_url_tests
	$(BUILD_DIR)/stdlib_yaml_tests
	$(BUILD_DIR)/sputnik_ext_tests
	$(BUILD_DIR)/io_tests
	$(BUILD_DIR)/http_codec_tests
	$(BUILD_DIR)/net_http_tests
	$(BUILD_DIR)/net_http_tcp_tests
	$(BUILD_DIR)/module_loader_tests
	$(BUILD_DIR)/package_tests
	$(BUILD_DIR)/sputnik lex corpus/parse/lexer/basic/source.s > $(BUILD_DIR)/lexer-basic.tokens.json
	$(BUILD_DIR)/sputnik build tests/fixtures/w14_build/sputnik.build.json --out-dir $(BUILD_DIR)/w14_build/out --cache-dir $(BUILD_DIR)/w14_build/cache > $(BUILD_DIR)/w14-build-first.json
	$(BUILD_DIR)/sputnik build tests/fixtures/w14_build/sputnik.build.json --target bytecode --out-dir $(BUILD_DIR)/w14_build/out --cache-dir $(BUILD_DIR)/w14_build/cache > $(BUILD_DIR)/w14-build-second.json
	$(BUILD_DIR)/sputnik sputnikbc-verify $(BUILD_DIR)/w14_build/out/demo.main.sputnikbc > $(BUILD_DIR)/w14-main.verify.json
	$(BUILD_DIR)/sputnik metadata $(BUILD_DIR)/w14_build/out/demo.main.sputnikbc --json > $(BUILD_DIR)/w14-main.metadata.json
	$(BUILD_DIR)/sputnik verify $(BUILD_DIR)/w14_build/out/demo.main.sputnikbc --json > $(BUILD_DIR)/w14-main.public-verify.json
	! $(BUILD_DIR)/sputnik verify tests/fixtures/bad.sputnikbc --json > $(BUILD_DIR)/bad.public-verify.json
	grep -q '"schema": "sputnik.bc.v1"' $(BUILD_DIR)/w14-main.metadata.json
	grep -q '"schema": "sputnik.bc.verify.v1"' $(BUILD_DIR)/w14-main.public-verify.json
	grep -q '"code":"BC1002"' $(BUILD_DIR)/bad.public-verify.json
	$(BUILD_DIR)/sputnik sputnikbc-disasm $(BUILD_DIR)/w14_build/out/demo.main.sputnikbc > $(BUILD_DIR)/w14-main.disasm.txt
	grep -q '"native_output": "$(BUILD_DIR)/w14_build/out/demo.main"' $(BUILD_DIR)/w14-build-first.json
	$(BUILD_DIR)/w14_build/out/demo.main > $(BUILD_DIR)/w14-main-native-manifest.out
	grep -q '^42$$' $(BUILD_DIR)/w14-main-native-manifest.out
	$(BUILD_DIR)/sputnik tests/fixtures/run_script/main.s > $(BUILD_DIR)/run-script.out
	grep -q '^42$$' $(BUILD_DIR)/run-script.out
	$(BUILD_DIR)/sputnik build tests/fixtures/w14_build/src/main.s -o $(BUILD_DIR)/w14-main-exe > $(BUILD_DIR)/w14-main-exe-build.json
	grep -q '"entry": "main"' $(BUILD_DIR)/w14-main-exe-build.json
	grep -q '"native_entry": true' $(BUILD_DIR)/w14-main-exe-build.json
	$(BUILD_DIR)/w14-main-exe > $(BUILD_DIR)/w14-main-exe.out
	grep -q '^42$$' $(BUILD_DIR)/w14-main-exe.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_scalar_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-scalar-core > $(BUILD_DIR)/native-scalar-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-scalar-core-build.json
	awk '/compact native frame:/ { if ($$5 > $$8) reduced = 1 } END { exit reduced ? 0 : 1 }' $(BUILD_DIR)/native-scalar-core.native.cpp
	! grep -q '^  if (handler_seed != nullptr) native_seed_handler_frame' $(BUILD_DIR)/native-scalar-core.native.cpp
	! grep -q '^  } catch (NativeNonlocalReturn &signal)' $(BUILD_DIR)/native-scalar-core.native.cpp
	grep -q 'static SPUTNIK_NATIVE_COLD void native_bailout' $(BUILD_DIR)/native-scalar-core.native.cpp
	$(BUILD_DIR)/native-scalar-core > $(BUILD_DIR)/native-scalar-core.out
	grep -q '^12$$' $(BUILD_DIR)/native-scalar-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_scalar_core/main.s > $(BUILD_DIR)/native-scalar-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-scalar-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-scalar-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_str_bytes_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-str-bytes-core > $(BUILD_DIR)/native-str-bytes-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-str-bytes-core-build.json
	$(BUILD_DIR)/native-str-bytes-core > $(BUILD_DIR)/native-str-bytes-core.out
	grep -q '^23$$' $(BUILD_DIR)/native-str-bytes-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_str_bytes_core/main.s > $(BUILD_DIR)/native-str-bytes-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-str-bytes-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-str-bytes-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_regexp_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-regexp-core > $(BUILD_DIR)/native-regexp-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-regexp-core-build.json
	$(BUILD_DIR)/native-regexp-core > $(BUILD_DIR)/native-regexp-core.out
	grep -q '^42$$' $(BUILD_DIR)/native-regexp-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_regexp_core/main.s > $(BUILD_DIR)/native-regexp-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-regexp-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-regexp-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_sequence_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-sequence-core > $(BUILD_DIR)/native-sequence-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-sequence-core-build.json
	grep -q 'NativeClosure \*invocation = as_closure' $(BUILD_DIR)/native-sequence-core.native.cpp
	! grep -q 'NativeClosure invocation = \*as_closure' $(BUILD_DIR)/native-sequence-core.native.cpp
	test "$$(grep -c 'native_append_trace_frame(raised' $(BUILD_DIR)/native-sequence-core.native.cpp)" -eq 2
	$(BUILD_DIR)/native-sequence-core > $(BUILD_DIR)/native-sequence-core.out
	grep -q '^26$$' $(BUILD_DIR)/native-sequence-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_sequence_core/main.s > $(BUILD_DIR)/native-sequence-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-sequence-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-sequence-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_higher_order_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-higher-order-core > $(BUILD_DIR)/native-higher-order-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-higher-order-core-build.json
	$(BUILD_DIR)/native-higher-order-core > $(BUILD_DIR)/native-higher-order-core.out
	grep -q '^25$$' $(BUILD_DIR)/native-higher-order-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_higher_order_core/main.s > $(BUILD_DIR)/native-higher-order-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-higher-order-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-higher-order-core.dump
	$(BUILD_DIR)/sputnik build corpus/run/collection_counts/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-collection-counts > $(BUILD_DIR)/native-collection-counts-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-collection-counts-build.json
	$(BUILD_DIR)/native-collection-counts > $(BUILD_DIR)/native-collection-counts.out
	grep -q '^42$$' $(BUILD_DIR)/native-collection-counts.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_keyed_collections_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-keyed-collections-core > $(BUILD_DIR)/native-keyed-collections-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-keyed-collections-core-build.json
	$(BUILD_DIR)/native-keyed-collections-core > $(BUILD_DIR)/native-keyed-collections-core.out
	grep -q '^26$$' $(BUILD_DIR)/native-keyed-collections-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_keyed_collections_core/main.s > $(BUILD_DIR)/native-keyed-collections-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-keyed-collections-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-keyed-collections-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_open_protocol_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-open-protocol-core > $(BUILD_DIR)/native-open-protocol-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-open-protocol-core-build.json
	$(BUILD_DIR)/native-open-protocol-core > $(BUILD_DIR)/native-open-protocol-core.out
	grep -q '^"native-open-protocol-ok"$$' $(BUILD_DIR)/native-open-protocol-core.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_set_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-set-core > $(BUILD_DIR)/native-set-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-set-core-build.json
	$(BUILD_DIR)/native-set-core > $(BUILD_DIR)/native-set-core.out
	grep -q '^12$$' $(BUILD_DIR)/native-set-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_set_core/main.s > $(BUILD_DIR)/native-set-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-set-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-set-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_math_numeric_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-math-numeric-core > $(BUILD_DIR)/native-math-numeric-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-math-numeric-core-build.json
	$(BUILD_DIR)/native-math-numeric-core > $(BUILD_DIR)/native-math-numeric-core.out
	grep -q '^12$$' $(BUILD_DIR)/native-math-numeric-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_math_numeric_core/main.s > $(BUILD_DIR)/native-math-numeric-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-math-numeric-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-math-numeric-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_numeric_saturating_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-numeric-saturating-core > $(BUILD_DIR)/native-numeric-saturating-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-numeric-saturating-core-build.json
	$(BUILD_DIR)/native-numeric-saturating-core > $(BUILD_DIR)/native-numeric-saturating-core.out
	grep -q '^9$$' $(BUILD_DIR)/native-numeric-saturating-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_numeric_saturating_core/main.s > $(BUILD_DIR)/native-numeric-saturating-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-numeric-saturating-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-numeric-saturating-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_numeric_wrapping_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-numeric-wrapping-core > $(BUILD_DIR)/native-numeric-wrapping-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-numeric-wrapping-core-build.json
	$(BUILD_DIR)/native-numeric-wrapping-core > $(BUILD_DIR)/native-numeric-wrapping-core.out
	grep -q '^7$$' $(BUILD_DIR)/native-numeric-wrapping-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_numeric_wrapping_core/main.s > $(BUILD_DIR)/native-numeric-wrapping-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-numeric-wrapping-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-numeric-wrapping-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_text_value_stdlib_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-text-value-stdlib-core > $(BUILD_DIR)/native-text-value-stdlib-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-text-value-stdlib-core-build.json
	$(BUILD_DIR)/native-text-value-stdlib-core > $(BUILD_DIR)/native-text-value-stdlib-core.out
	grep -q '^25$$' $(BUILD_DIR)/native-text-value-stdlib-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_text_value_stdlib_core/main.s > $(BUILD_DIR)/native-text-value-stdlib-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-text-value-stdlib-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-text-value-stdlib-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_structured_modules_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-structured-modules-core > $(BUILD_DIR)/native-structured-modules-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-structured-modules-core-build.json
	$(BUILD_DIR)/native-structured-modules-core > $(BUILD_DIR)/native-structured-modules-core.out
	grep -q '^9$$' $(BUILD_DIR)/native-structured-modules-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_structured_modules_core/main.s > $(BUILD_DIR)/native-structured-modules-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-structured-modules-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-structured-modules-core.dump
	$(BUILD_DIR)/sputnik build tests/fixtures/native_capability_modules_core/main.s --entry main-only --grant random.secure --require-full-native -o $(BUILD_DIR)/native-capability-modules-core > $(BUILD_DIR)/native-capability-modules-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-capability-modules-core-build.json
	$(BUILD_DIR)/native-capability-modules-core > $(BUILD_DIR)/native-capability-modules-core.out
	grep -q '^11$$' $(BUILD_DIR)/native-capability-modules-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_capability_modules_core/main.s > $(BUILD_DIR)/native-capability-modules-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-capability-modules-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-capability-modules-core.dump
	python3 tests/native_http_query_test.py $(BUILD_DIR)/sputnik $(BUILD_DIR)/native-http-query
	$(BUILD_DIR)/sputnik build tests/fixtures/native_http_server_core/sputnik.build.yaml --target native --out-dir $(BUILD_DIR)/native-http-server-core --cache-dir $(BUILD_DIR)/native-http-server-core/cache --grant net.listen --require-full-native > $(BUILD_DIR)/native-http-server-core-build.json
	grep -q '"native_graph_full_coverage": true' $(BUILD_DIR)/native-http-server-core-build.json
	grep -q '"native_graph_vm_independent": true' $(BUILD_DIR)/native-http-server-core-build.json
	! grep -q 'invoke_native_stdlib_send' $(BUILD_DIR)/native-http-server-core/native.http_server_core.native.cpp
	! grep -q 'NativeCycleScope cycle_scope(cycle_boundary)' $(BUILD_DIR)/native-http-server-core/native.http_server_core.native.cpp
	grep -q 'runtime_http_server_read_body_chunk' $(BUILD_DIR)/native-http-server-core/native.http_server_core.native.cpp
	$(BUILD_DIR)/native-http-server-core/native.http_server_core > $(BUILD_DIR)/native-http-server-core.out
	grep -q '^7$$' $(BUILD_DIR)/native-http-server-core.out
	python3 tests/native_cycle_lifetime_test.py $(BUILD_DIR)/sputnik $(BUILD_DIR)/native-cycle-lifetime
	python3 tests/conditional_chain_test.py $(BUILD_DIR)/sputnik $(BUILD_DIR)/conditional-chain
	$(BUILD_DIR)/sputnik build tests/fixtures/native_fs_path_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-fs-path-core > $(BUILD_DIR)/native-fs-path-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-fs-path-core-build.json
	$(BUILD_DIR)/native-fs-path-core > $(BUILD_DIR)/native-fs-path-core.out
	grep -q '^8$$' $(BUILD_DIR)/native-fs-path-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_fs_path_core/main.s > $(BUILD_DIR)/native-fs-path-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-fs-path-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-fs-path-core.dump
	$(BUILD_DIR)/sputnik build corpus/run/native_object_state/source.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-object-state > $(BUILD_DIR)/native-object-state-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-object-state-build.json
	$(BUILD_DIR)/native-object-state > $(BUILD_DIR)/native-object-state.out
	grep -q '^87$$' $(BUILD_DIR)/native-object-state.out
	$(BUILD_DIR)/sputnik build corpus/run/native_exception_state/source.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-exception-state > $(BUILD_DIR)/native-exception-state-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-exception-state-build.json
	grep -q '^  if (handler_seed != nullptr) native_seed_handler_frame' $(BUILD_DIR)/native-exception-state.native.cpp
	grep -q 'static constexpr NativeTraceLocationRecord kNativeTraceLocations' $(BUILD_DIR)/native-exception-state.native.cpp
	grep -q 'static SPUTNIK_NATIVE_COLD NativeTraceFrame native_trace_frame' $(BUILD_DIR)/native-exception-state.native.cpp
	test "$$(grep -c 'native_append_trace_frame(raised' $(BUILD_DIR)/native-exception-state.native.cpp)" -eq 1
	$(BUILD_DIR)/native-exception-state > $(BUILD_DIR)/native-exception-state.out
	grep -q '^74$$' $(BUILD_DIR)/native-exception-state.out
	$(BUILD_DIR)/sputnik build corpus/run/native_io_state/source.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-io-state > $(BUILD_DIR)/native-io-state-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-io-state-build.json
	$(BUILD_DIR)/native-io-state > $(BUILD_DIR)/native-io-state.out
	grep -q '^5$$' $(BUILD_DIR)/native-io-state.out
	$(BUILD_DIR)/sputnik build corpus/run/native_task_state/source.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-task-state > $(BUILD_DIR)/native-task-state-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-task-state-build.json
	$(BUILD_DIR)/native-task-state > $(BUILD_DIR)/native-task-state.out
	grep -q '^4$$' $(BUILD_DIR)/native-task-state.out
	$(BUILD_DIR)/sputnik build corpus/run/native_map_get_or_set/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-map-get-or-set > $(BUILD_DIR)/native-map-get-or-set-build.json
	$(BUILD_DIR)/native-map-get-or-set > $(BUILD_DIR)/native-map-get-or-set.out
	grep -q '^64$$' $(BUILD_DIR)/native-map-get-or-set.out
	$(BUILD_DIR)/sputnik build corpus/run/native_pattern_triple_eq/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-pattern-triple-eq > $(BUILD_DIR)/native-pattern-triple-eq-build.json
	$(BUILD_DIR)/native-pattern-triple-eq > $(BUILD_DIR)/native-pattern-triple-eq.out
	grep -q '^21$$' $(BUILD_DIR)/native-pattern-triple-eq.out
	$(BUILD_DIR)/sputnik build corpus/run/native_user_exception_state/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-user-exception-state > $(BUILD_DIR)/native-user-exception-state-build.json
	$(BUILD_DIR)/native-user-exception-state > $(BUILD_DIR)/native-user-exception-state.out
	grep -q '^21$$' $(BUILD_DIR)/native-user-exception-state.out
	$(BUILD_DIR)/sputnik build corpus/run/native_collection_type_matchers/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-collection-type-matchers > $(BUILD_DIR)/native-collection-type-matchers-build.json
	$(BUILD_DIR)/native-collection-type-matchers > $(BUILD_DIR)/native-collection-type-matchers.out
	grep -q '^95$$' $(BUILD_DIR)/native-collection-type-matchers.out
	$(BUILD_DIR)/sputnik build corpus/run/native_call_shape/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-call-shape > $(BUILD_DIR)/native-call-shape-build.json
	$(BUILD_DIR)/native-call-shape > $(BUILD_DIR)/native-call-shape.out
	grep -q '^115$$' $(BUILD_DIR)/native-call-shape.out
	$(BUILD_DIR)/sputnik build corpus/run/task_mutex_parallel/source.s --entry main --require-full-native -o $(BUILD_DIR)/task-mutex-parallel > $(BUILD_DIR)/task-mutex-parallel-build.json
	$(BUILD_DIR)/task-mutex-parallel > $(BUILD_DIR)/task-mutex-parallel.out
	grep -q '^0$$' $(BUILD_DIR)/task-mutex-parallel.out
	$(BUILD_DIR)/sputnik build corpus/run/native_channel_parallel/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-channel-parallel > $(BUILD_DIR)/native-channel-parallel-build.json
	$(BUILD_DIR)/native-channel-parallel > $(BUILD_DIR)/native-channel-parallel.out
	grep -q '^42$$' $(BUILD_DIR)/native-channel-parallel.out
	$(BUILD_DIR)/sputnik build corpus/run/native_no_gil_parallel/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-no-gil-parallel > $(BUILD_DIR)/native-no-gil-parallel-build.json
	$(BUILD_DIR)/native-no-gil-parallel > $(BUILD_DIR)/native-no-gil-parallel.out
	grep -q '^2$$' $(BUILD_DIR)/native-no-gil-parallel.out
	$(BUILD_DIR)/sputnik build corpus/run/native_clause_method/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-clause-method > $(BUILD_DIR)/native-clause-method-build.json
	$(BUILD_DIR)/native-clause-method > $(BUILD_DIR)/native-clause-method.out
	grep -q '^3$$' $(BUILD_DIR)/native-clause-method.out
	$(BUILD_DIR)/sputnik build corpus/run/native_sequence_zip_to_map/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-sequence-zip-to-map > $(BUILD_DIR)/native-sequence-zip-to-map-build.json
	$(BUILD_DIR)/native-sequence-zip-to-map > $(BUILD_DIR)/native-sequence-zip-to-map.out
	grep -q '^35$$' $(BUILD_DIR)/native-sequence-zip-to-map.out
	$(BUILD_DIR)/sputnik build corpus/run/native_class_variables/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-class-variables > $(BUILD_DIR)/native-class-variables-build.json
	$(BUILD_DIR)/native-class-variables > $(BUILD_DIR)/native-class-variables.out
	grep -q '^5$$' $(BUILD_DIR)/native-class-variables.out
	$(BUILD_DIR)/sputnik build corpus/run/native_callable_instance/source.s --entry main --require-full-native -o $(BUILD_DIR)/native-callable-instance > $(BUILD_DIR)/native-callable-instance-build.json
	$(BUILD_DIR)/native-callable-instance > $(BUILD_DIR)/native-callable-instance.out
	grep -q '^18$$' $(BUILD_DIR)/native-callable-instance.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_benchmark_core/main.s --entry main-only --require-full-native -o $(BUILD_DIR)/native-benchmark-core > $(BUILD_DIR)/native-benchmark-core-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native-benchmark-core-build.json
	$(BUILD_DIR)/native-benchmark-core > $(BUILD_DIR)/native-benchmark-core.out
	grep -q '^4$$' $(BUILD_DIR)/native-benchmark-core.out
	$(BUILD_DIR)/sputnik native-dump tests/fixtures/native_benchmark_core/main.s > $(BUILD_DIR)/native-benchmark-core.dump
	grep -q 'cpp-bytecode-direct-v1 coverage' $(BUILD_DIR)/native-benchmark-core.dump
	grep -q 'mode=direct-native' $(BUILD_DIR)/native-benchmark-core.dump
	$(BUILD_DIR)/sputnik build bench/polyglot/sputnik/src/calls_collections.s --entry init -o $(BUILD_DIR)/calls-collections-native > $(BUILD_DIR)/calls-collections-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/calls-collections-native-build.json
	$(BUILD_DIR)/calls-collections-native > $(BUILD_DIR)/calls-collections-native.out
	grep -q '^2047795430$$' $(BUILD_DIR)/calls-collections-native.out
	$(BUILD_DIR)/sputnik build bench/polyglot/sputnik/src/sha_digest.s --entry init -o $(BUILD_DIR)/sha-digest-native > $(BUILD_DIR)/sha-digest-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/sha-digest-native-build.json
	$(BUILD_DIR)/sha-digest-native > $(BUILD_DIR)/sha-digest-native.out
	grep -q '^5512000$$' $(BUILD_DIR)/sha-digest-native.out
	$(BUILD_DIR)/sputnik build tests/fixtures/secure_random_native/main.s --entry main-only --grant random.secure --require-full-native -o $(BUILD_DIR)/secure-random-native > $(BUILD_DIR)/secure-random-native-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/secure-random-native-build.json
	$(BUILD_DIR)/secure-random-native > $(BUILD_DIR)/secure-random-native.out
	grep -q '^42$$' $(BUILD_DIR)/secure-random-native.out
	$(BUILD_DIR)/sputnik build tests/fixtures/secure_random_native/int.s --entry main-only --grant random.secure --require-full-native -o $(BUILD_DIR)/secure-random-native-int > $(BUILD_DIR)/secure-random-native-int-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/secure-random-native-int-build.json
	$(BUILD_DIR)/secure-random-native-int > $(BUILD_DIR)/secure-random-native-int.out
	grep -q '^42$$' $(BUILD_DIR)/secure-random-native-int.out
	$(BUILD_DIR)/sputnik build tests/fixtures/uuid_native/main.s --entry main-only --grant random.secure --require-full-native -o $(BUILD_DIR)/uuid-native > $(BUILD_DIR)/uuid-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/uuid-native-build.json
	$(BUILD_DIR)/uuid-native > $(BUILD_DIR)/uuid-native.out
	grep -q '^42$$' $(BUILD_DIR)/uuid-native.out
	$(BUILD_DIR)/sputnik build tests/fixtures/digest_native/main.s --entry main-only -o $(BUILD_DIR)/digest-native > $(BUILD_DIR)/digest-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/digest-native-build.json
	$(BUILD_DIR)/digest-native > $(BUILD_DIR)/digest-native.out
	grep -q '^42$$' $(BUILD_DIR)/digest-native.out
	$(BUILD_DIR)/sputnik build tests/fixtures/signature_native/main.s --entry main-only --grant random.secure --require-full-native -o $(BUILD_DIR)/signature-native > $(BUILD_DIR)/signature-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_full_coverage"] and result["native_body_coverage_full"] and result["native_vm_independent"] and not result["native_runtime_bridge"] and result["vm_fallback_code_count"] == 0 and result["native_fallback_code_count"] == 0 and not result["bytecode_fallback"], result' $(BUILD_DIR)/signature-native-build.json
	$(BUILD_DIR)/signature-native > $(BUILD_DIR)/signature-native.out
	grep -q '^42$$' $(BUILD_DIR)/signature-native.out
	$(BUILD_DIR)/sputnik build tests/fixtures/url_native/source.s -o $(BUILD_DIR)/url-native > $(BUILD_DIR)/url-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/url-native-build.json
	$(BUILD_DIR)/url-native > $(BUILD_DIR)/url-native.out
	grep -q '^42$$' $(BUILD_DIR)/url-native.out
	$(BUILD_DIR)/sputnik build bench/polyglot/sputnik/src/time_flow.s --entry main-only -o $(BUILD_DIR)/time-flow-native > $(BUILD_DIR)/time-flow-native-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["native_entry"] and result["native_code_count"] == result["bytecode_code_count"], result' $(BUILD_DIR)/time-flow-native-build.json
	$(BUILD_DIR)/time-flow-native > $(BUILD_DIR)/time-flow-native.out
	grep -q '^110397732$$' $(BUILD_DIR)/time-flow-native.out
	rm -rf $(BUILD_DIR)/native_ext_demo
	$(BUILD_DIR)/sputnik build tests/fixtures/native_ext_demo/sputnik.build.json --target native --out-dir $(BUILD_DIR)/native_ext_demo/out --cache-dir $(BUILD_DIR)/native_ext_demo/cache > $(BUILD_DIR)/native-ext-demo-build.json
	grep -q '"status": "ok"' $(BUILD_DIR)/native-ext-demo-build.json
	$(BUILD_DIR)/native_ext_demo/out/nat.demo > $(BUILD_DIR)/native-ext-demo-native.out
	grep -q '^42$$' $(BUILD_DIR)/native-ext-demo-native.out
	$(BUILD_DIR)/sputnik tests/fixtures/native_ext_demo/src/main.s > $(BUILD_DIR)/native-ext-demo-bytecode.out
	grep -q '^210$$' $(BUILD_DIR)/native-ext-demo-bytecode.out
	$(BUILD_DIR)/sputnik run tests/fixtures/native_ext_demo/sputnik.build.json > $(BUILD_DIR)/native-ext-demo-run-fallback.out
	grep -q '^210$$' $(BUILD_DIR)/native-ext-demo-run-fallback.out
	$(BUILD_DIR)/sputnik run tests/fixtures/native_ext_demo/sputnik.build.json --grant ffi > $(BUILD_DIR)/native-ext-demo-run-dylib.out
	grep -q '^42$$' $(BUILD_DIR)/native-ext-demo-run-dylib.out
	! $(BUILD_DIR)/sputnik run tests/fixtures/native_ext_demo/sputnik.build.json --grant ffi.load > $(BUILD_DIR)/native-ext-demo-run-incomplete.out 2>&1
	grep -q 'requires --grant ffi.call=demo.doubled' $(BUILD_DIR)/native-ext-demo-run-incomplete.out
	rm -rf $(BUILD_DIR)/native_graph_pure
	$(BUILD_DIR)/sputnik build tests/fixtures/native_graph_pure/sputnik.build.json --target native --out-dir $(BUILD_DIR)/native_graph_pure/out --cache-dir $(BUILD_DIR)/native_graph_pure/cache > $(BUILD_DIR)/native-graph-pure-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["status"] == "ok" and result["native_graph_module_count"] == 2 and result["native_graph_vm_fallback_code_count"] == 0, result' $(BUILD_DIR)/native-graph-pure-build.json
	$(BUILD_DIR)/native_graph_pure/out/nat.graph.main > $(BUILD_DIR)/native-graph-pure.out
	grep -q '^42$$' $(BUILD_DIR)/native-graph-pure.out
	$(MAKE) test-object-lifecycle
	rm -rf $(BUILD_DIR)/native_implicit_self_cli
	$(BUILD_DIR)/sputnik build tests/fixtures/native_implicit_self_cli/sputnik.build.json --target native --require-full-native --out-dir $(BUILD_DIR)/native_implicit_self_cli/out --cache-dir $(BUILD_DIR)/native_implicit_self_cli/cache > $(BUILD_DIR)/native-implicit-self-cli-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["status"] == "ok" and result["native_graph_module_count"] == 2 and result["native_graph_native_code_count"] == result["native_graph_code_count"] and result["native_graph_vm_fallback_code_count"] == 0 and not result["native_bytecode_fallback"], result' $(BUILD_DIR)/native-implicit-self-cli-build.json
	$(BUILD_DIR)/native_implicit_self_cli/out/native.implicit_self_cli.main --count 42 > $(BUILD_DIR)/native-implicit-self-cli.out
	grep -q '^84$$' $(BUILD_DIR)/native-implicit-self-cli.out
	rm -rf $(BUILD_DIR)/native_ext_dep
	$(BUILD_DIR)/sputnik build tests/fixtures/native_ext_dep/sputnik.build.json --target native --require-full-native --out-dir $(BUILD_DIR)/native_ext_dep/out --cache-dir $(BUILD_DIR)/native_ext_dep/cache > $(BUILD_DIR)/native-ext-dep-build.json
	python3 -c 'import json, sys; result = json.load(open(sys.argv[1])); assert result["status"] == "ok" and result["native_graph_module_count"] == 2 and result["native_graph_native_code_count"] == result["native_graph_code_count"] and result["native_graph_vm_fallback_code_count"] == 0 and not result["native_bytecode_fallback"] and result["native_extensions"] and result["native_extensions"][0]["native_source_sha256"].startswith("sha256:"), result' $(BUILD_DIR)/native-ext-dep-build.json
	! grep -Eq 'run_vm_entry|sputnik_vm_fallback|\.execute\(' $(BUILD_DIR)/native_ext_dep/out/nat.dep.main.native.cpp
	$(BUILD_DIR)/native_ext_dep/out/nat.dep.main > $(BUILD_DIR)/native-ext-dep-native.out
	grep -q '^42$$' $(BUILD_DIR)/native-ext-dep-native.out
	rm -rf $(BUILD_DIR)/native_class_demo
	$(BUILD_DIR)/sputnik build tests/fixtures/native_class_demo/sputnik.build.json --target native --out-dir $(BUILD_DIR)/native_class_demo/out --cache-dir $(BUILD_DIR)/native_class_demo/cache > $(BUILD_DIR)/native-class-demo-build.json
	grep -q '"status": "ok"' $(BUILD_DIR)/native-class-demo-build.json
	$(BUILD_DIR)/native_class_demo/out/nat.box > $(BUILD_DIR)/native-class-demo-native.out
	grep -q '^24$$' $(BUILD_DIR)/native-class-demo-native.out
	! $(BUILD_DIR)/sputnik tests/fixtures/native_class_demo/src/main.s > $(BUILD_DIR)/native-class-demo-bytecode.out 2>&1
	grep -q 'NativeRequiredError' $(BUILD_DIR)/native-class-demo-bytecode.out
	$(BUILD_DIR)/sputnik build tests/fixtures/not_implemented_error/sputnik.build.json --target native --require-full-native --out-dir $(BUILD_DIR)/not_implemented_error/out --cache-dir $(BUILD_DIR)/not_implemented_error/cache > $(BUILD_DIR)/not-implemented-error-build.json
	grep -q '"status": "ok"' $(BUILD_DIR)/not-implemented-error-build.json
	$(BUILD_DIR)/not_implemented_error/out/errors.not_implemented > $(BUILD_DIR)/not-implemented-error-native.out
	grep -q '^2$$' $(BUILD_DIR)/not-implemented-error-native.out
	$(BUILD_DIR)/sputnik run tests/fixtures/not_implemented_error/sputnik.build.json --grant ffi > $(BUILD_DIR)/not-implemented-error-vm.out
	grep -q '^2$$' $(BUILD_DIR)/not-implemented-error-vm.out
	$(BUILD_DIR)/sputniktest run corpus

.PHONY: test-object-lifecycle
test-object-lifecycle: $(BUILD_DIR)/sputnik
	$(BUILD_DIR)/sputnik run tests/fixtures/instance_fields_after_init/sputnik.build.json > $(BUILD_DIR)/object-lifecycle-vm.out
	grep -q '^PASS after_init! + instance_fields$$' $(BUILD_DIR)/object-lifecycle-vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/instance_fields_after_init/sputnik.build.json --target native --require-full-native --out-dir $(BUILD_DIR)/object-lifecycle > $(BUILD_DIR)/object-lifecycle-build.json
	PATH=/nonexistent $(BUILD_DIR)/object-lifecycle/conformance.run > $(BUILD_DIR)/object-lifecycle-native.out
	grep -q '^PASS after_init! + instance_fields$$' $(BUILD_DIR)/object-lifecycle-native.out

.PHONY: test-bool-parse
test: test-bool-parse
test-bool-parse: $(BUILD_DIR)/sputnik $(BUILD_DIR)/stdlib_bool_tests $(BUILD_DIR)/stdlib_argparser_tests
	$(BUILD_DIR)/stdlib_bool_tests
	$(BUILD_DIR)/stdlib_argparser_tests
	$(BUILD_DIR)/sputnik tests/fixtures/bool_parse_native/main.s > $(BUILD_DIR)/bool-parse-vm.out
	grep -q '^PASS Bool.parse$$' $(BUILD_DIR)/bool-parse-vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/bool_parse_native/main.s --entry main --require-full-native -o $(BUILD_DIR)/bool-parse-native > $(BUILD_DIR)/bool-parse-native-build.json
	python3 -c 'import json, sys; r = json.load(open(sys.argv[1])); assert r["native_full_coverage"] and r["native_vm_independent"] and not r["native_runtime_bridge"] and r["vm_fallback_code_count"] == 0, r' $(BUILD_DIR)/bool-parse-native-build.json
	$(BUILD_DIR)/bool-parse-native > $(BUILD_DIR)/bool-parse-native.out
	grep -q '^PASS Bool.parse$$' $(BUILD_DIR)/bool-parse-native.out
	$(BUILD_DIR)/sputnik build tests/fixtures/bool_parse_native/invalid_flag.s --entry main --require-full-native -o $(BUILD_DIR)/bool-parse-invalid-flag > $(BUILD_DIR)/bool-parse-invalid-flag-build.json
	! $(BUILD_DIR)/bool-parse-invalid-flag > $(BUILD_DIR)/bool-parse-invalid-flag.out 2>&1
	grep -q 'ArgParser.InvalidValue' $(BUILD_DIR)/bool-parse-invalid-flag.out

.PHONY: test-native-language-idioms
test: test-native-language-idioms test-native-call-buffers

.PHONY: test-native-call-buffers
test-native-call-buffers: $(BUILD_DIR)/sputnik
	mkdir -p $(BUILD_DIR)/native_call_buffers
	$(BUILD_DIR)/sputnik run tests/fixtures/native_call_buffers/sputnik.build.json --grant ffi > $(BUILD_DIR)/native_call_buffers/vm.out
	grep -q '^PASS native call buffers$$' $(BUILD_DIR)/native_call_buffers/vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_call_buffers/sputnik.build.json --target native --require-full-native --out-dir $(BUILD_DIR)/native_call_buffers/native > $(BUILD_DIR)/native_call_buffers/build.json
	grep -q '"native_graph_full_coverage": true' $(BUILD_DIR)/native_call_buffers/build.json
	$(BUILD_DIR)/native_call_buffers/native/test.call_buffers > $(BUILD_DIR)/native_call_buffers/native.out
	grep -q '^PASS native call buffers$$' $(BUILD_DIR)/native_call_buffers/native.out

test-native-language-idioms: $(BUILD_DIR)/sputnik
	mkdir -p $(BUILD_DIR)/native_language_idioms
	$(BUILD_DIR)/sputnik run tests/fixtures/native_class_demo/sputnik.build.json --grant ffi > $(BUILD_DIR)/native_language_idioms/lifecycle-vm.out
	grep -q '^24$$' $(BUILD_DIR)/native_language_idioms/lifecycle-vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_class_demo/sputnik.build.json --target native --require-full-native --out-dir $(BUILD_DIR)/native_language_idioms/lifecycle > $(BUILD_DIR)/native_language_idioms/lifecycle-build.json
	$(BUILD_DIR)/native_language_idioms/lifecycle/nat.box > $(BUILD_DIR)/native_language_idioms/lifecycle-native.out
	grep -q '^24$$' $(BUILD_DIR)/native_language_idioms/lifecycle-native.out
	$(BUILD_DIR)/sputnik tests/fixtures/native_conversion_properties/main.s > $(BUILD_DIR)/native_language_idioms/conversions-vm.out
	grep -q '^PASS native conversion properties$$' $(BUILD_DIR)/native_language_idioms/conversions-vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_conversion_properties/main.s --entry main --require-full-native -o $(BUILD_DIR)/native_language_idioms/conversions > $(BUILD_DIR)/native_language_idioms/conversions-build.json
	grep -q '"native_full_coverage": true' $(BUILD_DIR)/native_language_idioms/conversions-build.json
	$(BUILD_DIR)/native_language_idioms/conversions > $(BUILD_DIR)/native_language_idioms/conversions-native.out
	grep -q '^PASS native conversion properties$$' $(BUILD_DIR)/native_language_idioms/conversions-native.out
	! $(BUILD_DIR)/sputnik tests/fixtures/native_conversion_properties/invalid_set_spread.s > $(BUILD_DIR)/native_language_idioms/spread-vm.out 2>&1
	grep -q 'TypeError' $(BUILD_DIR)/native_language_idioms/spread-vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_conversion_properties/invalid_set_spread.s --entry main --require-full-native -o $(BUILD_DIR)/native_language_idioms/invalid-spread > $(BUILD_DIR)/native_language_idioms/spread-build.json
	! $(BUILD_DIR)/native_language_idioms/invalid-spread > $(BUILD_DIR)/native_language_idioms/spread-native.out 2>&1
	grep -q 'TypeError' $(BUILD_DIR)/native_language_idioms/spread-native.out
	$(BUILD_DIR)/sputnik run tests/fixtures/native_private_classes/sputnik.build.json > $(BUILD_DIR)/native_language_idioms/classes-vm.out
	grep -q '^PASS native private classes$$' $(BUILD_DIR)/native_language_idioms/classes-vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_private_classes/sputnik.build.json --target native --require-full-native --out-dir $(BUILD_DIR)/native_language_idioms/private > $(BUILD_DIR)/native_language_idioms/classes-build.json
	grep -q '"native_graph_full_coverage": true' $(BUILD_DIR)/native_language_idioms/classes-build.json
	$(BUILD_DIR)/native_language_idioms/private/native_private_classes > $(BUILD_DIR)/native_language_idioms/classes-native.out
	grep -q '^PASS native private classes$$' $(BUILD_DIR)/native_language_idioms/classes-native.out
	$(BUILD_DIR)/sputnik run tests/fixtures/native_fs_read_bytes/main.s --grant fs.read=./tests/fixtures/native_fs_read_bytes > $(BUILD_DIR)/native_language_idioms/fs-vm.out
	grep -q '^PASS native binary file read$$' $(BUILD_DIR)/native_language_idioms/fs-vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_fs_read_bytes/main.s --entry main --grant fs.read=./tests/fixtures/native_fs_read_bytes --require-full-native -o $(BUILD_DIR)/native_language_idioms/fs > $(BUILD_DIR)/native_language_idioms/fs-build.json
	$(BUILD_DIR)/native_language_idioms/fs > $(BUILD_DIR)/native_language_idioms/fs-native.out
	grep -q '^PASS native binary file read$$' $(BUILD_DIR)/native_language_idioms/fs-native.out
	! $(BUILD_DIR)/sputnik run tests/fixtures/native_fs_read_bytes/denied.s > $(BUILD_DIR)/native_language_idioms/fs-denied-vm.out 2>&1
	grep -q 'CapabilityError' $(BUILD_DIR)/native_language_idioms/fs-denied-vm.out
	$(BUILD_DIR)/sputnik build tests/fixtures/native_fs_read_bytes/denied.s --entry main --require-full-native -o $(BUILD_DIR)/native_language_idioms/fs-denied > $(BUILD_DIR)/native_language_idioms/fs-denied-build.json
	! $(BUILD_DIR)/native_language_idioms/fs-denied > $(BUILD_DIR)/native_language_idioms/fs-denied-native.out 2>&1
	grep -q 'CapabilityError' $(BUILD_DIR)/native_language_idioms/fs-denied-native.out

conformance: $(BUILD_DIR)/sputniktest
	$(BUILD_DIR)/sputniktest run corpus --bundle M11

backend-equivalence: $(BUILD_DIR)/sputnik
	python3 tools/backend_equivalence.py

spec-sync-check:
	python3 tools/spec_sync.py check

fmt:
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(FORMAT_FILES); \
	else \
		echo "clang-format not found; skipping"; \
	fi

clean:
	rm -rf $(BUILD_DIR)

.PHONY: test-http-tls
test-http-tls: $(BUILD_DIR)/sputnik $(BUILD_DIR)/vm_net_http_tests $(BUILD_DIR)/net_http_tests $(BUILD_DIR)/net_http_tcp_tests
	$(BUILD_DIR)/net_http_tests
	$(BUILD_DIR)/net_http_tcp_tests
	$(BUILD_DIR)/vm_net_http_tests
	python3 tests/http_tls_test.py $(BUILD_DIR)/sputnik $(BUILD_DIR)/http-tls

# The TLS resource state and canonical error list affect runtime ABI/dispatch.
$(BUILD_DIR)/sputnik $(BUILD_DIR)/vm_net_http_tests: runtime/tls.h runtime/net_http_client.h spec/registries/runtime_errors.def
