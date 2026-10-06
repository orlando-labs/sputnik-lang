(() => {
  const sourcePath = "./spec/amber_unified_final_spec.md";
  const content = document.querySelector("#spec-content");
  const toc = document.querySelector("#spec-toc");
  const search = document.querySelector("#spec-search");
  const count = document.querySelector("#spec-section-count");

  if (!content || !toc) return;

  const escapeHtml = window.AmberMarkdown.escapeHtml;

  // Spec links use plain relative paths into ./spec/; absolute/anchor links pass
  // through untouched.
  const resolveHref = (href) => {
    if (/^(https?:|mailto:|#)/i.test(href)) return href;
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
      const markdown = await response.text();
      const rendered = window.AmberMarkdown.render(markdown, { resolveHref });
      content.innerHTML = rendered.html;
      renderToc(rendered.headings);
      wireTocSearch();
      wireActiveHeadings();
    } catch (error) {
      content.innerHTML = `
        <div class="spec-error">
          <h2>Не удалось загрузить Markdown-копию</h2>
          <p>Откройте сайт через локальный HTTP-сервер или перейдите прямо к исходной копии спеки.</p>
          <a class="button button-primary" href="${sourcePath}">Открыть Markdown</a>
        </div>
      `;
      console.error(error);
    }
  };

  void load();
})();
