(() => {
  document.body.innerHTML = `
    <header class="site-header">
      <a class="brand" href="../index.html" aria-label="Sputnik home">
        <span class="brand-gem"><img src="../assets/sputnik-mark.png" alt="" /></span>
        <span>Sputnik</span>
      </a>
      <nav class="main-nav" aria-label="Primary navigation">
        <a href="../index.html#features" data-i18n="nav.features">Возможности</a>
        <a href="../index.html#inside" data-i18n="nav.technology">Технологии</a>
        <a href="../learn.html" data-i18n="nav.guide">Гид</a>
        <a href="../modules.html" class="active" data-i18n="nav.modules">Модули VM</a>
        <a href="../spec.html" data-i18n="nav.spec">Спека</a>
      </nav>
      <div class="header-actions">
        <div class="language-switch" role="group" aria-label="Language">
          <button type="button" data-lang="ru" aria-pressed="true">RU</button>
          <button type="button" data-lang="en" aria-pressed="false">EN</button>
        </div>
        <a class="header-cta" href="../spec.html">
          <span data-i18n="nav.spec">Спека</span>
          <span aria-hidden="true">↗</span>
        </a>
      </div>
    </header>
    <main id="module-page-root" class="module-page-root"></main>
    <footer>
      <a class="brand" href="../index.html">
        <span class="brand-gem"><img src="../assets/sputnik-mark.png" alt="" /></span>
        <span>Sputnik</span>
      </a>
      <p data-i18n="footer.text">Expressive language. Verified machine.</p>
      <span>runtime/stdlib_registry.cpp</span>
    </footer>
  `;

  const labels = {
    ru: {
      back: "Все модули",
      methods: "Методы",
      copy: "Копировать",
      copied: "Готово",
      previous: "Предыдущий",
      next: "Следующий",
      missing: "Модуль не найден"
    },
    en: {
      back: "All modules",
      methods: "Methods",
      copy: "Copy",
      copied: "Copied",
      previous: "Previous",
      next: "Next",
      missing: "Module not found"
    }
  };

  const highlightSputnik = (source) => {
    const escaped = source
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;");
    return escaped
      .replace(/("(?:\\.|[^"\\])*")/g, '<span class="tok-str">$1</span>')
      .replace(/\b(def|case|when|else|if|unless|true|false|null|as|or|and|rescue)\b/g, '<span class="tok-key">$1</span>')
      .replace(/\b(\d[\d_]*)\b/g, '<span class="tok-num">$1</span>');
  };

  const copyText = async (text) => {
    if (navigator.clipboard?.writeText) {
      try {
        await navigator.clipboard.writeText(text);
        return;
      } catch (_) {
        // Fall through for local/file previews where Clipboard API is denied.
      }
    }
    const field = document.createElement("textarea");
    field.value = text;
    field.style.position = "fixed";
    field.style.opacity = "0";
    document.body.appendChild(field);
    field.select();
    document.execCommand("copy");
    field.remove();
  };

  const render = (lang = "ru") => {
    const root = document.querySelector("#module-page-root");
    const id = document.body.dataset.module;
    const modules = window.SPUTNIK_MODULES || [];
    const index = modules.findIndex((item) => item.id === id);
    const module = modules[index];
    if (!root || !module) {
      if (root) root.innerHTML = `<h1>${labels[lang].missing}</h1>`;
      return;
    }

    const previous = modules[(index - 1 + modules.length) % modules.length];
    const next = modules[(index + 1) % modules.length];
    document.title = `Sputnik · ${module.title}`;

    root.innerHTML = `
      <aside class="module-page-sidebar">
        <a class="module-back" href="../modules.html">← ${labels[lang].back}</a>
        <nav aria-label="VM modules">
          ${modules.map((item) => `
            <a href="./${item.path}" class="${item.id === module.id ? "active" : ""}">
              <span>${item.icon}</span>${item.title}
            </a>
          `).join("")}
        </nav>
      </aside>

      <article class="module-reference">
        <header class="module-reference-head">
          <div class="module-icon module-icon-large">${module.icon}</div>
          <div>
            <p>Sputnik VM · prelude</p>
            <h1>${module.title}</h1>
            <div class="module-reference-description">${module.description[lang]}</div>
          </div>
        </header>

        <div class="module-note">${module.note[lang]}</div>

        <section class="method-reference">
          <h2>${labels[lang].methods}</h2>
          <div class="method-reference-list">
            ${module.methods.map((item, methodIndex) => `
              <section class="method-reference-card" id="method-${methodIndex + 1}">
                <div class="method-reference-copy">
                  <code>${item.sig}</code>
                  <p>${item[lang]}</p>
                </div>
                <div class="method-code">
                  <button type="button" data-copy-method="${methodIndex}">
                    ${labels[lang].copy}
                  </button>
                  <pre><code>${highlightSputnik(item.example)}</code></pre>
                </div>
              </section>
            `).join("")}
          </div>
        </section>

        <nav class="module-pagination" aria-label="Adjacent modules">
          <a href="./${previous.path}">
            <small>← ${labels[lang].previous}</small>
            <b>${previous.title}</b>
          </a>
          <a href="./${next.path}">
            <small>${labels[lang].next} →</small>
            <b>${next.title}</b>
          </a>
        </nav>
      </article>
    `;

    root.querySelectorAll("[data-copy-method]").forEach((button) => {
      button.addEventListener("click", () => {
        const item = module.methods[Number(button.dataset.copyMethod)];
        button.textContent = labels[lang].copied;
        void copyText(item.example);
        window.setTimeout(() => { button.textContent = labels[lang].copy; }, 1200);
      });
    });
  };

  window.renderSputnikModulePage = render;
})();
