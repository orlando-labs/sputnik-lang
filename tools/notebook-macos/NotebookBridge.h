#ifndef AMBER_NOTEBOOK_BRIDGE_H
#define AMBER_NOTEBOOK_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AmberNotebook AmberNotebook;
typedef struct AmberNotebookControl AmberNotebookControl;

// All calls for a handle, including destruction, belong to ONE serial owner
// queue. Returned strings are UTF-8, caller-owned, and freed with string_free.
// Opening/creating, editing and saving never execute Amber source.
AmberNotebook *amber_notebook_open(const char *path, int create,
                                  const char *title, char **error);
void amber_notebook_destroy(AmberNotebook *notebook);
void amber_notebook_string_free(char *value);
char *amber_notebook_snapshot(AmberNotebook *notebook, char **error);

// Opt-in preview, configured once immediately after open, before any command.
// Never silently falls back to executing code in the host. Returned control
// owns an independent lifetime; its methods alone are safe on another thread.
AmberNotebookControl *amber_notebook_enable_worker(AmberNotebook *, const char *executable, char **error);
void amber_notebook_control_destroy(AmberNotebookControl *);
int amber_notebook_control_phase(AmberNotebookControl *); // 0 idle; 1..5 prepare/run/drain/stop/terminate
int amber_notebook_control_stop(AmberNotebookControl *, int force);
// Thread-safe, immutable latest live output; null means no update. Free with string_free.
char *amber_notebook_control_live(AmberNotebookControl *);

// Stable IDs: tab is "sheet:<id>" or "module:<id>"; cell is a decimal string.
// Actions: select, source, watch (text "on"/"off"), run, run_all, apply,
// add_cell/add_text (after cell), rich_source (JSON source+formatting payload),
// delete_cell, move_up, move_down, save, save_all,
// new_sheet (text = ID), new_module (text = ID), auto_import (text on/off).
// A failed operation never throws across the C boundary. Runtime diagnostics
// are available in the next snapshot even when an evaluation faults.
int amber_notebook_command(AmberNotebook *notebook, const char *action,
                           const char *tab, const char *cell,
                           const char *text, char **error);

// Borrowed event descriptor. Do not read/close it. Pump consumes the mailbox
// on the owner queue and processes bounded runtime work across the tabs.
// next_delay_ms = -1 means idle, otherwise schedule one bounded retry.
int amber_notebook_activity_descriptor(AmberNotebook *notebook, char **error);
int amber_notebook_pump(AmberNotebook *notebook, int *next_delay_ms,
                        char **error);

#ifdef __cplusplus
}
#endif
#endif
