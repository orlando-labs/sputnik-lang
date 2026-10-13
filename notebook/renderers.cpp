#include "notebook/renderers.h"
#ifdef SPUTNIK_NOTEBOOK_PLOT
#include "runtime/sputnik_ext.h"
#include "runtime/sputnik_ext_runtime.h"
#include <mutex>
extern "C" SputnikStatus sputnik_plot_render_png(SputnikCtx *, const SputnikValue *, size_t, SputnikValue *);
extern "C" SputnikStatus sputnik_plot_render_3d_png(SputnikCtx *, const SputnikValue *, size_t, SputnikValue *);
#endif

namespace sputnik::notebook {
void register_builtin_renderers() {
#ifdef SPUTNIK_NOTEBOOK_PLOT
  static std::once_flag once;
  std::call_once(once, [] {
    auto &registry = sputnik::runtime::NativeExtRegistry::global();
    registry.register_thunk("plot.render_png", reinterpret_cast<void *>(&sputnik_plot_render_png));
    registry.register_thunk("plot.render_3d_png", reinterpret_cast<void *>(&sputnik_plot_render_3d_png));
  });
#endif
}
}
