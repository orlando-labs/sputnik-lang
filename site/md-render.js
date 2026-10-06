// Shared Markdown renderer for the Sputnik site.
//
// Extracted from the original inline renderer in `spec-page.js` so the spec
// page and the Guide page (`guide-page.js`) share one implementation. The
// renderer is intentionally small and dependency-free (no build step): it
// understands headings, paragraphs, fenced code, tables, ordered/unordered
// lists, blockquotes, admonitions and inline emphasis/code/links.
//
// Guide-specific conventions (both grep-removable when the Guide becomes a
// self-contained doc, see docs/engineering/doc-system.md):
//   * a `spec:` link scheme renders as a distinct "spec reference" chip;
//   * blockquotes whose first line is `[!note] / [!warn] / [!spec] / [!tip]`
//     render as admonition callouts.
//
// Exposes `window.SputnikMarkdown.render(markdown, options) -> { html, headings }`.
// `options.resolveHref(href)` maps non-standard link targets (e.g. `spec:` and
// `guide:` schemes, or relative spec paths); it defaults to identity.
(() => {
  const escapeHtml = (value) =>
    String(value)
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;");

  const stripInline = (value) =>
    value
      .replace(/`([^`]+)`/g, "$1")
      .replace(/\*\*([^*]+)\*\*/g, "$1")
      .replace(/\[([^\]]+)\]\([^)]+\)/g, "$1")
      .replace(/[#*_~]/g, "")
      .trim();

  // Stable, Unicode-aware slug (Cyrillic kept) used for heading anchors.
  const slugify = (heading) =>
    stripInline(heading)
      .toLocaleLowerCase()
      .normalize("NFKD")
      .replace(/[^\p{Letter}\p{Number}\s-]/gu, "")
      .trim()
      .replace(/\s+/g, "-") || "section";

  const makeSlug = (heading, counts) => {
    const base = slugify(heading);
    const next = (counts.get(base) || 0) + 1;
    counts.set(base, next);
    return next === 1 ? base : `${base}-${next}`;
  };

  const identity = (href) => href;

  const inline = (value, resolveHref) => {
    let html = escapeHtml(value);
    html = html.replace(/`([^`]+)`/g, "<code>$1</code>");
    html = html.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    html = html.replace(/\*([^*\n]+)\*/g, "<em>$1</em>");
    html = html.replace(
      /\[([^\]]+)\]\(([^)\s]+)(?:\s+&quot;[^&]*&quot;)?\)/g,
      (_, label, href) => {
        // `spec:` is the isolated convention for references into the formal
        // spec; render it as a chip so it is visually distinct and trivial to
        // strip later.
        if (/^spec:/i.test(href)) {
          const target = escapeHtml(resolveHref(href));
          return `<a class="spec-ref-chip" data-spec-ref href="${target}">${label}</a>`;
        }
        const safeHref = escapeHtml(resolveHref(href));
        return `<a href="${safeHref}">${label}</a>`;
      }
    );
    return html;
  };

  const renderTable = (rows, resolveHref) => {
    const cells = (line) =>
      line
        .trim()
        .replace(/^\|/, "")
        .replace(/\|$/, "")
        .split("|")
        .map((cell) => cell.trim());
    const head = cells(rows[0]);
    const body = rows.slice(2).map(cells);
    return `
      <div class="spec-table-wrap">
        <table>
          <thead><tr>${head
            .map((cell) => `<th>${inline(cell, resolveHref)}</th>`)
            .join("")}</tr></thead>
          <tbody>
            ${body
              .map(
                (row) =>
                  `<tr>${row
                    .map((cell) => `<td>${inline(cell, resolveHref)}</td>`)
                    .join("")}</tr>`
              )
              .join("")}
          </tbody>
        </table>
      </div>
    `;
  };

  const render = (markdown, options = {}) => {
    const resolveHref = options.resolveHref || identity;
    const slugCounts = new Map();
    const lines = markdown.replace(/\r\n?/g, "\n").split("\n");
    const html = [];
    const headings = [];
    let paragraph = [];
    let code = null;
    let table = null;
    let quote = [];
    const listStack = [];

    const closeLists = (targetLevel = -1) => {
      while (
        listStack.length &&
        listStack[listStack.length - 1].level > targetLevel
      ) {
        const current = listStack.pop();
        if (current.openItem) html.push("</li>");
        html.push(`</${current.type}>`);
      }
    };

    const flushParagraph = () => {
      if (!paragraph.length) return;
      html.push(`<p>${inline(paragraph.join(" "), resolveHref)}</p>`);
      paragraph = [];
    };

    const flushQuote = () => {
      if (!quote.length) return;
      const marker = quote[0].match(/^\[!(\w+)\]\s*(.*)$/);
      if (marker) {
        const kind = marker[1].toLowerCase();
        const body = [marker[2], ...quote.slice(1)].filter((l) => l.length);
        html.push(
          `<div class="admonition admonition-${escapeHtml(kind)}">` +
            `<div class="admonition-body">${body
              .map((line) => `<p>${inline(line, resolveHref)}</p>`)
              .join("")}</div></div>`
        );
      } else {
        html.push(
          `<blockquote>${quote
            .map((line) => `<p>${inline(line, resolveHref)}</p>`)
            .join("")}</blockquote>`
        );
      }
      quote = [];
    };

    const flushTable = () => {
      if (!table) return;
      html.push(renderTable(table, resolveHref));
      table = null;
    };

    const flushBlocks = () => {
      flushParagraph();
      flushQuote();
      flushTable();
      closeLists();
    };

    const appendListItem = (indent, marker, text) => {
      flushParagraph();
      flushQuote();
      flushTable();
      const level = Math.floor(indent.replaceAll("\t", "  ").length / 2);
      const type = /^\d/.test(marker) ? "ol" : "ul";

      while (
        listStack.length &&
        listStack[listStack.length - 1].level > level
      ) {
        const current = listStack.pop();
        if (current.openItem) html.push("</li>");
        html.push(`</${current.type}>`);
      }

      const top = listStack[listStack.length - 1];
      if (!top || top.level < level || top.type !== type) {
        html.push(`<${type}>`);
        listStack.push({ level, type, openItem: false });
      } else if (top.openItem) {
        html.push("</li>");
        top.openItem = false;
      }

      const current = listStack[listStack.length - 1];
      html.push(`<li>${inline(text, resolveHref)}`);
      current.openItem = true;
    };

    for (let index = 0; index < lines.length; index += 1) {
      const line = lines[index];
      const fence = line.match(/^ {0,3}(`{3,}|~{3,})(.*)$/);

      if (code) {
        const closes = fence && fence[1][0] === code.marker &&
          fence[1].length >= code.length && !fence[2].trim();
        if (closes) {
          html.push(
            `<pre><code class="language-${escapeHtml(
              code.lang
            )}">${escapeHtml(code.lines.join("\n"))}</code></pre>`
          );
          code = null;
        } else {
          code.lines.push(line);
        }
        continue;
      }

      if (fence) {
        flushBlocks();
        const language = fence[2].trim().match(/^[A-Za-z0-9_+-]+/);
        code = { lang: language ? language[0] : "text", lines: [],
          marker: fence[1][0], length: fence[1].length };
        continue;
      }

      if (!line.trim()) {
        flushParagraph();
        flushQuote();
        flushTable();
        closeLists();
        continue;
      }

      const heading = line.match(/^(#{1,6})\s+(.+?)\s*#*$/);
      if (heading) {
        flushBlocks();
        const level = heading[1].length;
        const text = stripInline(heading[2]);
        const id = makeSlug(heading[2], slugCounts);
        headings.push({ id, level, text });
        html.push(
          `<h${level} id="${id}"><a href="#${id}" aria-hidden="true">#</a>${inline(
            heading[2],
            resolveHref
          )}</h${level}>`
        );
        continue;
      }

      if (/^[-*_]{3,}\s*$/.test(line.trim())) {
        flushBlocks();
        html.push("<hr />");
        continue;
      }

      const listItem = line.match(/^(\s*)([-*+]|\d+[.)])\s+(.+)$/);
      if (listItem) {
        appendListItem(listItem[1], listItem[2], listItem[3]);
        continue;
      }

      const isTableLine = /^\s*\|.+\|\s*$/.test(line);
      const nextIsSeparator =
        index + 1 < lines.length &&
        /^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$/.test(
          lines[index + 1]
        );
      if (isTableLine && (table || nextIsSeparator)) {
        flushParagraph();
        flushQuote();
        closeLists();
        if (!table) table = [];
        table.push(line);
        continue;
      }

      if (table) flushTable();

      const quoteLine = line.match(/^>\s?(.*)$/);
      if (quoteLine) {
        flushParagraph();
        closeLists();
        quote.push(quoteLine[1]);
        continue;
      }

      if (listStack.length && /^\s+/.test(line)) {
        html.push(`<br />${inline(line.trim(), resolveHref)}`);
        continue;
      }

      closeLists();
      paragraph.push(line.trim());
    }

    if (code) {
      html.push(
        `<pre><code class="language-${escapeHtml(code.lang)}">${escapeHtml(
          code.lines.join("\n")
        )}</code></pre>`
      );
    }
    flushBlocks();

    return { html: html.join("\n"), headings };
  };

  window.SputnikMarkdown = { render, slugify, escapeHtml, stripInline };
})();
