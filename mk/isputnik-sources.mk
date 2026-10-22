# Paths are relative to the language checkout. The standalone GUI compiles
# this same session/engine code with its own optional presentation renderers.
NOTEBOOK_GRAPH_SRCS := notebook/model.cpp notebook/analysis.cpp notebook/dependency_graph.cpp notebook/scheduler.cpp
NOTEBOOK_SRCS := $(NOTEBOOK_GRAPH_SRCS) notebook/slot_table.cpp notebook/kernel.cpp notebook/vm_cell_executor.cpp notebook/compiler.cpp notebook/renderers.cpp
NOTEBOOK_PROJECT_SRCS := notebook/project.cpp notebook/project_json.cpp
ISPUTNIK_SESSION_SRCS := tools/isputnik/session.cpp tools/isputnik/project_session.cpp tools/isputnik/tabs.cpp tools/isputnik/environment.cpp tools/isputnik/dependencies.cpp tools/isputnik/activity.cpp tools/isputnik/dispatch.cpp $(NOTEBOOK_PROJECT_SRCS) $(NOTEBOOK_SRCS)
ISPUTNIK_TEST_NAMES := isputnik_tests isputnik_project_tests isputnik_tabs_tests isputnik_activity_tests isputnik_poll_tests isputnik_dispatch_tests isputnik_terminal_wait_tests notebook_core_tests notebook_compiler_tests notebook_kernel_tests notebook_slot_tests notebook_vm_integration_tests notebook_project_tests notebook_dependency_tests
ISPUTNIK_TEST_NAMES += package_store_tests
