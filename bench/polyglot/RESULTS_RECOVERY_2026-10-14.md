# Sputnik performance recovery — 14 октября 2026

Порядок: исправление бенчмарков → контрольный перезамер → runtime/compiler changes → correctness gates → повторный перезамер. Контрольные CLI/VM и native executables сохранены до правок. Все сравнения ниже используют текущий dirty checkout, Apple M4, Apple Clang 21, O2, tagged Value и system allocator; PGO не используется.

## Эквивалентность и контрольная матрица

Добавлена отдельная `json-full-api`: общий DOM во всех реализациях, 1 000 compact round trips, 20 pretty round trips, 20 000 полностью материализованных JSONL rows. Все поля, массивы и строки проверяются. Изменение поля name, не участвующего в checksum, отвергли все семь реализаций до и после runtime changes. C++ использует json-c 0.19; Rust — serde_json 1.0.145 с Cargo.lock. Formatting/key order и представление JSON numbers остаются свойствами библиотек.

Прежняя json-серия сохранена как legacy reference: C++/Rust извлекают поля без полного DOM/pretty round trip, Go использует typed structs. `cpp-checked` проверяет каждое промежуточное Int64 add/sub (abort при overflow, без Sputnik error semantics). `cpp-generic` изолирует heterogeneous heap arrays с bounds/type checks; сами вызовы остаются typed C++.

Runner принимает явные пути инструментов, записывает hashes инструментов/библиотек/исходников и команды сборки. Admission manifest защищает --no-build от смешивания вариантов; hashes проверяются до и после timing. Для арифметики и full JSON добавлены body entries: одна untimed in-process warmup и пять timed invocations. Process wall, body per invocation и outside-batch time сохранены отдельно. Outside-batch включает warmup, imports/init, reporting и teardown и не равен чистому startup. Body wrapper меняет контекст C++ inlining, поэтому его нельзя вычитать из старого single-invocation executable.

Ниже контрольный перезамер до оптимизаций: 7 fresh-process repeats, 2 process warmups и rotating order. Все 45 строк шести серий прошли checksum/operation admission.

| Нагрузка / таймер | VM, мс | Native, мс | Python | Ruby | C++ | Go | Rust | Дополнительный C++ control |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| arithmetic / process | 285.903 | 10.068 | 171.173 | 81.575 | 4.683 | 6.917 | 4.495 | cpp-checked: 6.162 |
| arithmetic / body | 282.379 | 3.931 | 142.572 | 73.243 | 2.291 | 3.693 | 1.971 | cpp-checked: 3.842 |
| json / process | 30.909 | 17.365 | 39.921 | 21.217 | 4.489 | 15.605 | 5.812 | — |
| json-full-api / process | 59.130 | 21.196 | 44.105 | 26.026 | 25.915 | 25.400 | 11.594 | — |
| json-full-api / body | 51.736 | 16.585 | 23.952 | 15.017 | 22.508 | 21.578 | 9.009 | — |
| calls-collections / process | 15.921 | 10.243 | 24.911 | 12.944 | 2.509 | 3.217 | 2.721 | cpp-generic: 2.578 |

Полный generic JSON существенно меняет исходное сравнение с C++: native Sputnik уже в контрольной серии быстрее json-c reference. Это свойство выбранных API/библиотек и одинаковой работы, а не универсальное превосходство compiler backend.

## Принятые правки

VM: borrowed cancellation checkpoint view привязывается один раз на каждый step loop, в том числе leased child VM, callback и task redispatch после migration. Run scopes и cleanup masks обновляют TLS view на месте; atomic stop flag проверяется на каждой logical instruction. Shared owner продолжает удерживать flag lifetime.

Native: conservative whole-body effect proof допускает только известные Int/Bool operations, primitive constants и control flow. Calls, heap operations, unknown reads, captures, rest и handlers исключены. Entry checkpoint сначала обслуживает накопленный allocation debt; empty-argument scalar body затем не накапливает новый debt и пропускает повторный allocation polling. Nonempty arguments сохраняют back-edge checks. Ошибки checked arithmetic выходят из тела с исходной трассировкой. Allocating loops, GC thresholds, collector gate и mutator cooperation сохраняют прежнюю реализацию.

Первый native candidate с TLS debt counter отвергнут: scalar body был медленнее на 3,8%, 0/15 побед. Он полностью убран из production sources. Его binaries/JSON сохранены в `build/recovery-2026-10-14/native-attempt-1` и `results/recovery-2026-10-14/native-attempt-1`; результаты не смешаны с итоговыми.

## Итоговые причинные пары

15 rotating saved/current pairs и 2 warmups; elapsed/body измеряются внутри одной серии. [raw JSON](results/recovery-2026-10-14/causal-paired.json)

| Нагрузка / таймер | VM до → после, мс | Δ VM времени, paired | Побед VM | Native до → после, мс | Δ native времени, paired | Побед native |
|---|---:|---:|---:|---:|---:|---:|
| arithmetic/process | 293.786 → 199.694 | -32.3% | 15/15 | 9.997 → 8.149 | -19.1% | 14/15 |
| arithmetic/body | 283.997 → 192.681 | -32.4% | 14/15 | 3.884 → 3.971 | +1.8% | 2/15 |
| json-full-api/process | 59.654 → 53.839 | -10.1% | 15/15 | 21.728 → 21.713 | +0.3% | 6/15 |
| json-full-api/body | 51.958 → 46.234 | -11.3% | 15/15 | 16.715 → 16.626 | +0.0% | 7/15 |
| calls-collections/process | 15.740 → 14.475 | -9.1% | 15/15 | 10.011 → 9.985 | -0.3% | 9/15 |

В основной body серии был один большой VM outlier (CV около 29%); он сохранён, не исключён. После завершения HTTP проведено отдельное подтверждение, ещё 15 rotating pairs. Оно не заменяет первоначальные samples.

[raw JSON](results/recovery-2026-10-14/arithmetic-confirm-paired.json)

| Арифметика / таймер | VM до → после, мс | Δ VM paired | VM CV до / после | Native до → после, мс | Δ native paired | Native CV до / после |
|---|---:|---:|---:|---:|---:|---:|
| arithmetic/process | 291.861 → 200.469 | -31.3% | 2.54% / 0.89% | 10.009 → 8.411 | -15.6% | 2.95% / 3.23% |
| arithmetic/body | 283.720 → 193.176 | -31.9% | 2.93% / 2.47% | 3.921 → 4.006 | +2.2% | 0.88% / 1.04% |

Подтверждение: process arithmetic −31,3% VM и −15,6% native, обе 15/15 побед. Прогретое VM body −31,9%; native body **+2,2% времени**, 1/15 побед. Этот небольшой compute tradeoff сохранён явно: исправление выигрывает single-process arithmetic, а wrapper меняет контекст inlining и не показывает ускорения native compute. Для будущего generic call/inline пути это отдельный gate. Native в остальных micro workloads в основном нейтрален; time-flow +2,2% в 7-парной серии имеет ratio mean CI, пересекающий 1.

## Mixed micro admission

7 пар каждой нагрузки, все checksums сохранены. Отрицательное Δ означает сокращение времени. [raw JSON](results/recovery-2026-10-14/mixed-matrix-paired.json)

| Нагрузка | Δ VM paired | VM CV до / после | Δ native paired | Native CV до / после |
|---|---:|---:|---:|---:|
| arithmetic | -31.1% | 2.57% / 0.72% | -19.1% | 5.47% / 2.11% |
| calls-collections | -8.4% | 1.74% / 0.71% | -0.9% | 1.73% / 1.17% |
| sha-digest | -5.0% | 4.95% / 2.94% | -0.5% | 5.03% / 1.41% |
| json | +0.4% | 1.27% / 2.51% | +0.2% | 1.94% / 1.27% |
| string-ops | -3.3% | 1.89% / 1.00% | -1.8% | 3.97% / 4.19% |
| map-words | -8.8% | 0.86% / 1.00% | -2.2% | 1.23% / 2.16% |
| codecs | -6.4% | 1.33% / 1.59% | -0.8% | 5.91% / 2.03% |
| secure-random | -4.5% | 0.77% / 1.70% | -1.2% | 1.81% / 2.10% |
| time-flow | -9.6% | 0.76% / 1.00% | +2.2% | 2.91% / 2.30% |
| uuid | -5.0% | 1.49% / 4.40% | -1.5% | 6.13% / 6.09% |
| json-full-api | -9.2% | 1.09% / 0.89% | -0.7% | 0.85% / 1.08% |

## HTTP admission

Один ранее закреплённый клиент SHA-256 `32a31135352b651e9459bdcf14235d964c25186a861515160fb82db3cc163529`. По 5 rotating pairs, 15 s, 4 clients; каждое измерение прошло 76-request mixed contract. Native servers требуют full native coverage. [raw JSON](results/recovery-2026-10-14/http-paired.json)

| Серия | До, median RPS | После, median RPS | Paired after/before | CV до / после |
|---|---:|---:|---:|---:|
| raw-vm | 23215.31 | 24062.51 | 1.0406× | 0.43% / 1.66% |
| raw-native | 32805.82 | 32562.66 | 0.9904× | 0.38% / 0.88% |
| orbit-vm | 2443.19 | 2578.82 | 1.0543× | 0.36% / 0.37% |
| orbit-native | 7474.15 | 7435.83 | 0.9937× | 0.90% / 0.26% |

## Correctness и пределы результата

Прошли: 9 benchmark harness tests; cancellation view lifetime/masking/reentry/worker tests; VM, stdlib task, IO, blocking FFI, notebook integration, notebook cancellation и run-task suites; native blocking dispatch/throw/cancellation; 7 scalar effect-proof cases, включая heap argument и checked overflow trace. GC pressure проверяет 300 000 циклов в многозадачной нагрузке и длинные allocating while/until в task scope, с сохранением живого root. Реальные Torch CPU/MPS native и VM tests проверяют forward/backward, обучение и checkpoint/shared storage; MPS fallback выключен, LibTorch 2.14.0_1.

Отдельно воспроизведено прежнее ограничение: direct native entry вне task/request scope не устанавливает NativeCycleScope. Циклические arrays в таком entry удерживаются и в сохранённой сборке (~396 MiB peak), и в первом candidate (~385 MiB). Это не регрессия safepoint optimization; исправление root-entry lifecycle требует отдельной проверки всех blocking/collector boundaries. [Данные](results/recovery-2026-10-14/root-gc-limitation.json). Данный patch не объявляет сбор циклов вне существующих scope boundaries исправленным.

Speedup не переносится автоматически на NN приложения или другие workloads. Остаточный VM gap требует prepared registers/декодирования; native calls/collections — работы с callable ABI и startup. O3/LTO/PGO в этой задаче не включались.

Все исходные и итоговые samples, команды сборок, admission manifests и rejected experiment сохранены в [каталоге результатов](results/recovery-2026-10-14/).
