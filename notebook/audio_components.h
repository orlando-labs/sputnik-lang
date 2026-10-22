#pragma once
#include "notebook/project.h"
#include "runtime/audio.h"
#include <functional>
namespace sputnik::notebook {
inline std::shared_ptr<const std::vector<runtime::NotebookAudioComponent>> project_audio_components(const ProjectDocument &document, const std::string &sheet) {
  auto out = std::make_shared<std::vector<runtime::NotebookAudioComponent>>();
  std::function<void(const std::vector<ProjectComponent> &, const std::string &, CellId)> visit;
  visit = [&](const auto &components, const auto &board, CellId owner) {
    for (const auto &c : components) {
      if (c.panels) for (const auto &panel : *c.panels) visit(panel.components, board, owner);
      if (c.kind != "audio") continue;
      std::string channel;
      const auto value = c.extra.find("audio_id");
      if (value != c.extra.end()) channel = std::get<std::string>(parse_project_input_value(value->second));
      out->push_back({board, c.id, channel, owner ? owner : c.cell});
    }
  };
  for (const auto &b : document.boards) if (b.sheet == sheet) visit(b.components, b.id, 0);
  for (const auto &s : document.sheets) if (s.id == sheet) for (const auto &cell : s.cells) {
    const auto found = cell.extra.find("cell_ui"); if (found == cell.extra.end()) continue;
    const auto ui = parse_project_cell_ui(found->second);
    const auto scope = "cell_" + std::to_string(cell.id);
    visit(ui.pre, scope, cell.id); visit(ui.post, scope, cell.id);
  }
  return out;
}
}
