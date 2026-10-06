# Notebook result viewer

Cell results have a collapsed, selectable preview (600 characters / six lines),
an expandable native scrolling text view, and a copy action. Arrays, tuples,
sets and maps offer Inspect / Pretty representations. Pretty is generated from
the runtime value, not by attempting to parse an inspection string; quotes,
commas and braces inside strings are preserved.

Secondary actions live in the **…** menu at the upper-right corner: Inspect /
Pretty, Find, and Copy. Search controls are shown only after Find is requested.
Expand / Collapse remains a lightweight inline action only when the collapsed
preview actually hides captured text. This includes the character/newline
budget and six-line wrapping at the current width, measured using the same
SwiftUI text layout as the preview. Resizing, changing representation, and
replacing a result recompute this condition. A runtime truncation flag alone
does not create an Expand button when all captured text is already visible.

Find performs literal substring search over the **entire captured representation**,
including text outside the collapsed preview. It highlights all non-overlapping
matches, emphasizes the active match, shows the count, and wraps next/previous
navigation. Enter moves forward. Matching ignores case unless Aa is selected.
Changing representation or result resets navigation. Selecting/copying text does
not execute code.

The runtime limits each representation while walking the value: 64 KiB of UTF-8,
4,096 visited values, and depth 16. Cycle references are marked. Extremely large
BigInts use a size summary rather than expensive decimal conversion. Truncation
is explicit in both metadata and UI; search/copy covers only captured text, not
omitted values. Expanding changes layout, not this safety budget. The underlying
Amber value remains unchanged. The same bounded formatter protects local-value
inspection; prose interpolation retains raw string contents rather than escapes.

Snapshots include immutable pretty text and independent truncation flags for
Inspect and Pretty. They cross worker protocol v8 / presentation v5 and the
JSON bridge. Failed or cleared evaluations cannot retain an old pretty string
alongside a new error result. No runtime Value or lazy callback reaches the UI.

Checks: `make test-notebook-results`, worker round-trip/end-to-end tests, iamber,
native bridge and model regressions. For an isolated visual fixture, run
`build/notebook_macos_result_tests --interactive`; this does not touch any project.
