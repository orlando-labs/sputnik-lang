(() => {
  const content = document.querySelector("#spec-content");
  const toc = document.querySelector("#spec-toc");
  const search = document.querySelector("#spec-search");
  const count = document.querySelector("#spec-section-count");

  if (!content || !toc) return;

  const isCheatSheet = document.body.classList.contains("cheat-sheet-page");
  const sourcePath = content.dataset.source || "./spec/sputnik_unified_final_spec.md";

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
  };

  const wireTocSearch = () => {
    if (!search) return;
    search.addEventListener("input", () => {
      const query = search.value.trim().toLocaleLowerCase();
      toc.querySelectorAll("a").forEach((link) => {
        link.hidden = Boolean(query) && !link.dataset.tocText.includes(query);
      });
    });
  };

  const wireActiveHeadings = () => {
    const tocLinks = new Map(
      [...toc.querySelectorAll("a")].map((link) => [link.hash.slice(1), link])
    );
    const observer = new IntersectionObserver(
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
      .forEach((heading) => observer.observe(heading));
  };

  const load = async () => {
    try {
      const response = await fetch(sourcePath);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const source = await response.text();
      // The page hero already supplies the cheat sheet's title.
      const markdown = isCheatSheet ? source.replace(/^# [^\n]+\n/, "") : source;
      const rendered = window.SputnikMarkdown.render(markdown, { resolveHref });
      content.innerHTML = rendered.html;
      renderToc(rendered.headings);
      wireTocSearch();
      wireActiveHeadings();
      // A direct section URL must also work after the async Markdown load.
      const anchor = rendered.headings.find((heading) =>
        location.hash === `#${heading.id}` || location.hash === `#${encodeURIComponent(heading.id)}`
      );
      if (anchor) document.getElementById(anchor.id)?.scrollIntoView();
    } catch (error) {
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

  void load();
})();
