(() => {
  const content = document.querySelector("#spec-content");
  const toc = document.querySelector("#spec-toc");
  const search = document.querySelector("#spec-search");
  const count = document.querySelector("#spec-section-count");

  if (!content || !toc) return;

  const isCheatSheet = document.body.classList.contains("cheat-sheet-page");
  const defaultSource = content.dataset.source || "./spec/sputnik_unified_final_spec.md";
  const sources = {en: defaultSource, ru: content.dataset.sourceRu || defaultSource};
  const cache = new Map();
  let renderRevision = 0;
  let headingObserver;
  // Keep existing deep links and the selected section across RU/EN switches.
  const cheatSheetAnchors = [
    "1-essentials", "2-collections-ranges-and-absence", "3-blocks-and-chains-spaces-matter",
    "4-functions-types-and-callable-values", "5-classes-properties-and-composition",
    "6-pattern-matching-and-multi-clause-functions", "7-conditions-and-control-flow",
    "8-everyday-collection-recipes", "9-errors-cleanup-and-explicit-results",
    "10-concurrency-and-output", "11-modules", "12-optional-profiles-and-reference-additions"
  ];
  const headingId = (text, level) => {
    const number = text.match(/^(\d+)\./);
    return level === 2 && number ? cheatSheetAnchors[Number(number[1]) - 1] : undefined;
  };

  const escapeHtml = window.SputnikMarkdown.escapeHtml;

  // The cheat sheet keeps the canonical document's repository-relative links.
  // Specs already published on the site stay local; other references open GitHub.
  const resolveHref = (href) => {
    if (/^(https?:|mailto:|#)/i.test(href)) return href;
    if (isCheatSheet) {
      const [path, anchor] = href.split("#");
      const localSpecs = {
        "../sputnik_unified_final_spec.md": "./spec.html",
        "../sputnik_runtime_project_design.md": "./spec/sputnik_runtime_project_design.md"
      };
      const target = localSpecs[path] || new URL(path, "https://github.com/orlando-labs/amber-lang/blob/main/docs/").href;
      return target + (anchor ? `#${anchor}` : "");
    }
    if (href.startsWith("./") || href.startsWith("../")) return href;
    return `./spec/${href}`;
  };

  const renderToc = (headings) => {
    const visibleHeadings = headings.filter((heading) => heading.level <= 3);
    toc.innerHTML = visibleHeadings
      .map(
        (heading) => `
        <a href="#${heading.id}" class="toc-level-${heading.level}" data-toc-text="${escapeHtml(
          heading.text.toLocaleLowerCase()
        )}">
          <span>${escapeHtml(heading.text)}</span>
        </a>
      `
      )
      .join("");
    if (count) count.textContent = String(visibleHeadings.length);
    filterToc();
  };

  const filterToc = () => {
    const query = (search?.value || "").trim().toLocaleLowerCase();
    toc.querySelectorAll("a").forEach((link) => {
      link.hidden = Boolean(query) && !link.dataset.tocText.includes(query);
    });
  };
  search?.addEventListener("input", filterToc);

  const wireActiveHeadings = () => {
    const tocLinks = new Map(
      [...toc.querySelectorAll("a")].map((link) => [link.hash.slice(1), link])
    );
    headingObserver?.disconnect();
    headingObserver = new IntersectionObserver(
      (entries) => {
        const visible = entries
          .filter((entry) => entry.isIntersecting)
          .sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top)[0];
        if (!visible) return;
        tocLinks.forEach((link) => link.classList.remove("active"));
        tocLinks.get(visible.target.id)?.classList.add("active");
      },
      { rootMargin: "-18% 0px -72% 0px", threshold: 0 }
    );

    content
      .querySelectorAll("h1[id], h2[id], h3[id]")
      .forEach((heading) => headingObserver.observe(heading));
  };

  const fetchMarkdown = (path) => {
    if (!cache.has(path)) {
      cache.set(path, fetch(path).then((response) => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.text();
      }).catch((error) => { cache.delete(path); throw error; }));
    }
    return cache.get(path);
  };

  const load = async (lang = document.documentElement.lang) => {
    const revision = ++renderRevision;
    const sourcePath = isCheatSheet ? sources[lang] || sources.en : defaultSource;
    content.setAttribute("aria-busy", "true");
    if (isCheatSheet) {
      document.querySelectorAll("[data-cheat-sheet-source]").forEach((link) => { link.href = sourcePath; });
      if (search && content.lang !== lang) search.value = "";
    }
    try {
      const source = await fetchMarkdown(sourcePath);
      // A slower fetch for an earlier language must not replace the current one.
      if (revision !== renderRevision) return;
      // The page hero already supplies the cheat sheet's title.
      const markdown = isCheatSheet ? source.replace(/^# [^\n]+\n/, "") : source;
      const rendered = window.SputnikMarkdown.render(markdown, {resolveHref, headingId: isCheatSheet ? headingId : undefined});
      content.innerHTML = rendered.html;
      content.setAttribute("aria-busy", "false");
      if (isCheatSheet) {
        content.lang = lang;
        toc.lang = lang;
        content.dataset.renderedLanguage = lang;
      }
      renderToc(rendered.headings);
      wireActiveHeadings();
      // A direct section URL must also work after the async Markdown load.
      const anchor = rendered.headings.find((heading) =>
        location.hash === `#${heading.id}` || location.hash === `#${encodeURIComponent(heading.id)}`
      );
      if (anchor) document.getElementById(anchor.id)?.scrollIntoView();
    } catch (error) {
      if (revision !== renderRevision) return;
      content.setAttribute("aria-busy", "false");
      const isEnglish = document.documentElement.lang === "en";
      content.innerHTML = `
        <div class="spec-error">
          <h2 data-ru="Не удалось загрузить документ" data-en="Could not load the document">${isEnglish ? "Could not load the document" : "Не удалось загрузить документ"}</h2>
          <p data-ru="Откройте исходный Markdown по ссылке ниже." data-en="Open the Markdown source below.">${isEnglish ? "Open the Markdown source below." : "Откройте исходный Markdown по ссылке ниже."}</p>
          <a class="button button-primary" href="${sourcePath}">Markdown ↗</a>
        </div>
      `;
      console.error(error);
    }
  };

  if (isCheatSheet) window.renderSputnikCheatSheetPage = load;
  void load();
})();
