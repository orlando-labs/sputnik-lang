# План ускорения Sputnik VM и native — 14 октября 2026

Реализация начата в запрошенном порядке: эквивалентные бенчмарки и фиксация
инструментов → контрольный перезамер → cancellation fast path и native scalar
safepoints. [Результаты и выполненные проверки](bench/polyglot/RESULTS_RECOVERY_2026-10-14.md).
Первый native debt-counter experiment отвергнут по замерам; итоговая правка
использует conservative scalar effect proof. Остальные P1/P2 пункты остаются
следующими этапами. Outside-body timer отделён от body, но ещё не раскладывает
imports/module init/exit на отдельные фазы.

Замеры показывают отдельную регрессию скалярной арифметики и рост времени некоторых коротких native-программ. Они не показывают общего провала native: raw HTTP почти сохранил августовскую скорость, Orbit стал быстрее. Откат всего набора Sputniktorch-изменений не обоснован. Первые изменения должны устранить дорогой путь проверки отмены в VM и удешевить native safepoints, сохранив их семантику.

Текущая точка: `b93d2d74d8b92a6748db766ef3b3f060d45775dd` плюс локальные изменения. Исходники и рабочие бинарники во время исследования не менялись. Основной [перезамер](bench/polyglot/RESULTS_2026-10-13.md) использует Apple M4, Apple Clang 21, O2, tagged Value, system allocator, без PGO. Прототипы исследования находятся в `bench/polyglot/build/analysis-2026-10-14`; они не установлены вместо рабочей VM.

Для исторического сравнения использована последняя статистическая таблица `d5ca4c7` от 22 августа из [handoff](bench/polyglot/OPTIMIZATION_BASELINE_2026-08-20.md), а не июльский best-of-10. Августовские сырые JSON в текущем checkout отсутствуют. Поэтому исторические проценты ниже — сигнал для расследования, а не причинный A/B: менялись исходники, зависимости, ОС, способы сборки и HTTP-клиент.

| Нагрузка | VM август, мс | VM октябрь, мс | Δ времени VM | Native август, мс | Native октябрь, мс | Δ времени native |
|---|---:|---:|---:|---:|---:|---:|
| arithmetic | 162.432 | 265.670 | +63.6% | 5.550 | 8.647 | +55.8% |
| calls-collections | 12.968 | 14.009 | +8.0% | 8.305 | 8.713 | +4.9% |
| sha-digest | 39.723 | 38.496 | -3.1% | 13.454 | 15.776 | +17.3% |
| json | 31.118 | 27.819 | -10.6% | 14.971 | 15.370 | +2.7% |
| string-ops | 23.181 | 24.181 | +4.3% | 7.481 | 8.148 | +8.9% |
| map-words | 24.102 | 26.076 | +8.2% | 7.957 | 9.563 | +20.2% |
| codecs | 34.635 | 33.036 | -4.6% | 8.448 | 10.177 | +20.5% |
| secure-random | 20.884 | 20.440 | -2.1% | 8.375 | 9.960 | +18.9% |
| time-flow | 93.758 | 89.306 | -4.7% | 9.937 | 11.695 | +17.7% |
| uuid | 30.988 | 29.949 | -3.4% | 10.437 | 12.117 | +16.1% |

Для HTTP августовская последняя полная `watch-fast` матрица даёт raw VM 22 281 RPS, raw native 33 727, Orbit VM 1 972, Orbit native 6 183. Текущие значения — 23 848 / 33 488 / 2 477 / 7 582. Raw native отличается примерно на −0,7%, Orbit native — +22,6%. Клиент и версии Orbit/ORM/SQLite изменились; эти числа не выделяют эффект конкретного runtime patch.

Ранние таблицы с best-of-N обычно показывают меньшие времена, но арифметическая регрессия остаётся и при сравнении медиан: VM 162,432 → 265,670 мс, native 5,550 → 8,647 мс. Остальные VM-нагрузки смешаны: JSON, digest, secure-random, time-flow и UUID стали быстрее, calls/collections, map-words и string-ops несколько медленнее. Native short-process medians выросли почти везде; масштаб изменения требует отделять запуск/выход от тела программы.

Причины проверены дополнительными последовательными замерами 14 октября. Во время throughput runs не выполнялись наши сборки, тесты или profiling. Фоновый Spotlight наблюдался; поэтому абсолютные времена этого исследования не заменяют матрицу 13 октября. Контроли и кандидаты сравниваются внутри одной вращающейся серии. Полные samples, команды, исходники прототипов и hashes сохранены в [каталоге исследования](bench/polyglot/results/analysis-2026-10-14/).

| Контролируемый случай | Control | Диагностический вариант | Результат |
|---|---:|---:|---|
| VM, одна арифметическая функция, process wall | 286,579 мс | 188,557 мс без per-opcode run-cancellation check | median paired 0,6551; −34,5% времени; 15/15 побед; CV 2,15% / 1,32% |
| Native, арифметика, process wall | 9,696 мс | 8,069 мс без loop GC checkpoint | median paired 0,8413; −15,9%; 15/15 побед; CV 2,08% / 3,07% |
| Native, прогретое тело внутри процесса | 5,5514 мс | 3,8187 мс без loop GC checkpoint | startup и shutdown исключены; 15 процессов |
| C++, тот же прогретый арифметический цикл с checked Int64 | — | 3,7836 мс | CV 3,05%; native без checkpoint близок по compute time |
| C++, process wall, обычная / checked арифметика | 4,471 мс | 6,021 мс | checks заметно меняют сравнение; checked reference прерывается при overflow |

VM-прототип полностью подавляет проверку run cancellation и поэтому **не является production patch**. Native-прототип подавляет только checkpoint в конкретном скалярном цикле, где нет аллокаций; он также не проверен как общая политика GC. Эти A/B оценивают потенциал и причинную стоимость изменения кода, включая влияние на layout/inlining, а не чистое время одной функции. C++ checked reference совпадает по valid-input arithmetic и проверкам промежуточного Int64 overflow; его abort при overflow не моделирует Sputnik exceptions/traces.

Основные результаты: [VM подтверждение, 15 пар](bench/polyglot/results/analysis-2026-10-14/vm-idle-cancel-confirm.json), [native подтверждение, 15 пар](bench/polyglot/results/analysis-2026-10-14/native-checked-confirm.json), [внутренние таймеры](bench/polyglot/results/analysis-2026-10-14/internal-body.json), [11-повторная серия startup/GC/linker](bench/polyglot/results/analysis-2026-10-14/native-causes.json). В первом VM A/B был один выброс; он сохранён, а отдельное подтверждение имеет низкий CV.

Native пустая программа в той же диагностической серии занимает 4,160 мс, C++ — 2,200 мс. Удаление ненужного scheduler shutdown уменьшило пустую программу до 3,749 мс, но почти не изменило полный arithmetic process wall. `-dead_strip_dylibs` не дал ускорения и не снял основные crypto dependencies. Поэтому shutdown и простой linker flag не являются доказанным объяснением всей регрессии. Внутренние таймеры отдельно подтверждают, что оставшийся scalar compute после исключения GC-check близок к checked C++.

Native `Opcode::Safepoint` начал вызывать `native_cycle_checkpoint_if_due()` в `a39ecf6` 8 октября; раньше этот opcode в данном пути был no-op. Изменение исправляло удержание объектов в длинных allocating while/until loops. Это конкретный источник дополнительных расходов в миллионе оборотов arithmetic, а не основание вернуть ошибочное поведение GC. В проверке участвуют thread-local scope/debt state и bookkeeping allocations.

В VM `step()` вызывает out-of-line `runtime_run_cancel_checkpoint_requested()` на каждой инструкции. Cancellation support присутствует в снимке `345a6ff` 6 октября вместе с notebook/run lifecycle. Даже отсутствие run owner проходит TLS access/initialization. Отдельный [CPU sample control](bench/polyglot/results/analysis-2026-10-14/vm-control.sample.txt) показывает `Vm::step`, integer register read/write, cancellation check, TLS и `ensure_integer_sidecar_size`; в [sample диагностического варианта](bench/polyglot/results/analysis-2026-10-14/vm-no-idle-cancel.sample.txt) cancellation/TLS leaves исчезают. Профили снимались отдельно от замеров. Sample weights не являются процентами wall-time ускорения.

В [native sample](bench/polyglot/results/analysis-2026-10-14/native-control.sample.txt) почти всё активное время приходится на сгенерированную скалярную функцию. Детализация этого sample не выделяет стоимость каждого inlined check; причинная проверка опирается на A/B и внутренние таймеры.

Сопоставимость конкурентов имеет практическое значение. Локальный CPython 3.14.6 использует `BINARY_OP_ADD_INT`, `BINARY_OP_SUBTRACT_INT`, `COMPARE_OP_INT`; JIT недоступен, GIL enabled. Это подтверждено [локальным disassembly](bench/polyglot/results/analysis-2026-10-14/python-adaptive.txt) и [метаданными](bench/polyglot/results/analysis-2026-10-14/competitor-runtime.json). Специализация и inline caches описаны в [PEP 659](https://peps.python.org/pep-0659/) и [документации dis](https://docs.python.org/3.14/library/dis.html). У установленной Ruby 4.0.5 в режиме runner нет YJIT/ZJIT; арифметический ISeq уже содержит `opt_plus`, `opt_minus`, `opt_gt`, `opt_lt`. Поражение этим VM не объясняется включённым у конкурента JIT.

Sputnik уже имеет quickened instructions, fused compare/jump, integer sidecars, 16-byte tagged Value и native int/bool scalar lanes. Повторное внедрение этих механизмов не является планом ускорения. Актуальные лишние расходы — проверки/декодирование/чтение подготовленного register state, динамический callable protocol и владение heap values между короткими вызовами.

Состав работы также различается. В [C++ JSON](bench/polyglot/cpp/json_workload.cpp) и [Rust JSON](bench/polyglot/rust/json_workload.rs) используются поиск строковых маркеров и чтение выбранных integers; полноценный JSON DOM не строится, pretty-generation round trip пропущен. [Sputnik JSON](bench/polyglot/sputnik/src/json.s) делает обе операции. Совпадение checksum не означает совпадения работы, поэтому native/Cpp JSON ≈4,09× нельзя считать чистой ценой backend. В calls/collections C++ использует `std::array<int64_t,3>` и прямые typed calls, Sputnik — общие mutable arrays и dynamic values. В time-flow C++/Rust используют value structs, Sputnik — общий Time/TimePeriod API. Это нижние специализированные references, которые полезны после дополнительного контроля семантики.

Для digest есть отдельный алгоритмический сигнал: C++ вызывает собственный `runtime/digest.cpp`, Go — стандартные crypto/hash APIs; C++ сам медленнее Go примерно в 2,84×. Следовательно весь native/Go digest gap нельзя приписать codegen или VM. Распределение стоимости backend, API wrapper и bytes ownership требует отдельного профиля.

Отношения текущих process medians: больше 1 означает, что Sputnik медленнее. JSON C++/Rust — упрощённые references, как описано выше.

| Нагрузка | VM/Python | VM/Ruby | Native/C++ | Native/Go | Native/Rust |
|---|---:|---:|---:|---:|---:|
| arithmetic | 1.70× | 3.68× | 2.15× | 1.49× | 2.18× |
| calls-collections | 0.68× | 1.31× | 4.23× | 3.31× | 3.86× |
| sha-digest | 1.42× | 0.93× | 1.28× | 3.64× | 1.30× |
| json | 0.81× | 1.50× | 4.09× | 1.12× | 3.09× |
| string-ops | 1.38× | 1.85× | 2.29× | 2.23× | 1.88× |
| map-words | 1.31× | 1.92× | 3.04× | 2.28× | 2.04× |
| codecs | 1.55× | 2.06× | 2.03× | 2.90× | 2.40× |
| secure-random | 0.59× | 0.90× | 1.74× | 1.82× | 0.64× |
| time-flow | 0.49× | 1.36× | 4.11× | 2.34× | 3.96× |
| uuid | 0.50× | 0.61× | 1.80× | 1.14× | 0.70× |

Изменения для Sputniktorch необходимо разделять по фактическому состоянию:

| Группа | Состояние и вывод |
|---|---|
| Inline primitive Value operations, negative constructor-hook cache, small constructor CALL buffer | Приняты. Saved pre-inline / selected VM на одной arithmetic source дают 314,402 / 293,956 мс в исследовательской серии. Сборки охватывают группу изменений, не только inline. Эти результаты не показывают регрессии от выбранной группы. |
| Native borrowed self/block, receiver-copy removal, cached signature flags, literal defaults, typed leaf/resource-result ABI, trivial method bodies | Приняты. FFI profiles и отдельные A/B показывают уменьшение dispatch/frame/ownership overhead. Польза короткого call path не гарантирует сквозной MNIST gain. |
| Четыре move при RETURN/caller writes | Приняты. Улучшают O2 call micro; значимое ускорение полного MNIST не установлено. |
| Managed retain/release inline, native code-entry function table, stage6 rest views/frame-pool/SEND buffers, heap-record node cache, take-return-register | Описанные экспериментальные варианты отклонены. Они не являются установленной причиной текущей регрессии; более ранние уже принятые forwarding/trivial-call paths продолжают существовать. |
| PGO | Опциональная конфигурация, не default. Текущий перезамер — O2. Saved Torch PGO VM дала 174,460 мс arithmetic против O2 control около 287–295 мс, но версии compiler/source/profile отличаются: это сравнение конфигураций, не причинный A/B PGO. |

Источники статуса: [Value/dispatch](../sputniktorch/docs/vm-value-dispatch-profile.md), [native numel](../sputniktorch/docs/native-numel-profile.md), [call fast](../sputniktorch/docs/call-fast-profile.md), [stage5](../sputniktorch/docs/mnist-runtime-stage5.md), [stage6](../sputniktorch/docs/mnist-runtime-stage6.md), [stage7](../sputniktorch/docs/mnist-runtime-stage7.md). Saved-binary сравнение записано в [vm-causes.json](bench/polyglot/results/analysis-2026-10-14/vm-causes.json). Предварительный `arithmetic-saved-vm.json` использует source с явным `main()` плюс CLI automatic main entry и выполняет функцию дважды; он не используется в итоговых оценках. Финальные CLI A/B используют source без последнего `main()`, одну invocation, проверенный checksum.

В native Numel профиле Sputniktorch дорогими были NativeFrame, user-send/invocation, code-id dispatch и signature/shaping queries. У самого numel нет нового Tensor allocation или GPU kernel; объяснение этого case аллокациями Tensor неверно. У public ReLU появляются wrapper/resource allocations и несколько Sputnik calls вокруг одного LibTorch kernel. На большой модели GPU/LibTorch работа и ожидания ограничивают эффект уменьшения host overhead. Нельзя суммировать samples разных threads или переносить процент call-micro выигрыша на train wall time.

Порядок реализации ниже выбран по измеренной полезности, риску и зависимости между работами. Диагностическое отключение механизма никогда не проходит admission как итоговое решение.

| Очередь | Изменение | Основание | Основной gate |
|---|---|---|---|
| P0.1 | Дешёвый VM cancellation fast path без TLS lookup на каждой инструкции | 15/15 пар, −34,5% диагностического времени | Scalar loops + cancellation/ensure/task/notebook semantics |
| P0.2 | Native safepoint: cached cycle state и эффектные ограничения для scalar loops | 15/15 пар, −15,9%; compute 5,55 →3,82 мс | Scalar + allocating loops + concurrent GC + native exceptions |
| P0.3 | Воспроизводимый benchmark gate и equal-work references | JSON work mismatch, process floor, разные compiler/profile configurations | Все samples/operations/provenance воспроизводимы |
| P1.1 | VM prepared register execution: убрать повторные sidecar checks и лишнее decode work | Post-cancellation CPU sample | Numeric/calls/collections и полная mixed матрица |
| P1.2 | Общий typed direct-call path и расширение ограниченного inline/forwarding | Native/VM call profiles; C++ typed-call gap | Call micros + aliases/defaults/kwargs/handlers + Torch |
| P1.3 | Точная декомпозиция native startup и lazy runtime family initialization | Native empty 4,16 против C++ 2,20 мс; blanket linker flag не помог | Empty/short programs + реально используемые modules/tasks/crypto |
| P2.1 | Builtin aggregate/stdlib specialization и временные allocation lifetimes | Time/collections/strings и algorithm-dependent digest gap | Equal-work API lanes + allocation/ownership profiles |
| P2.2 | Fresh balanced O3/LTO/PGO tier, затем решение о tracing/JIT | Existing PGO potential, но неполное закрытие host overhead | Broad held-out suite + source/toolchain/profile identity |

1. **P0.1 — cancellation fast path.** В [context.cpp](runtime/context.cpp) и [VM step](runtime/vm.cpp) подготовить привязку текущего run owner/cancel flag к VM execution context. Для отсутствующего owner hot path должен иметь дешёвый local predicate; для активного owner — прежний atomic flag check и корректное masking в ensure. При lease child VM, callback reentry, park/resume и смене logical task context привязка обновляется. Уменьшать частоту проверки отмены не требуется. Возможная альтернатива — компактный inline context accessor с контролируемой TLS initialization boundary; её надо сравнить с VM-local state, а не заранее объявлять быстрее.

   Первый результат должен быть production-safe эквивалентом fast path, а не `if (false)`. Проверить отмену бесконечного integer loop, активный run, вложенные tasks/callbacks, cancellation during I/O, ensure masking и сохранение latency отмены. Benchmark gate: arithmetic, task-local, return/send/forwarding, все десять micro workloads, raw/Orbit VM; отдельно notebook execution lifecycle. Измеренные −34,5% — потенциал данного micro, не обещание результата полноценной правки.

2. **P0.2 — native safepoints.** В [генераторе](tools/sputnik/main.cpp) передавать подготовленный cycle-state pointer в исполняемый code body, снижая повторные TLS accesses. Обновлять state после переходов, способных изменить execution/thread context. Рассмотреть fast debt/pending-GC predicate вместо повторного чтения thread-local vector state на каждой back edge. Для effect-proven scalar region можно ограничить/устранить allocation-debt polling, но GC handshake и bounded responsiveness должны сохраняться. Критерии региона: отсутствие heap allocations/root mutations, parking и неизвестных calls; наличие stable GC root description.

   Обязательные negative/pressure cases: длинный allocating while/until, cyclic containers, escaping closures, shared cross-task roots, concurrent collection, blocking callbacks и cancellation/error unwinding. Контролировать peak/end live bytes, чтобы не вернуть прежнее накопление всего training loop. Уровень `Opcode::Safepoint` нельзя сделать глобальным no-op. Первая цель — приблизиться к checked C++ по scalar body без ухудшения allocating programs.

3. **P0.3 — benchmark admission.** Runner должен принимать явные compiler/interpreter paths, проверять compile flags и hashes до/после, сохранять saved control до rebuild. Разделить process wall, imports/compile/module init, body, teardown; short programs оставить отдельной startup lane, а body gates делать достаточно длинными. Выделить `json-full-api` с полноценным decode/materialization, pretty round trip и streaming в каждой реализации; существующий C++/Rust extraction reference сохранить под явной отдельной меткой. Добавить checked Int64 C++ reference и typed-versus-generic collection controls. Проверять выполненные операции/форму результата, а не только checksum.

   Историческую attribution продолжить, если нужен точный commit виновник: пересобрать один и тот же source fixture из `d5ca4c7`, `a39ecf6^`, `a39ecf6` и текущего dirty snapshot одинаковым Clang/flags/SDK в изолированных каталогах. Имеющийся audit уже доказывает стоимость конкретного safepoint и cancellation paths; он не делает полный bisect остальных +15–20% native rows. HTTP сравнивать с одним client SHA, одинаковым storage/contract/threads и без profiler.

4. **P1.1 — VM prepared execution.** `ensure_integer_sidecar_size` виден после устранения cancellation excess. Pre-size sidecars при frame preparation/rebinding, оставить growth/validation на переходах, а validated hot read/write использовать подготовленные storage/capacity. Убрать повторное исходное-bytecode и quick-code fetch там, где quick instruction уже содержит всё необходимое. Сохранять initialized bits, watched registers, captures, snapshots, handler merges и full-width integer values. Small numeric execution loop или дополнительные fused sequences должны сохранять budget и cancellation semantics каждой logical instruction.

   Quickening и fused compare/jump уже существуют. Generic batching и fallback split из MNIST stage5 не подтвердили сквозной выигрыш; переносить их без нового A/B нельзя. Новый admission ориентирован на scalar/mixed language workloads с отдельным контролем Torch regression. После P0.1 выбрать точные участки по post-change profiles и assembly, чтобы не оптимизировать исчезнувший hotspot.

5. **P1.2 — calls и scalar ABI.** Расширять существующие trivial-body/typed-leaf пути, не создавать параллельный набор Torch-only exceptions. Для доказанных private/frozen callees генерировать scalar parameter/result lanes и direct code entry; materialize generic Value на настоящей dynamic boundary. Для небольших call chains добавить ограниченный inline/forwarding с сохранением source stack в diagnostics. Подготовить полную signature plan для defaults/keyword/rest shapes; cached flags уже реализованы, следующий шаг должен устранять остающиеся repeated shape queries/packing. Подготовленный или borrowed argument view должен иметь явный lifetime и GC roots через nested calls/parking.

   Проверить `wrap/mix/pick/score_row`, defaults, обычные и forwarded calls, dynamic replacement/alias/inherited methods, kwargs/block forwarding, type hooks, auto-assign, rescue/ensure и escaping captures. Сравнить VM/native и typed/generic controls. Stage6 rest/SEND buffers улучшали соответствующие micros, но не MNIST; их можно пересмотреть как общую language optimization только с новым scope/gate. Scalar int instructions и 16-byte Value уже присутствуют; менять общий atomic ownership на небезопасный non-atomic нельзя.

6. **P1.3 — startup и footprint.** Измерить init/exit phases и реально загруженные dylibs/initializers в empty, arithmetic, JSON, TLS и crypto programs. Найти, какие required symbols удерживают runtime/crypto families после dead stripping. Рассмотреть lazy construction только используемых task/scheduler/module families, разделение registrars/helpers по объектным файлам и native dependency plan из фактических operations. Удаление scheduler shutdown в диагностике полезно только пустому case и не доказало ускорение полного arithmetic; `-dead_strip_dylibs` сам по себе не является решением.

   Native shutdown должен запускаться для уже использованного runtime и гарантировать завершение реальных tasks/FFI carriers. Не изменять module registration/reflection, crypto availability, blocking lifecycle или порядок корректной очистки ради empty benchmark. Gate включает task-heavy native, raw/Orbit HTTP и Torch resource lifetime. Цель этой работы — уменьшить fixed floor коротких binaries; большой compute loop этим не лечится.

7. **P2.1 — stdlib и representations.** Сначала profile равного объёма работы. Для Time/TimePeriod проверить повторное извлечение civil fields, selector dispatch, materialization temporaries и возможность immutable/escape-proven aggregate lanes. Для arrays/maps/strings — direct homogeneous access с guards, borrowed byte spans и устранение доказанных временных копий; сохранить bounds, insertion order, name-indifferent keys, Unicode и mutation-during-iteration semantics. Hash index и definitive missing-key path уже исправлены; повторный проект O(1) map lookup не нужен.

   Для JSON измерять parser/generator/materialization отдельно. Для digest сравнить один и тот же алгоритмический backend через VM, native и direct C++, затем отдельно оптимизировать его: native/Go gap включает backend choice. Для codecs/random/UUID сохранить validation, capabilities, real entropy и unbiased ranges. На Torch измерять количество wrapper/resource/control-block allocations отдельно от raw numel, где новых Tensor allocations нет. Хранение RSS после churn не равно leak: в последнем telemetry остаётся 7 объектов/1 400 байт shells после 2 400 013 allocations, но allocator удерживает около 84 MiB RSS.

8. **P2.2 — optimized tier и архитектура.** После первых runtime fixes заново обучить отдельные profiles на balanced mix arithmetic/calls/collections/JSON/HTTP и Torch CPU/MPS. Сделать три свежие конфигурации одним toolchain: O2, O3+ThinLTO, O3+ThinLTO+PGO. Held-out inputs, отдельные output directories, strict CFG mismatch checks и provenance обязательны. Учесть подтверждённые `llvm-ar`/test harness fixes в [profile-build](docs/engineering/profile-builds.md). Saved PGO выигрыш не переносится автоматически на текущий snapshot и не делает NN-only profile универсальным default.

   Если после P0/P1 VM host overhead всё ещё доминирует в целевых workloads, измерить trace/superinstruction prototype либо typed native compilation горячих регионов с guards/deopt. Полная замена backend на LLVM или большой JIT — следующий архитектурный выбор, не первая правка. Сначала должна быть показана польза относительно исправленного O2 и balanced PGO, включая compile/startup/memory cost.

Каждый пункт — отдельная reviewable правка с causal A/B, а не пакет из множества гипотез. Сборки, tests, profiling и timing runs разделены. Использовать одинаковые source/SDK/toolchain/flags/library versions и проверку hashes; сохранять выбросы. Для значимого утверждения иметь минимум 15 пар в коротком causal gate и дополнительную подтверждающую серию при высоком CV. Admission требует подтверждённого выигрыша целевого workload, отсутствия значимого ухудшения полной матрицы и сохранения семантики; эффект одного micro не выдаётся за ускорение приложения.

Общие correctness gates: VM/native/backend equivalence, integer overflow profiles и error traces, method/world invalidation, watch/debugger/notebook bindings, GC roots/cycles/weak references, cancel/ensure/task-local inheritance, blocking FFI and callbacks, typed leaf/resource lifetime, native full coverage без bytecode fallback. Для backend-sensitive Torch changes — реальные CPU/MPS tests, MPS fallback выключен, одинаковые LibTorch/PyTorch/thread counts, epoch losses/accuracy и checkpoint round trip. Algorithms/model composition остаются в Sputnik; C++ сохраняет kernel/lifetime/serialization boundary согласно [AGENTS.md](../sputniktorch/AGENTS.md).

Первый рекомендуемый PR: P0.1. Второй: P0.2, после проверки concurrent GC invariants. P0.3 нужно выполнить до оценки более широких P1/P2 improvements. Конкретные значения speedup после production-safe правок определяются повторными замерами, а не диагностическим выключением обязательных mechanisms.
