#include "notebook/renderers.h"
#ifdef AMBER_NOTEBOOK_PLOT
#include "runtime/amber_ext.h"
#include "runtime/amber_ext_runtime.h"
#include <mutex>
extern "C" AmberStatus amber_plot_render_png(AmberCtx *, const AmberValue *, size_t, AmberValue *);
extern "C" AmberStatus amber_plot_render_3d_png(AmberCtx *, const AmberValue *, size_t, AmberValue *);
#endif

namespace amber::notebook {
void register_builtin_renderers() {
#ifdef AMBER_NOTEBOOK_PLOT
  static std::once_flag once;
  std::call_once(once, [] {
    auto &registry = amber::runtime::NativeExtRegistry::global();
    registry.register_thunk("plot.render_png", reinterpret_cast<void *>(&amber_plot_render_png));
    registry.register_thunk("plot.render_3d_png", reinterpret_cast<void *>(&amber_plot_render_3d_png));
  });
#endif
}
}
