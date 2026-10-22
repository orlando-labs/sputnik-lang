// Image descriptors are ordinary maps produced by notebook.ui.image.
  SendStatus apply_notebook_read_images(Frame &frame, const std::string &selector,
      const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args, Value *out) {
    try {
      if (args.size() != 1 || !args[0].is_string() || !block.is_null() ||
          !stdlib_reject_unknown_keywords(&frame, kw_args, selector == "read_image" ? std::initializer_list<const char *>{"index"} : std::initializer_list<const char *>{}))
        throw std::invalid_argument("read_image/read_images require a channel ID");
      const auto capture = notebook_cell_context_.input_capture;
      const auto inputs = notebook_cell_context_.project_images;
      const auto id = string_text_from_value(args[0]).value_or("");
      if (!capture || !capture->active() || !inputs || !inputs->count(id))
        throw std::invalid_argument("image reading requires an active notebook cell and a configured board channel");
      auto input = inputs->at(id);
      if (!input.readable) throw std::invalid_argument("image reading is disabled for channel: " + id);
      auto sink = notebook_cell_context_.live_store;
      if (sink) if (const auto live = sink->image_channel(id)) { input.images = live->images; input.caption = live->caption; }
      auto descriptor = [&](std::size_t index) {
        const auto &image = input.images[index];
        auto key = [&](const char *name) { return stdlib_symbol_value_from_text(name); };
        return make_symbol_map_value({
          {key("data"), stdlib_bytes_value_from_bytes(image.bytes)},
          {key("width"), Value::integer(image.width)}, {key("height"), Value::integer(image.height)},
          {key("format"), stdlib_symbol_value_from_text(image.mime == "image/jpeg" ? "jpeg" : "png")},
          {key("caption"), string_value_from_text(image.caption)},
          {key("asset_id"), string_value_from_text(notebook_asset_id(image.bytes))},
          {key("series_caption"), string_value_from_text(input.caption)}, {key("index"), Value::integer(index)}
        });
      };
      if (selector == "read_images") {
        std::vector<Value> images;
        for (std::size_t i = 0; i < input.images.size(); ++i) images.push_back(descriptor(i));
        *out = make_list_value(std::move(images));
      } else {
        auto index = input.images.empty() ? 0 : std::min(input.selected, input.images.size() - 1);
        if (const auto value = keyword_arg_value(kw_args, "index"); value && !value->is_null()) {
          if (!value->is_integer() || value->as_integer() < 0 || std::size_t(value->as_integer()) >= input.images.size())
            throw std::invalid_argument("image index is out of range");
          index = value->as_integer();
        }
        *out = input.images.empty() ? Value::null() : descriptor(index);
      }
      return SendStatus::Matched;
    } catch (const std::exception &error) {
      set_fault(frame, "NotebookCellError", error.what()); return SendStatus::Faulted;
    }
  }
  SendStatus apply_notebook_images(Frame &frame, const std::vector<Value> &args,
      const Value &block, const std::vector<std::pair<std::uint32_t, Value>> &kw_args, Value *out) {
    try {
      if (args.size() != 2 || !args[0].is_string() || !args[1].is_list() || !args[1].as_list() ||
          !block.is_null() || !stdlib_reject_unknown_keywords(&frame, kw_args, {"caption", "append"}))
        throw std::invalid_argument("notebook.images accepts channel ID, image list, caption: and append:");
      const auto &items = args[1].as_list()->items;
      if (items.size() > 64) throw std::invalid_argument("image series exceeds 64 images");
      std::optional<std::string> caption;
      if (const auto v = keyword_arg_value(kw_args, "caption"); v && !v->is_null()) {
        if (!v->is_string()) throw std::invalid_argument("series caption must be Str or null");
        caption = string_text_from_value(*v).value_or("");
      }
      bool append = false;
      if (const auto v = keyword_arg_value(kw_args, "append")) {
        if (!v->is_bool()) throw std::invalid_argument("append must be Bool");
        append = v->as_bool();
      }
      auto sink = notebook_cell_context_.live_store;
      if (!sink) sink = NotebookLiveScope::current();
      if (!sink || !sink->open()) throw std::invalid_argument("image publication requires an active notebook run");
      std::vector<NotebookDisplay> images;
      std::size_t total_bytes = 0;
      for (const auto &item : items) {
        if (!item.is_map() || !item.as_map()) throw std::invalid_argument("expected notebook.ui.image descriptor");
        auto field = [&](const char *key) {
          for (const auto &entry : item.as_map()->entries)
            if (selector_text_from_value(entry.key).value_or("") == key) return entry.value;
          return Value::null();
        };
        NotebookDisplay image;
        const auto label = field("caption");
        if (!label.is_null()) {
          if (!label.is_string()) throw std::invalid_argument("image caption must be Str or null");
          image.caption = string_text_from_value(label).value_or("");
        }
        const auto format = selector_text_from_value(field("format")).value_or("");
        const auto data = field("data");
        if (data.is_list() && data.as_list()) {
          if (data.as_list()->items.size() > 16U * 1024U * 1024U) throw std::invalid_argument("image data exceeds 16 MiB");
          for (const auto &sample : data.as_list()->items) {
            if (!sample.is_integer() || sample.as_integer() < 0 || sample.as_integer() > 255)
              throw std::invalid_argument("pixel samples must be Int in 0..255");
            image.bytes += static_cast<char>(sample.as_integer());
          }
        } else {
          const auto bytes = stdlib_bytes_of(&frame, data);
          if (!bytes) return SendStatus::Faulted;
          image.bytes = *bytes;
        }
        if (image.bytes.size() > 16U * 1024U * 1024U) throw std::invalid_argument("image data exceeds 16 MiB");
        const auto width = field("width"), height = field("height");
        auto dimension = [](const Value &v) -> std::uint32_t {
          if (!v.is_integer() || v.as_integer() <= 0 || v.as_integer() > 8192)
            throw std::invalid_argument("image dimensions must be Int in 1..8192");
          return v.as_integer();
        };
        if (format == "gray" || format == "rgb" || format == "rgba") {
          image.width = dimension(width); image.height = dimension(height);
          image.bytes = notebook_pixels_png(image.bytes, image.width, image.height,
              format == "gray" ? 1 : format == "rgb" ? 3 : 4);
        } else if (format == "png" || format == "jpeg" || format == "jpg") {
          image.mime = format == "png" ? "image/png" : "image/jpeg";
          const auto size = notebook_image_dimensions(image.bytes, image.mime);
          image.width = width.is_null() ? size.first : dimension(width);
          image.height = height.is_null() ? size.second : dimension(height);
        } else throw std::invalid_argument("image format must be :png, :jpeg, :gray, :rgb or :rgba");
        notebook_validate_image(image);
        total_bytes += image.storage_bytes();
        if (total_bytes > NotebookLiveStore::kMaxFigureBytes) throw std::invalid_argument("image series exceeds 64 MiB");
        images.push_back(std::move(image));
      }
      const auto cell = notebook_cell_context_.live_cell_id ? notebook_cell_context_.live_cell_id : NotebookLiveScope::current_cell_id();
      const auto generation = notebook_cell_context_.live_generation ? notebook_cell_context_.live_generation : NotebookLiveScope::cell_generation();
      const auto id = string_text_from_value(args[0]).value_or("");
      if (append && !sink->image_channel(id) && notebook_cell_context_.project_images && notebook_cell_context_.project_images->count(id)) {
        const auto &current = notebook_cell_context_.project_images->at(id);
        auto combined = current.images;
        combined.insert(combined.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
        images = std::move(combined);
        if (!caption) caption = current.caption;
        append = false;
      }
      const auto result = sink->publish_images(cell, generation, string_text_from_value(args[0]).value_or(""),
          std::move(images), std::move(caption), append);
      if (result != NotebookLivePublishResult::Accepted) throw std::invalid_argument("image channel exceeds limits or run is closed/stale");
      *out = Value::null(); return SendStatus::Matched;
    } catch (const std::exception &error) {
      set_fault(frame, "NotebookCellError", error.what()); return SendStatus::Faulted;
    }
  }
