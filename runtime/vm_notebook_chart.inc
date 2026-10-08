// Board chart publication has no plot rendering or traversal of past points.
  SendStatus apply_notebook_chart(Frame &frame, const std::string &selector,
      const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args, Value *out) {
    auto sink = notebook_cell_context_.live_store;
    if (!sink) sink = NotebookLiveScope::current();
    try {
      if (!sink || !sink->open()) throw std::invalid_argument("board chart requires an active notebook run");
      if (!block.is_null()) throw std::invalid_argument("board chart does not accept a block");
      auto number = [&](const Value &v) {
        if (v.is_integer()) return static_cast<double>(v.as_integer());
        if (v.is_float()) return v.as_float();
        throw std::invalid_argument("chart coordinate must be numeric");
      };
      if (selector == "chart_begin") {
        if (args.size() != 1 || !args[0].is_string() || !stdlib_reject_unknown_keywords(&frame, kw_args,
            {"title", "x_label", "y_label", "caption", "width", "height", "stroke", "stroke_width", "throttle_ms", "order", "domain"}))
          throw std::invalid_argument("chart_begin requires one string id and chart style keywords");
        NotebookChartStyle style;
        style.id = string_text_from_value(args[0]).value_or("");
        if (style.id.empty() || style.id.size() > 4096) throw std::invalid_argument("invalid chart id");
        auto text = [&](const char *key, std::string &target) {
          if (auto v = keyword_arg_value(kw_args, key)) {
            if (!v->is_string()) throw std::invalid_argument(std::string("chart ") + key + " must be Str");
            target = string_text_from_value(*v).value_or("");
            if (target.size() > 16384) throw std::invalid_argument("chart label exceeds 16 KiB");
          }
        };
        text("title", style.title); text("x_label", style.x_label); text("y_label", style.y_label);
        text("caption", style.caption); text("stroke", style.stroke);
        if (style.stroke.size() != 7 || style.stroke[0] != '#' ||
            style.stroke.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
          throw std::invalid_argument("chart stroke must be #rrggbb");
        auto integer = [&](const char *key, std::uint64_t fallback) {
          const auto value = keyword_arg_value(kw_args, key);
          if (!value) return fallback;
          if (!value->is_integer() || value->as_integer() < 0) throw std::invalid_argument(std::string("chart ") + key + " must be nonnegative Int");
          return static_cast<std::uint64_t>(value->as_integer());
        };
        const auto w = integer("width", 900), h = integer("height", 420);
        if (w < 160 || h < 160 || w > 8192 || h > 8192 || w * h > 16000000)
          throw std::invalid_argument("chart dimensions must fit 160..8192 and 16 megapixels");
        style.width = w; style.height = h; style.throttle_ms = integer("throttle_ms", 500);
        if (style.throttle_ms > 3600000) throw std::invalid_argument("chart throttle exceeds one hour");
        if (auto v = keyword_arg_value(kw_args, "stroke_width")) style.stroke_width = number(*v);
        if (!std::isfinite(style.stroke_width) || style.stroke_width <= 0 || style.stroke_width > 1024)
          throw std::invalid_argument("invalid chart stroke_width");
        unsigned remaining = 256;
        style.order = notebook_order(keyword_arg_value(kw_args, "order").value_or(Value::integer(0)), 0, remaining);
        if (const auto v = keyword_arg_value(kw_args, "domain"); v && !v->is_null()) {
          if (!v->is_list() || !v->as_list() || v->as_list()->items.size() != 4)
            throw std::invalid_argument("chart domain must be [xmin,ymin,xmax,ymax]");
          std::array<double, 4> domain;
          for (std::size_t i = 0; i < 4; ++i) {
            domain[i] = number(v->as_list()->items[i]);
            if (!std::isfinite(domain[i]) || std::abs(domain[i]) >= 1e100) throw std::invalid_argument("chart domain must be finite");
          }
          for (std::size_t i = 0; i < 2; ++i)
            if (domain[i + 2] - domain[i] <= std::max(1e-100, std::max(std::abs(domain[i]), std::abs(domain[i + 2])) * 1e-13))
              throw std::invalid_argument("chart domain maximum must exceed minimum with a resolvable interval");
          style.domain = domain;
        }
        const auto cell = notebook_cell_context_.live_cell_id ? notebook_cell_context_.live_cell_id : NotebookLiveScope::current_cell_id();
        const auto generation = notebook_cell_context_.live_generation ? notebook_cell_context_.live_generation : NotebookLiveScope::cell_generation();
        *out = Value::integer(static_cast<std::int64_t>(sink->begin_chart(cell, generation, std::move(style))));
      } else {
        if (!kw_args.empty() || args.empty() || !args[0].is_integer() || args[0].as_integer() <= 0)
          throw std::invalid_argument("invalid chart handle");
        const auto handle = static_cast<std::uint64_t>(args[0].as_integer());
        if (selector == "chart_point" && args.size() == 3) sink->append_chart(handle, {number(args[1]), number(args[2])});
        else if (selector == "chart_end" && args.size() == 1) sink->end_chart(handle);
        else throw std::invalid_argument("chart_point accepts handle,x,y; chart_end accepts handle");
        *out = Value::null();
      }
      return SendStatus::Matched;
    } catch (const std::exception &e) {
      set_fault(frame, "NotebookCellError", e.what()); return SendStatus::Faulted;
    }
  }
