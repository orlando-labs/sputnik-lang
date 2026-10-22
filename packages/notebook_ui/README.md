# Notebook UI channels

`DataTableView` separates tabular data from publication and supports text exports:

```sputnik
from notebook.ui import DataTableView

view = DataTableView([{name: "Alpha", count: 3}], columns: [:name, :count])
notebook.show(view, id: "items", caption: "Items")
csv = view.to_csv()
markdown = view.to_markdown()
42
```

Constructing/exporting works outside a notebook. `notebook.show` copies a bounded,
typed snapshot to the active cell; tables appear on the sheet and in Data Table
board components. Their menus copy/save CSV and Markdown for visible rows.
See the [DataTableView API](../../../sputnik-notebook/docs/engineering/notebook-data-table-view.md)
for data shapes, columns, live replacement, limits and export scope.

Board components declare channel IDs and layout. Sputnik code publishes data:

```sputnik
from notebook.ui import publish_point, image, publish_image, publish_images, add_image, clear_images, read_image, read_images

publish_point("loss", step, value)
publish_image("photo", image(fs.read_bytes("photo.png"), caption: "A frame"))
publish_images("gallery", [image([0, 80, 160, 255], width: 2, height: 2,
  format: :gray, caption: "Grayscale")], caption: "The whole series")
add_image("gallery", image(fs.read_bytes("photo.jpg"), format: :jpeg, caption: "Another frame"))
clear_images("gallery")
```

Images accept PNG/JPEG bytes (dimensions inferred), or raw `:gray`, `:rgb`,
`:rgba` bytes/arrays of Int samples in 0..255 with explicit dimensions.
Publish replaces a channel; add appends and preserves the series caption
unless `caption:` is supplied. Caption on `image` belongs to that frame.
Publication requires an active notebook run and produces runtime output.

Image/Image Carousel settings independently enable file saving, importing and
editing, and sheet reads. Imported/edited collections persist in the project.
With reading enabled, `read_image("gallery")` returns the selected descriptor
(or null for an empty channel), `read_image("gallery", index: 0)` selects an
explicit zero-based index, and `read_images("gallery")` returns the ordered
Array. Descriptors include `data` (encoded Bytes), `width`, `height`, `format`,
`caption`, `series_caption`, and `index`, and can be published again directly.

See the [native notebook image API](../../../sputnik-notebook/docs/engineering/notebook-board-images.md)
for sizing, lifecycle, limits and the MNIST example.
