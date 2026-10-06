// Guide renderer. Loads the knowledge-graph manifest, builds the category
// sidebar and the prerequisite/related rail, and renders the active node's
// Markdown (per current language) through the shared `SputnikMarkdown` renderer.
//
// Routing is hash-based: `learn.html#<node-id>` selects a node. Language is
// shared with the rest of the site through the `sputnik-lang` localStorage key;
// `app.js` calls `window.renderSputnikGuidePage(lang)` when the RU/EN switch is
// toggled.
(() => {
  const MANIFEST_URL = "./guide/guide-manifest.json";
  const nav = document.querySelector("#guide-nav");
  const content = document.querySelector("#guide-content");
  const rail = document.querySelector("#guide-rail");
  const nodeCount = document.querySelector("#guide-node-count");
  if (!nav || !content || !rail) return;

  const labels = {
    ru: {
      prerequisites: "Предпосылки",
      related: "Связанные темы",
      specRefs: "Ссылки на спеку",
      previous: "Предыдущая",
      next: "Следующая",
      missing: "Страница не найдена",
      failed: "Не удалось загрузить Гид",
      hint: "Откройте сайт через локальный HTTP-сервер.",
      status: { draft: "черновик", review: "ревью", stable: "стабильно" }
    },
    en: {
      prerequisites: "Prerequisites",
      related: "Related",
      specRefs: "Spec references",
      previous: "Previous",
      next: "Next",
      missing: "Page not found",
      failed: "Could not load the Guide",
      hint: "Open the site through a local HTTP server.",
      status: { draft: "draft", review: "review", stable: "stable" }
    }
  };

  const escapeHtml = window.SputnikMarkdown.escapeHtml;

  let manifest = null;
  const cache = new Map();

  const currentLang = () =>
    localStorage.getItem("sputnik-lang") ||
    (navigator.language.startsWith("ru") ? "ru" : "en");

  const nodeById = (id) => manifest.nodes.find((node) => node.id === id);

  const pickLang = (map, lang) =>
    map[lang] || map.ru || map.en || Object.values(map)[0];

  // The two grep-removable spec-coupling conventions live here: the `spec:`
  // link scheme points into the rendered spec page, `guide:` stays in-graph.
  const resolveHref = (href) => {
    if (/^spec:/i.test(href)) return `./spec.html#${href.slice(5)}`;
    if (/^guide:/i.test(href)) return `#${href.slice(6)}`;
    return href;
  };

  const currentNodeId = () => {
    const hash = decodeURIComponent(location.hash.replace(/^#/, ""));
    if (hash && nodeById(hash)) return hash;
    return manifest.nodes[0]?.id;
  };

  const titleOf = (node, lang) => pickLang(node.titles, lang);

  const renderNav = (lang, activeId) => {
    nav.innerHTML = manifest.categories
      .map((cat) => {
        const nodes = manifest.nodes.filter((node) => node.category === cat.id);
        if (!nodes.length) return "";
        return `
          <div class="guide-nav-group">
            <p class="guide-nav-cat">${escapeHtml(pickLang(cat.title, lang))}</p>
            ${nodes
              .map(
                (node) => `
              <a href="#${node.id}" class="${node.id === activeId ? "active" : ""}">
                <span>${escapeHtml(titleOf(node, lang))}</span>
                ${
                  node.status && node.status !== "stable"
                    ? `<em class="guide-status guide-status-${node.status}">${
                        labels[lang].status[node.status] || node.status
                      }</em>`
                    : ""
                }
              </a>
            `
              )
              .join("")}
          </div>
        `;
      })
      .join("");
    if (nodeCount) nodeCount.textContent = String(manifest.nodes.length);
  };

  const renderRail = (node, lang) => {
    const L = labels[lang];
    const linkList = (ids) =>
      ids
        .map((id) => {
          const target = nodeById(id);
          if (!target) return "";
          const summary = pickLang(target.summaries, lang) || "";
          return `<a href="#${target.id}"><span>${escapeHtml(
            titleOf(target, lang)
          )}</span>${
            summary ? `<small>${escapeHtml(summary)}</small>` : ""
          }</a>`;
        })
        .join("");

    const sections = [];
    if (node.prerequisites.length) {
      sections.push(
        `<section class="guide-rail-section"><h3>${L.prerequisites}</h3><div class="guide-rail-links">${linkList(
          node.prerequisites
        )}</div></section>`
      );
    }
    if (node.related.length) {
      sections.push(
        `<section class="guide-rail-section"><h3>${L.related}</h3><div class="guide-rail-links">${linkList(
          node.related
        )}</div></section>`
      );
    }
    // Spec references are gated by one manifest flag so they can be hidden
    // site-wide when the Guide becomes self-contained.
    if (manifest.show_spec_refs && node.spec_refs.length) {
      sections.push(
        `<section class="guide-rail-section guide-spec-refs"><h3>${
          L.specRefs
        }</h3><div class="guide-rail-links">${node.spec_refs
          .map(
            (ref) =>
              `<a class="spec-ref-chip" href="./spec.html#${escapeHtml(
                ref.anchor
              )}">${escapeHtml(ref.label)}</a>`
          )
          .join("")}</div></section>`
      );
    }
    rail.innerHTML = sections.join("");
    rail.hidden = sections.length === 0;
  };

  const renderPagination = (node, lang) => {
    const index = manifest.nodes.findIndex((item) => item.id === node.id);
    const prev = index > 0 ? manifest.nodes[index - 1] : null;
    const next =
      index < manifest.nodes.length - 1 ? manifest.nodes[index + 1] : null;
    const L = labels[lang];
    return `<nav class="module-pagination guide-pagination" aria-label="Adjacent pages">
      ${
        prev
          ? `<a href="#${prev.id}"><small>← ${L.previous}</small><b>${escapeHtml(
              titleOf(prev, lang)
            )}</b></a>`
          : "<span></span>"
      }
      ${
        next
          ? `<a href="#${next.id}"><small>${L.next} →</small><b>${escapeHtml(
              titleOf(next, lang)
            )}</b></a>`
          : "<span></span>"
      }
    </nav>`;
  };

  const fetchMarkdown = async (path) => {
    if (cache.has(path)) return cache.get(path);
    const response = await fetch(`./${path}`);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const text = await response.text();
    cache.set(path, text);
    return text;
  };

  // Drop the frontmatter block (already captured in the manifest) before the
  // body is rendered.
  const stripFrontmatter = (markdown) => {
    if (!markdown.startsWith("---")) return markdown;
    const close = markdown.indexOf("\n---", 3);
    if (close === -1) return markdown;
    const lineEnd = markdown.indexOf("\n", close + 1);
    return lineEnd === -1 ? "" : markdown.slice(lineEnd + 1);
  };

  const render = async (lang = currentLang()) => {
    if (!manifest) return;
    const node = nodeById(currentNodeId());
    if (!node) {
      content.innerHTML = `<h1>${labels[lang].missing}</h1>`;
      return;
    }
    document.title = `Sputnik · ${titleOf(node, lang)}`;
    renderNav(lang, node.id);
    renderRail(node, lang);
    const path = pickLang(node.paths, lang);
    try {
      const markdown = stripFrontmatter(await fetchMarkdown(path));
      const rendered = window.SputnikMarkdown.render(markdown, { resolveHref });
      content.innerHTML = rendered.html + renderPagination(node, lang);
      window.scrollTo(0, 0);
    } catch (error) {
      content.innerHTML = `<div class="spec-error"><h2>${labels[lang].missing}</h2></div>`;
      console.error(error);
    }
  };

  window.renderSputnikGuidePage = render;
  window.addEventListener("hashchange", () => void render());

  const load = async () => {
    try {
      const response = await fetch(MANIFEST_URL);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      manifest = await response.json();
      await render();
    } catch (error) {
      const L = labels[currentLang()];
      content.innerHTML = `<div class="spec-error"><h2>${L.failed}</h2><p>${L.hint}</p></div>`;
      console.error(error);
    }
  };

  void load();
})();
