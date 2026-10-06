BUILD_DIR ?= build

ifeq ($(origin CXX),default)
CXX := clang++
endif

CPPFLAGS ?=
CPPFLAGS += -I.

CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
ifeq ($(DEBUG),1)
CXXFLAGS += -g
endif
LDFLAGS ?=

# TLS is shared by the VM and native runtime. On macOS OpenSSL is keg-only;
# pkg-config (or OPENSSL_PREFIX) locates its headers and libraries.
OPENSSL_PREFIX ?=
OPENSSL_INCLUDE_DIR ?= $(if $(strip $(OPENSSL_PREFIX)),$(OPENSSL_PREFIX)/include,$(shell pkg-config --variable=includedir openssl 2>/dev/null))
OPENSSL_LIB_DIR ?= $(if $(strip $(OPENSSL_PREFIX)),$(OPENSSL_PREFIX)/lib,$(shell pkg-config --variable=libdir openssl 2>/dev/null))
ifneq ($(strip $(OPENSSL_INCLUDE_DIR)),)
CPPFLAGS += -I$(OPENSSL_INCLUDE_DIR) '-DSPUTNIK_OPENSSL_INCLUDE_DIR="$(OPENSSL_INCLUDE_DIR)"'
endif
ifneq ($(strip $(OPENSSL_LIB_DIR)),)
LDFLAGS += -L$(OPENSSL_LIB_DIR)
CPPFLAGS += '-DSPUTNIK_OPENSSL_LIB_DIR="$(OPENSSL_LIB_DIR)"'
endif
LDFLAGS += -lssl -lcrypto

# GOST R 34.10-2012 needs Nettle's supported TC26 curves. Other signature
# algorithms remain available when this optional backend is not installed.
NETTLE_GOST_LIBS := $(shell pkg-config --libs hogweed nettle gmp 2>/dev/null)
ifneq ($(strip $(NETTLE_GOST_LIBS)),)
CPPFLAGS += $(shell pkg-config --cflags hogweed nettle gmp 2>/dev/null) -DSPUTNIK_HAVE_NETTLE_GOST
CPPFLAGS += '-DSPUTNIK_NETTLE_INCLUDE_DIR="$(shell pkg-config --variable=includedir hogweed)"'
CPPFLAGS += '-DSPUTNIK_NETTLE_LIB_DIR="$(shell pkg-config --variable=libdir hogweed)"'
CPPFLAGS += '-DSPUTNIK_GMP_INCLUDE_DIR="$(shell pkg-config --variable=includedir gmp)"'
CPPFLAGS += '-DSPUTNIK_GMP_LIB_DIR="$(shell pkg-config --variable=libdir gmp)"'
LDFLAGS += $(NETTLE_GOST_LIBS)
endif

# --- Allocator selection -----------------------------------------------------
# MALLOC selects the C/C++ allocator the binaries link against (RESEARCH heap
# fragmentation, §10 step 1). Flavors:
#   system   (default) -- platform malloc, no extra dependency
#   mimalloc            -- recommended; best macOS story, aggressive page return
#   jemalloc           -- Linux deploys + diagnosis (stats_print, heap profiling)
# Install via `brew install mimalloc` / `brew install jemalloc`, or point
# MIMALLOC_PREFIX / JEMALLOC_PREFIX at an existing install.
#
# Page return is a *runtime* knob, not a build flag -- set it when you run:
#   mimalloc: MIMALLOC_PURGE_DELAY=0            (decommit dirty pages immediately)
#   jemalloc: MALLOC_CONF=background_thread:true,dirty_decay_ms:0,muzzy_decay_ms:0
# Use SPUTNIK_HEAP_STATS=1 on a run to print the §9 RSS / live-bytes line.
MALLOC ?= system
UNAME_S := $(shell uname -s)

# `sputnik run <manifest> --grant ffi` loads package thunks into the interpreter
# with dlopen. Export the stable sputnik_ext.h ABI from the main executable so a
# package .dylib/.so can resolve it without linking a second runtime copy.
ifeq ($(UNAME_S),Darwin)
SPUTNIK_DYNAMIC_EXPORT_FLAGS := -Wl,-export_dynamic
SPUTNIK_DYNAMIC_LOADER_LIBS :=
else ifeq ($(UNAME_S),Linux)
SPUTNIK_DYNAMIC_EXPORT_FLAGS := -Wl,--export-dynamic
SPUTNIK_DYNAMIC_LOADER_LIBS := -ldl
else
SPUTNIK_DYNAMIC_EXPORT_FLAGS :=
SPUTNIK_DYNAMIC_LOADER_LIBS := -ldl
endif

ifeq ($(MALLOC),system)
CPPFLAGS += -DSPUTNIK_ALLOCATOR=\"system\"
else ifeq ($(MALLOC),mimalloc)
CPPFLAGS += -DSPUTNIK_ALLOCATOR=\"mimalloc\"
MIMALLOC_PREFIX ?= $(shell brew --prefix mimalloc 2>/dev/null)
ifeq ($(strip $(MIMALLOC_PREFIX)),)
$(error MALLOC=mimalloc but mimalloc was not found; run `brew install mimalloc` or set MIMALLOC_PREFIX)
endif
CPPFLAGS += -I$(MIMALLOC_PREFIX)/include
MIMALLOC_STATIC := $(firstword $(wildcard $(MIMALLOC_PREFIX)/lib/libmimalloc.a))
ifeq ($(UNAME_S),Darwin)
# Force-load the static archive so mimalloc's malloc-zone override wins over the
# system allocator; fall back to dynamic linking if no static archive is present.
ifeq ($(strip $(MIMALLOC_STATIC)),)
LDFLAGS += -L$(MIMALLOC_PREFIX)/lib -lmimalloc
else
LDFLAGS += -Wl,-force_load,$(MIMALLOC_STATIC)
endif
else
LDFLAGS += -L$(MIMALLOC_PREFIX)/lib -Wl,-rpath,$(MIMALLOC_PREFIX)/lib -lmimalloc
endif
else ifeq ($(MALLOC),jemalloc)
CPPFLAGS += -DSPUTNIK_ALLOCATOR=\"jemalloc\"
JEMALLOC_PREFIX ?= $(shell brew --prefix jemalloc 2>/dev/null)
ifeq ($(strip $(JEMALLOC_PREFIX)),)
$(error MALLOC=jemalloc but jemalloc was not found; run `brew install jemalloc` or set JEMALLOC_PREFIX)
endif
CPPFLAGS += -I$(JEMALLOC_PREFIX)/include
JEMALLOC_STATIC := $(firstword $(wildcard $(JEMALLOC_PREFIX)/lib/libjemalloc.a))
ifeq ($(UNAME_S),Darwin)
# jemalloc registers its malloc zone from a static-archive constructor; force-load
# it so the override takes effect (macOS jemalloc is second-tier -- see RESEARCH §4).
ifeq ($(strip $(JEMALLOC_STATIC)),)
LDFLAGS += -L$(JEMALLOC_PREFIX)/lib -ljemalloc
else
LDFLAGS += -Wl,-force_load,$(JEMALLOC_STATIC)
endif
else
LDFLAGS += -L$(JEMALLOC_PREFIX)/lib -Wl,-rpath,$(JEMALLOC_PREFIX)/lib -ljemalloc
endif
else
$(error Unknown MALLOC='$(MALLOC)'; use system, mimalloc, or jemalloc)
endif
# -----------------------------------------------------------------------------

# --- Value representation selection ------------------------------------------
# VALUE_REPR selects the runtime `Value` storage. Flavors:
#   tagged  (default)  -- 16-byte tagged-union Value. Immediates and
#                         the six ObjHeader heap kinds are stored inline; the
#                         ~15 cold tail types (BigInt/error/task/io/...) are
#                         boxed behind a refcounted TailBox (+1 alloc per tail
#                         value, all cold paths). Defines SPUTNIK_VALUE_REPR_TAGGED.
#   variant            -- legacy 24-byte std::variant Value, retained for
#                         compatibility A/Bs during the tagged migration.
# A/B the legacy representation against the default with:
#   make BUILD_DIR=build-variant build-variant/sputnik VALUE_REPR=variant
# (do not mix object files across reps -- rebuild from clean or use a fresh
# BUILD_DIR, since the flag changes sizeof(Value) ABI-wide).
VALUE_REPR ?= tagged
ifeq ($(VALUE_REPR),variant)
# default storage; no macro needed
else ifeq ($(VALUE_REPR),tagged)
CPPFLAGS += -DSPUTNIK_VALUE_REPR_TAGGED
else
$(error Unknown VALUE_REPR='$(VALUE_REPR)'; use variant or tagged)
endif
# -----------------------------------------------------------------------------
