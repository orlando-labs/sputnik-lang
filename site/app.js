(() => {
  const translations = {
    ru: {
      nav: {
        features: "Возможности",
        examples: "Примеры",
        technology: "Технологии",
        guide: "Гид",
        cheatsheet: "Шпаргалка",
        modules: "Библиотека",
        spec: "Спека",
        docs: "Документация"
      },
      common: { copy: "Копировать", copied: "Готово" },
      guide: {
        contents: "Содержание",
        pages: "страниц",
        loading: "Загружаю Гид…"
      },
      cheatsheet: {
        eyebrow: "Technical preview · RU",
        title: "Шпаргалка Sputnik",
        lead: "Синтаксис и повседневные рецепты в одном документе: от первых выражений до блоков, коллекций, сопоставления с образцом и задач.",
        read: "Начать с основ",
        source: "Скачать Markdown",
        loading: "Загружаю шпаргалку…"
      },
      hero: {
        eyebrow: "No-GIL VM · native threads · compact syntax",
        title: "Лаконичный язык для <em>no-GIL</em> runtime.",
        lead: "Sputnik берёт читаемость Python, пластичность Ruby, pattern matching и современный data-flow syntax — и ведёт их в VM без глобального lock: worker threads, strands, cooperative tasks, verified bytecode и full-native build pipeline живут в одной модели.",
        ctaPrimary: "Попробовать синтаксис",
        ctaSecondary: "Открыть спеку",
        codeCaption: "Block suffix, safe navigation и real strands — без синтаксического шума"
      },
      features: {
        kicker: "Синтаксис без лишней позы",
        title: "Сахар есть, но он компилируется в понятные формы.",
        lead: "Sputnik не пытается быть «ещё одним Python» или «ещё одним Ruby». Он собирает удачные идеи из разных миров и задаёт им строгую lowering-модель: удобно писать, не стыдно компилировать.",
        pattern: {
          tag: "blocks",
          title: "Block suffix вместо callback-лесов",
          text: "Блок привязывается к вызову как отдельный channel, `_1/_2` работают только внутри implicit-block, а HIR видит уже явную closure."
        },
        safe: {
          tag: "matching",
          title: "Pattern matching и multi-clause def",
          text: "Деструктурирование, guards, pin-pattern и dispatch по клаузам — часть языка, а не протокол поверх `if`."
        },
        collections: {
          tag: "literals",
          title: "Spread и условия в коллекциях",
          text: "`*` и `**` — контекстный spread, conditional entries вычисляются слева направо и не притворяются магическими операторами."
        },
        expressions: {
          tag: "data flow",
          title: "Safe navigation без `nil`-акробатики",
          text: "`.?.` работает для методов, полей, индексов и callable-call; `if`, `unless`, `case` и циклы остаются выражениями."
        },
        callables: {
          tag: "call ABI",
          title: "Callable references без FFI-дыр",
          text: "`&target` создаёт callable object, class object вызывается как constructor, а call/send/block-pass сходятся в единый VM ABI."
        }
      },
      examples: {
        kicker: "Меньше ритуала, больше смысла",
        title: "Sputnik оставляет в коде действие, а не церемонию вокруг него.",
        lead: "Те же идеи, которые в других языках часто распухают в служебные конструкции, в Sputnik остаются рядом с доменной мыслью: поля, цепочки, matching, условные элементы и безопасный доступ.",
        carouselLabel: "Сравнения Sputnik-кода с другими языками",
        dotsLabel: "Слайды с примерами",
        controls: {
          label: "Управление каруселью примеров",
          prev: "Предыдущий пример",
          next: "Следующий пример"
        },
        fields: {
          tag: "constructor",
          title: "Поля конструктора без ручного перекладывания аргументов",
          text: "`@attr` в сигнатуре сразу задаёт поле экземпляра, а class object вызывается как конструктор.",
          sputnikLines: "3 строки",
          otherLines: "6 строк"
        },
        pipeline: {
          tag: "collections",
          title: "Цепочка данных без callback-обвязки",
          text: "Block suffix оставляет фильтрацию, проекцию и дедупликацию в одной читаемой линии потока.",
          sputnikLines: "4 строки",
          otherLines: "13 строк"
        },
        matching: {
          tag: "matching",
          title: "Варианты формы как клаузулы, а не лестница проверок",
          text: "Multi-clause `def` показывает dispatch прямо в объявлении функции и не прячет его в условных ветках.",
          sputnikLines: "3 строки",
          otherLines: "10 строк"
        },
        collections: {
          tag: "literals",
          title: "Условия и spread внутри литералов",
          text: "Коллекция собирается там, где она объявлена: без временных переменных, `push` и мутаций ради формы.",
          sputnikLines: "2 строки",
          otherLines: "6 строк"
        },
        safe: {
          tag: "safe nav",
          title: "Безопасный доступ без защитной пирамиды",
          text: "`.?.` проходит через поля, методы и вызовы, а fallback остаётся обычным выражением.",
          sputnikLines: "1 строка",
          otherLines: "9 строк"
        }
      },
      inside: {
        kicker: "Runtime без глобального замка",
        title: "No-GIL VM: параллельность — часть модели исполнения.",
        lead: "Sputnik строит concurrency вокруг Worker → Strand → Task. `task.async` даёт кооперативные задачи внутри strand, `task.spawn` выносит работу в новый strand на worker thread, а verifier/root maps держат runtime безопасным для байткода и native backend.",
        source: "syntax sugar",
        ir: "explicit lowering",
        bytecode: "verify first",
        runtime: "no-GIL execution",
        cards: {
          verified: {
            title: "Worker / Strand / Task",
            text: "Несколько strands реально исполняются параллельно на worker threads; ordinary mutable state по умолчанию остаётся strand-confined."
          },
          concurrent: {
            title: "Кооперативная многозадачность",
            text: "`wait`, `sleep`, `yield` и cancellation работают на safepoints; `task.sync:` временно запрещает same-strand переключение."
          },
          effects: {
            title: "Verifier, root maps, safepoints",
            text: "`.sputnikbc` — data, not trusted code: loader проверяет структуру, dataflow, handler ranges и GC roots до исполнения."
          },
          frozen: {
            title: "Full native build",
            text: "Native profile сохраняет bytecode как source-of-truth, добавляет MIR/native metadata, root maps и безопасный fallback на VM."
          }
        }
      },
      modules: {
        kicker: "Prelude, а не случайный набор пакетов",
        title: "VM-модули документированы как часть runtime-контракта.",
        lead: "JSON, URL, UUID, crypto-grade randomness, digest, codecs, time, math и CLI parsing доступны как native prelude-типы; каждый метод имеет отдельную страницу и пример Sputnik-кода.",
        cta: "Все модули, методы и примеры"
      },
      closing: {
        kicker: "Спека подключена",
        title: "Если хочется проверить детали — они рядом, не в рекламной дымке.",
        text: "На сайте есть rendered HTML-версия единой Markdown-спеки с боковым оглавлением, anchors и ссылкой на исходную копию.",
        cta: "Читать спеку"
      },
      footer: { text: "No-GIL runtime. Expressive surface. Native path." },
      docs: {
        back: "На главную",
        eyebrow: "Sputnik · standard library",
        title: "Стандартная библиотека Sputnik",
        lead: "Модули на отдельных страницах: сигнатуры, описания и короткие примеры Sputnik-кода. Требования к импорту и доступу указаны на странице модуля.",
        stats: { modules: "native типов", methods: "методов", imports: "обязательных imports" },
        search: "Модуль или метод",
        sidebarNote: "Встроенные типы доступны из prelude; для файлов, процессов и сети нужен импорт.",
        toolbar: " модулей в каталоге",
        open: "Открыть модуль",
        expand: "Развернуть методы",
        collapse: "Свернуть методы",
        methods: "Методы",
        example: "Пример",
        emptyTitle: "Ничего не найдено",
        emptyText: "Попробуйте имя модуля или метода, например Json, parse или sha256."
      },
      spec: {
        eyebrow: "Markdown → HTML · unified spec",
        title: "Единая спецификация Sputnik",
        lead: "Копия `sputnik_unified_final_spec.md`, отрендеренная в HTML: язык, runtime-facing API, VM contracts, build pipeline и implementation blueprint в одном документе.",
        read: "Читать HTML",
        source: "Открыть Markdown-копию",
        toc: "Оглавление",
        sections: "разделов",
        search: "Найти раздел",
        sourceShort: "Исходный Markdown",
        loading: "Рендерю Markdown-спеку…"
      }
    },
    en: {
      nav: {
        features: "Features",
        examples: "Examples",
        technology: "Technology",
        guide: "Guide",
        cheatsheet: "Cheat sheet",
        modules: "Library",
        spec: "Spec",
        docs: "Documentation"
      },
      common: { copy: "Copy", copied: "Copied" },
      guide: {
        contents: "Contents",
        pages: "pages",
        loading: "Loading the Guide…"
      },
      cheatsheet: {
        eyebrow: "Technical preview · EN",
        title: "Sputnik cheat sheet",
        lead: "Syntax and everyday recipes in one document: from your first expressions to blocks, collections, pattern matching and tasks.",
        read: "Start with the basics",
        source: "Download Markdown",
        loading: "Loading the cheat sheet…"
      },
      hero: {
        eyebrow: "No-GIL VM · native threads · compact syntax",
        title: "A compact language for a <em>no-GIL</em> runtime.",
        lead: "Sputnik takes Python readability, Ruby plasticity, pattern matching and modern data-flow syntax, then lowers them into a VM without a global lock: worker threads, strands, cooperative tasks, verified bytecode and a full-native build pipeline share one model.",
        ctaPrimary: "Taste the syntax",
        ctaSecondary: "Open the spec",
        codeCaption: "Block suffix, safe navigation and real strands without syntax noise"
      },
      features: {
        kicker: "Syntax without theatre",
        title: "The sugar is pleasant, but it lowers into explicit forms.",
        lead: "Sputnik is not trying to be “another Python” or “another Ruby”. It takes strong ideas from several worlds and gives them a strict lowering model: nice to write, sane to compile.",
        pattern: {
          tag: "blocks",
          title: "Block suffix instead of callback forests",
          text: "A block travels through a dedicated call channel, `_1/_2` exist only inside implicit blocks, and HIR sees an explicit closure."
        },
        safe: {
          tag: "matching",
          title: "Pattern matching and multi-clause def",
          text: "Destructuring, guards, pin-patterns and clause dispatch are language semantics, not a protocol built on top of `if`."
        },
        collections: {
          tag: "literals",
          title: "Spread and conditions inside collections",
          text: "`*` and `**` are contextual spread markers; conditional entries evaluate left to right and do not pretend to be magical operators."
        },
        expressions: {
          tag: "data flow",
          title: "Safe navigation without nil acrobatics",
          text: "`.?.` works for methods, fields, indexes and callable calls; `if`, `unless`, `case` and loops remain expressions."
        },
        callables: {
          tag: "call ABI",
          title: "Callable references without FFI holes",
          text: "`&target` creates a callable object, class objects can be called as constructors, and call/send/block-pass meet in one VM ABI."
        }
      },
      examples: {
        kicker: "Less ritual, more meaning",
        title: "Sputnik keeps the action in code, not the ceremony around it.",
        lead: "The same ideas that often swell into boilerplate elsewhere stay close to the domain thought in Sputnik: fields, pipelines, matching, conditional entries and safe access.",
        carouselLabel: "Sputnik code comparisons with other languages",
        dotsLabel: "Example slides",
        controls: {
          label: "Example carousel controls",
          prev: "Previous example",
          next: "Next example"
        },
        fields: {
          tag: "constructor",
          title: "Constructor fields without manual argument shuffling",
          text: "`@attr` in a signature assigns an instance field immediately, and a class object can be called as a constructor.",
          sputnikLines: "3 lines",
          otherLines: "6 lines"
        },
        pipeline: {
          tag: "collections",
          title: "Data pipelines without callback wrapping",
          text: "Block suffix keeps filtering, projection and deduplication in one readable flow.",
          sputnikLines: "4 lines",
          otherLines: "13 lines"
        },
        matching: {
          tag: "matching",
          title: "Shape variants as clauses, not a ladder of checks",
          text: "Multi-clause `def` shows dispatch in the function declaration instead of hiding it in conditional branches.",
          sputnikLines: "3 lines",
          otherLines: "10 lines"
        },
        collections: {
          tag: "literals",
          title: "Conditions and spread inside literals",
          text: "The collection is assembled where it is declared, without temporary variables, `push` calls or mutation for ceremony.",
          sputnikLines: "2 lines",
          otherLines: "6 lines"
        },
        safe: {
          tag: "safe nav",
          title: "Safe access without a defensive pyramid",
          text: "`.?.` moves through fields, methods and calls, while the fallback remains an ordinary expression.",
          sputnikLines: "1 line",
          otherLines: "9 lines"
        }
      },
      inside: {
        kicker: "Runtime without a global lock",
        title: "No-GIL VM: parallelism belongs to the execution model.",
        lead: "Sputnik builds concurrency around Worker → Strand → Task. `task.async` creates cooperative work inside a strand, `task.spawn` moves work to a new strand on a worker thread, and verifier/root-map machinery keeps bytecode and native execution honest.",
        source: "syntax sugar",
        ir: "explicit lowering",
        bytecode: "verify first",
        runtime: "no-GIL execution",
        cards: {
          verified: {
            title: "Worker / Strand / Task",
            text: "Multiple strands can run in parallel on worker threads; ordinary mutable state is strand-confined by default."
          },
          concurrent: {
            title: "Cooperative multitasking",
            text: "`wait`, `sleep`, `yield` and cancellation operate at safepoints; `task.sync:` temporarily suppresses same-strand switching."
          },
          effects: {
            title: "Verifier, root maps, safepoints",
            text: "`.sputnikbc` is data, not trusted code: the loader checks structure, dataflow, handler ranges and GC roots before execution."
          },
          frozen: {
            title: "Full native build",
            text: "The native profile keeps bytecode as source of truth while adding MIR/native metadata, root maps and safe fallback into the VM."
          }
        }
      },
      modules: {
        kicker: "Prelude, not a random package pile",
        title: "VM modules are documented as part of the runtime contract.",
        lead: "JSON, URL, UUID, crypto-grade randomness, digests, codecs, time, math and CLI parsing ship as native prelude types; every method has its own page and Sputnik example.",
        cta: "All modules, methods and examples"
      },
      closing: {
        kicker: "The spec is connected",
        title: "If you want the details, they are nearby — not hidden behind brochure fog.",
        text: "The site now includes a rendered HTML version of the unified Markdown spec with a sidebar table of contents, anchors and a link to the copied source.",
        cta: "Read the spec"
      },
      footer: { text: "No-GIL runtime. Expressive surface. Native path." },
      docs: {
        back: "Back home",
        eyebrow: "Sputnik · standard library",
        title: "Sputnik standard library",
        lead: "Module pages include signatures, descriptions and concise Sputnik examples. Each page explains its import and access requirements.",
        stats: { modules: "native types", methods: "methods", imports: "required imports" },
        search: "Module or method",
        sidebarNote: "Built-in types are available from the prelude; files, processes and networking require imports.",
        toolbar: " modules in the catalog",
        open: "Open module",
        expand: "Expand methods",
        collapse: "Collapse methods",
        methods: "Methods",
        example: "Example",
        emptyTitle: "Nothing found",
        emptyText: "Try a module or method name such as Json, parse or sha256."
      },
      spec: {
        eyebrow: "Markdown → HTML · unified spec",
        title: "Unified Sputnik specification",
        lead: "A copied `sputnik_unified_final_spec.md` rendered as HTML: language semantics, runtime-facing API, VM contracts, build pipeline and implementation blueprint in one document.",
        read: "Read HTML",
        source: "Open Markdown copy",
        toc: "Contents",
        sections: "sections",
        search: "Find a section",
        sourceShort: "Markdown source",
        loading: "Rendering the Markdown spec…"
      }
    }
  };

  const state = {
    lang: localStorage.getItem("sputnik-lang") || (navigator.language.startsWith("ru") ? "ru" : "en"),
    allExpanded: false,
    openModules: new Set()
  };

  const getTranslation = (path) =>
    path.split(".").reduce((value, key) => value && value[key], translations[state.lang]);

  const applyTranslations = () => {
    document.documentElement.lang = state.lang;
    document.querySelectorAll("[data-ru][data-en]").forEach(node => { node.textContent = node.dataset[state.lang]; });
    document.querySelectorAll("[data-i18n]").forEach((node) => {
      const value = getTranslation(node.dataset.i18n);
      if (value == null) return;
      if (value.includes("<em>")) node.innerHTML = value;
      else node.textContent = value;
    });
    document.querySelectorAll("[data-i18n-placeholder]").forEach((node) => {
      const value = getTranslation(node.dataset.i18nPlaceholder);
      if (value != null) node.placeholder = value;
    });
    document.querySelectorAll("[data-i18n-aria-label]").forEach((node) => {
      const value = getTranslation(node.dataset.i18nAriaLabel);
      if (value != null) node.setAttribute("aria-label", value);
    });
    document.querySelectorAll("[data-lang]").forEach((button) => {
      button.setAttribute("aria-pressed", String(button.dataset.lang === state.lang));
    });
    if (document.body.classList.contains("module-page")) {
      window.renderSputnikModulePage?.(state.lang);
    } else if (document.body.classList.contains("cheat-sheet-page")) {
      document.title = state.lang === "ru" ? "Sputnik — шпаргалка" : "Sputnik — cheat sheet";
      window.renderSputnikCheatSheetPage?.(state.lang);
    } else if (document.body.classList.contains("guide-page")) {
      window.renderSputnikGuidePage?.(state.lang);
    } else {
      document.title = document.body.classList.contains("spec-page")
        ? (state.lang === "ru" ? "Sputnik — спецификация" : "Sputnik — specification")
        : document.body.classList.contains("docs-page")
        ? (state.lang === "ru" ? "Sputnik — модули VM" : "Sputnik — VM modules")
        : (state.lang === "ru" ? "Sputnik — Technical Preview" : "Sputnik — Technical Preview");
    }
  };

  const methodCountLabel = (count) => {
    if (state.lang === "en") return `${count} ${count === 1 ? "method" : "methods"}`;
    const mod10 = count % 10;
    const mod100 = count % 100;
    const noun = mod10 === 1 && mod100 !== 11
      ? "метод"
      : mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)
        ? "метода"
        : "методов";
    return `${count} ${noun}`;
  };

  const renderModules = () => {
    const list = document.querySelector("#module-list");
    if (!list || !window.SPUTNIK_MODULES) return;
    const query = (document.querySelector("#module-search")?.value || "").trim().toLocaleLowerCase();
    const modules = window.SPUTNIK_MODULES.filter((module) => {
      const haystack = [
        module.title,
        module.description.ru,
        module.description.en,
        ...module.methods.flatMap((method) => [method.sig, method.ru, method.en])
      ].join(" ").toLocaleLowerCase();
      return haystack.includes(query);
    });

    list.innerHTML = modules.map((module) => `
      <a class="module-index-card" href="./modules/${module.path}">
        <div class="module-icon">${module.icon}</div>
        <div class="module-index-copy">
          <h2>${module.title}</h2>
          <p>${module.description[state.lang]}</p>
        </div>
        <div class="module-index-meta">
          <span>${methodCountLabel(module.methods.length)}</span>
          <b>${translations[state.lang].docs.open} →</b>
        </div>
      </a>
    `).join("");

    const resultCount = document.querySelector("#result-count");
    if (resultCount) resultCount.textContent = String(modules.length);
    const empty = document.querySelector("#empty-state");
    if (empty) empty.hidden = modules.length !== 0;
  };

  document.querySelectorAll("[data-lang]").forEach((button) => {
    button.addEventListener("click", () => {
      state.lang = button.dataset.lang;
      localStorage.setItem("sputnik-lang", state.lang);
      applyTranslations();
      window.updateSputnikExampleCarouselLabels?.();
      renderModules();
    });
  });

  document.querySelectorAll("[data-copy-target]").forEach((button) => {
    button.addEventListener("click", async () => {
      const target = document.getElementById(button.dataset.copyTarget);
      if (!target) return;
      await navigator.clipboard.writeText(target.textContent);
      const label = button.querySelector("[data-i18n]");
      if (!label) return;
      label.textContent = translations[state.lang].common.copied;
      window.setTimeout(() => { label.textContent = translations[state.lang].common.copy; }, 1200);
    });
  });

  const initExampleCarousel = () => {
    const section = document.querySelector("#examples");
    const root = document.querySelector("[data-example-carousel]");
    if (!section || !root) return;

    const track = root.querySelector("[data-carousel-track]");
    const slides = [...root.querySelectorAll("[data-carousel-slide]")];
    const prev = section.querySelector("[data-carousel-prev]");
    const next = section.querySelector("[data-carousel-next]");
    const dots = [...section.querySelectorAll("[data-carousel-dot]")];
    const current = section.querySelector("[data-carousel-current]");
    let index = 0;
    let dragStart = null;

    const setSlide = (nextIndex) => {
      if (!slides.length) return;
      index = (nextIndex + slides.length) % slides.length;
      track.style.transform = `translateX(-${index * 100}%)`;
      slides.forEach((slide, slideIndex) => {
        slide.setAttribute("aria-hidden", String(slideIndex !== index));
      });
      dots.forEach((dot, dotIndex) => {
        const active = dotIndex === index;
        if (active) dot.setAttribute("aria-current", "true");
        else dot.removeAttribute("aria-current");
        dot.setAttribute("aria-label", `${state.lang === "ru" ? "Пример" : "Example"} ${dotIndex + 1}`);
      });
      if (current) current.textContent = String(index + 1).padStart(2, "0");
    };

    prev?.addEventListener("click", () => setSlide(index - 1));
    next?.addEventListener("click", () => setSlide(index + 1));
    dots.forEach((dot) => {
      dot.addEventListener("click", () => setSlide(Number(dot.dataset.carouselDot)));
    });
    root.addEventListener("keydown", (event) => {
      if (event.key === "ArrowLeft") setSlide(index - 1);
      if (event.key === "ArrowRight") setSlide(index + 1);
    });
    root.addEventListener("pointerdown", (event) => {
      dragStart = event.clientX;
    });
    root.addEventListener("pointerup", (event) => {
      if (dragStart == null) return;
      const delta = event.clientX - dragStart;
      dragStart = null;
      if (Math.abs(delta) < 45) return;
      setSlide(index + (delta < 0 ? 1 : -1));
    });

    window.updateSputnikExampleCarouselLabels = () => setSlide(index);
    setSlide(0);
  };

  const search = document.querySelector("#module-search");
  if (search) {
    search.addEventListener("input", renderModules);
    document.addEventListener("keydown", (event) => {
      if (event.key === "/" && document.activeElement !== search) {
        event.preventDefault();
        search.focus();
      }
    });
  }

  const revealObserver = new IntersectionObserver((entries) => {
    entries.forEach((entry) => {
      if (entry.isIntersecting) {
        entry.target.classList.add("is-visible");
        revealObserver.unobserve(entry.target);
      }
    });
  }, { threshold: 0.12 });
  document.querySelectorAll(".reveal").forEach((node) => revealObserver.observe(node));

  applyTranslations();
  initExampleCarousel();
  renderModules();
  window.SputnikSyntax.highlightAll();
})();
