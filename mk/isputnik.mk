include mk/isputnik-sources.mk

ISPUTNIK_SRCS := $(ISPUTNIK_SESSION_SRCS) tools/isputnik/terminal_wait.cpp
ISPUTNIK_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/obj/%.o,$(ISPUTNIK_SRCS))
ISPUTNIK_LIBRARY := $(BUILD_DIR)/libisputnik.a
ISPUTNIK_LDLIBS ?= -lncurses
ISPUTNIK_TEST_BINS := $(addprefix $(BUILD_DIR)/,$(ISPUTNIK_TEST_NAMES))
-include $(ISPUTNIK_OBJS:.o=.d)

$(ISPUTNIK_LIBRARY): $(ISPUTNIK_OBJS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/isputnik: $(BUILD_DIR)/obj/tools/isputnik/main.o $(ISPUTNIK_LIBRARY) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) $(ISPUTNIK_LDLIBS) -o $@

$(ISPUTNIK_TEST_BINS): $(BUILD_DIR)/%: $(BUILD_DIR)/obj/tests/%.o $(ISPUTNIK_LIBRARY) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) $(SPUTNIK_DYNAMIC_EXPORT_FLAGS) $(SPUTNIK_DYNAMIC_LOADER_LIBS) -o $@

.PHONY: test-isputnik test-isputnik-tui
build-tests: $(ISPUTNIK_TEST_BINS)
test: test-isputnik
test-isputnik: $(ISPUTNIK_TEST_BINS) $(BUILD_DIR)/isputnik
	@set -e; $(foreach binary,$(ISPUTNIK_TEST_BINS),"$(binary)";)
	python3 tests/isputnik_system_test.py $(BUILD_DIR)/isputnik

test-isputnik-tui: $(BUILD_DIR)/isputnik
	python3 tests/isputnik_tui_smoke.py $(BUILD_DIR)/isputnik
	python3 tests/isputnik_project_tui_smoke.py $(BUILD_DIR)/isputnik
	python3 tests/isputnik_tabs_tui_smoke.py $(BUILD_DIR)/isputnik
	python3 tests/isputnik_project_commands_tui_smoke.py $(BUILD_DIR)/isputnik
