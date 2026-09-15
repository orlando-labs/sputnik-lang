#pragma once

// Public entry point for the first notebook-core slice.  Keeping this small
// umbrella header lets iamber and future native clients depend on the model
// without reaching into implementation-specific files.
#include "notebook/compiler.h"
#include "notebook/dependency_graph.h"
#include "notebook/kernel.h"
#include "notebook/model.h"
#include "notebook/scheduler.h"
#include "notebook/slot_table.h"
#include "notebook/vm_cell_executor.h"
