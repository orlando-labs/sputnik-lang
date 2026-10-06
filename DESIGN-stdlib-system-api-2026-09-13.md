# DESIGN: `system` — запуск процессов, capture и async IO

Статус: исходный проект полного контракта. Базовый модуль реализован;
актуальный API и границы реализации описаны в [руководстве](docs/stdlib-system.md).
Дополнительные возможности ниже остаются проектом, если их нет в руководстве.

Дата: 2026-09-13.

Область v1: запуск дочерних процессов на Linux/macOS, обмен через stdin,
stdout и stderr, управление временем жизни, строковый тег `cmd`. Примеры
ниже задают будущую семантику; они не являются проходящим сейчас corpus.

## 1. Предлагаемая форма

Модуль называется `system`, как предложено в задаче. Lowercase соответствует
соседним системным модулям `io`, `fs`, `net`, `task`.

```sputnik
import system
from system import cmd

# Самый короткий путь от программы к строке stdout.
version = system.output("git", "--version")

sputnik_code = "p(1 + 2)"
command = cmd"""
  bin/isputnik --eval "#{sputnik_code}"
  """

text = command.output()
result = command.capture()
result.stdout_text()
result.stderr_text()
result.status.code
```

`cmd` строит неизменяемый `system.Command`. Создание команды ничего не
запускает. Одна команда может выполняться несколько раз, в том числе
параллельно. Интерполяции вычисляются один раз при **создании** команды;
каждый вызов `output`, `capture`, `run` или `spawn` создаёт новый процесс.

| Операция | Результат | Для чего |
| --- | --- | --- |
| `command.output(...)` | `Str` | Получить stdout как UTF-8 |
| `command.capture(...)` | `system.Result` | Собрать вывод или обработать три потока |
| `command.run(...)` | `system.Status` | Выполнить с выводом в терминал |
| `command.spawn(...)` | `system.Process` | Самостоятельно управлять процессом и IO |

У модуля есть соответствующие сокращения:
`system.output(program, *args, ...)`, `system.capture(program, *args, ...)`,
`system.run(program, *args, ...)`, `system.spawn(program, *args, ...)`.
Они также принимают готовый `Command` первым аргументом, без дополнительных
позиционных аргументов. Во всех формах нет автоматического shell detection.

## 2. `Command` и тег

### 2.1. Обычный конструктор

```sputnik
command = system.command("bin/isputnik", "--eval", sputnik_code)
command.argv                         # ["bin/isputnik", "--eval", sputnik_code]
command = command.with(cwd: project_dir, env: {"SPUTNIK_ENV": "test"})
```

`system.command(program, *args, cwd: null, env: {}, clear_env: false)`
сохраняет снимок аргументов и настроек. `with(...)` возвращает новый объект;
для `env` это объединение overlays, последнее значение побеждает.
Доступный пользователю `argv` включает имя программы в позиции 0 и не может
изменить команду через мутацию возвращённой коллекции.

`program` — непустой `Str` или `fs.Path`. Аргументы — `Str`, `fs.Path`,
`Int`, `Float`, `Bool`; числовые и логические значения используют обычное
Sputnik `to_str()`, строки не перекодируются. `null`, `Bytes`, коллекции и
произвольные объекты требуют явного преобразования и иначе дают `TypeError`.
Нулевой байт запрещён в program, аргументах, cwd и env. Пустой аргумент
разрешён. Динамическая ошибка значения даёт `ArgumentError` до запуска.

`system.output("git status")` пытается запустить файл с именем `git status`;
эта строка не разбивается на программу и аргументы.

### 2.2. Интерполяция сохраняет границы аргументов

```sputnik
quoted = cmd"""
  bin/isputnik --eval "#{sputnik_code}"
  """
quoted.argv
# ["bin/isputnik", "--eval", sputnik_code]

unquoted = cmd"""
  bin/isputnik --eval #{sputnik_code}
  """
unquoted.argv
# Те же три аргумента, даже если sputnik_code содержит пробелы и переводы строк.

named = cmd"""
  tool --name=#{name} #{path}
  """
named.argv
# ["tool", "--name=" + name, path]

listed = cmd"""
  tool #{system.args(paths)}
  """
listed.argv
# ["tool", *paths]
```

Правила тега:

1. Макрос разбирает только статический текст. Каждая `#{expr}` остаётся
   отдельным value slot; её содержимое никогда не разбирается как командный
   синтаксис. Даже кавычки, `;`, `$()`, обратные кавычки и новые строки в
   `sputnik_code` останутся байтами одного аргумента.
2. Смежные части одного слова конкатенируются. Например, `pre#{x}post`
   образует один аргумент. Пустой самостоятельный scalar slot образует `""`,
   а не исчезает. Несколько интерполяций вычисляются слева направо, один раз.
3. Список не разворачивается неявно. `system.args(sequence)` создаёт
   неизменяемую группу аргументов с теми же scalar-правилами. Она допустима
   только как целое некавычённое слово после program. В кавычках, рядом с
   префиксом/суффиксом или на месте program — ошибка. Пустая группа даёт
   ноль аргументов. В обычном API доступен существующий call spread `*args`.
4. Статические пробел, табуляция, LF и CRLF вне кавычек разделяют аргументы.
   Новая строка — продолжение **одной** команды. Она не запускает следующую.
   Структурный dedent выполняется по общим правилам текстовых блоков.
5. Статические одинарные и двойные кавычки группируют текст и удаляются.
   `""` и `''` дают пустой аргумент. В обеих группах Sputnik-интерполяции
   остаются активны: кавычки внутри `cmd` не меняют синтаксис Sputnik.
6. Неэкранированные `|`, `&`, `;`, `<`, `>`, `$`, обратная кавычка,
   `(`, `)`, `*`, `?`, `[`, `]`, `~` вне кавычек дают диагностику с
   предложением использовать явный shell или заключить литерал в кавычки.
   В кавычках это обычные символы. Glob, env expansion, command substitution
   и shell operators в `cmd` не исполняются. `#` — обычный символ, не комментарий.
7. Тег использует **исходный статический текст после dedent**, а не сначала
   вычисленный `Str`. Вне одинарных кавычек `\` экранирует следующий
   статический символ; `\` + перевод строки удаляются как continuation.
   В одинарных кавычках `\` — обычный символ. Конечный `\`, незакрытая
   кавычка и пустая команда — compile-time ошибки. Последовательность `\n`
   в command-грамматике даёт букву `n`; для программного перевода строки
   нужен scalar slot со строкой, содержащей LF, либо буквальный LF в кавычках.
8. `\#{` — предусмотренное Sputnik экранирование маркера интерполяции и
   даёт буквальные `#{`. Нельзя backslash-escape уже выделенный value slot.
   Интерполяция не может закрыть кавычку или создать разделитель аргументов.

Это защищает границы argv, но не отменяет собственные правила вызываемой
программы: файл, начинающийся с `-`, может быть опцией. Например,
`system.command("git", "diff", "--", path)` использует разделитель опций Git.

### 2.3. `cmd` или `system`

Канонический экспорт — `cmd`. Желаемое длинное написание получается
обычным переименованием импорта, без второго поведения:

```sputnik
from system import cmd as system

command = system"""
  bin/isputnik --eval "#{sputnik_code}"
  """
text = command.output()
```

В этом варианте примера имя `system` занято тегом; runtime namespace при
необходимости импортируется под другим именем. Никакого специального keyword
`system` или нового parser-level command literal не нужно.

**Однострочная форма.** Сейчас `cmd"""bin/run --help"""` не поддерживается:
`Lexer::lex_text_block` требует перевод строки после opener и выдаёт
`AMB_TEXTBLOCK_OPENER`. Это ограничение самого текстового литерала, до
обработки тега. Проверено текущим `build/sputnik parse`.

Для коротких команд реализованы `cmd"bin/run --help"` и
`cmd'bin/isputnik --eval "#{sputnik_code}"'`. Обе формы поддерживают интерполяцию
и передают макросу `Ast.StringTemplate` с соответствующим `quote_kind`.
Это общий механизм для всех string tags; обычная строка в одинарных кавычках
без тега по-прежнему не интерполируется. Поддержка закреплена в lexer, VM,
corpus и full native проверках.

Многострочный `cmd"""…"""` также поддерживается.
Однострочное triple-quoted написание потребовало бы отдельного изменения
грамматики текстовых блоков и в этом проекте не вводится. Запись тега
внутри prose обозначает форму, а не работающий inline triple-quoted literal.

### 2.4. Shell — явная команда

```sputnik
command = system.shell("sort | uniq -c", executable: "/bin/sh")
result = command.capture(input: lines)

# Параметры shell передаются отдельно от его исходного текста.
command = system.shell('printf "%s" "$1"', args: [text])
```

`system.shell(script, executable: "/bin/sh", args: [])` строит обычный
`Command` с argv `[executable, "-c", script, "sputnik-shell", *args]`:
`$0` фиксирован, пользовательские args начинаются с `$1`. Shell работает без
login/interactive flags и не выбирается через `$SHELL`. `script` — явный
shell source; интерполяция в обычный `Str` здесь не получает защиту `cmd`.
Capability проверяется для executable shell; выполняемые им команды не
проходят через Sputnik повторно. Отдельный `sh` tag и pipeline DSL отложены.

## 3. Простой запуск и результат

### 3.1. `output`, `capture`, `run`

```sputnik
text = system.output("git", "status", "--short")

result = system.capture("bin/isputnik", "--eval", sputnik_code, check: false)
if result.success?():
  p(result.stdout_text())
else:
  p(result.stderr_text())

status = system.run("make", "test")
```

`output` — `capture(...).stdout_text()`. Она захватывает **оба** выходных
потока, поэтому ошибка содержит stderr. Строка декодируется строго как UTF-8;
нет автоматических `strip`, chomp или нормализации CRLF. Для бинарного вывода
используется `capture().stdout`. Invalid UTF-8 даёт существующий
`CodecDecodeError` после завершения и освобождения процесса.

`capture` и `output` по умолчанию проверяют код завершения (`check: true`).
`run` тоже использует `check: true`; `check: false` сохраняет любой exit/signal
status, но не подавляет ошибки запуска, IO, callbacks, лимиты или отмену.

| Метод | stdin по умолчанию | stdout | stderr |
| --- | --- | --- | --- |
| `output` | EOF | capture | capture |
| `capture` | EOF | capture | capture |
| `run` | `:null` | `:inherit` | `:inherit` |
| `spawn` | `:pipe` | `:pipe` | `:pipe` |

`input: Str | Bytes | null` в `output`/`capture` пишет UTF-8 или исходные
байты и обязательно закрывает stdin. `null` означает немедленный EOF.
Это не наследование stdin Sputnik. `run(stdin_from: :inherit)` явно подключает
терминальный ввод. `output` фиксирует оба выхода в capture; для изменения
маршрутов используется `capture` или `run`.

### 3.2. Стабильные типы

```sputnik
result.command              # Command
result.status               # Status
result.stdout               # Bytes?; null, если поток не сохраняли
result.stderr               # Bytes?; null при merge или обработчике
result.stdin                # Bytes?; только при record_input: true
result.values               # Map: результаты переданных обработчиков
result.input_bytes          # Int: число байтов, записанных в pipe
result.input_closed_early?() # Bool: subprocess закрыл stdin раньше producer
result.complete?()          # Bool: status и все требуемые IO завершены успешно
result.success?()           # result.complete?() and result.status.success?()
result.stdout_text()         # строгий UTF-8; ошибка состояния, если stdout == null
result.stderr_text()         # строгий UTF-8; ошибка состояния, если stderr == null
result.check()              # self или ProcessExitError для неуспешного status

status.pid                  # Int
status.code                 # Int?; exit code, null при завершении сигналом
status.signal               # Symbol?; например :term, null при обычном exit
status.signal_number        # Int?; host-specific номер, в том числе неизвестный
status.exited?()             # Bool
status.signaled?()           # Bool
status.success?()            # exited?() and code == 0
status.check()              # self или ProcessExitError
```

Пустой захваченный вывод — пустые `Bytes`, а не `null`. Status не кодирует
сигнал числом `128 + signal` и не смешивает отрицательный signal number с
exit code. Для неизвестного имени signal остаётся `null`, но
`signaled?()` и `signal_number` сохраняют информацию.

Метаданные результата и byte buffers неизменяемы. Callback values не
замораживаются неявно: shareability всего `Result` зависит от них по общим
правилам Sputnik. `Status` и `Command` shareable. Свойства показаны в getter
нотации; предикаты и операции имеют обычную call-форму.

## 4. Удобный capture трёх потоков

Для потокового обмена `capture` использует существующий Sputnik multiblock
`with:` с именованными callable-параметрами `&stdin: null`, `&stdout: null`,
`&stderr: null`. Каждый callback опционален и получает один endpoint.
Библиотека управляет параллельным IO и временем жизни процесса.

```sputnik
command = cmd"""
  worker --stdio
  """
result = command.capture(timeout: 30.0) with:
  stdin |input|:
    input.write_all!(request_header.bytes())
    input.write_all!(request_body)

  stdout |output|:
    output.each_chunk(size: 65536) |chunk|:
      consume_output(chunk)

  stderr |errors|:
    errors.each_chunk(size: 65536) |chunk|:
      consume_diagnostic(chunk)
```

По общей multiblock-семантике блоки становятся обычными callable keyword
arguments. Готовые функции передаются через callable references:

```sputnik
result = command.capture(
  timeout: 30.0,
  stdin: &write_request,
  stdout: &read_output,
  stderr: &read_diagnostics
)
```

Если переменная уже содержит callable, она передаётся без `&`.
`stdin: null`, `stdout: null`, `stderr: null` эквивалентны отсутствующему
callback. Anonymous block у `capture` не принимается. Правила аргументов
соответствуют [named multiblock](sputnik_v20.9_named_multiblock_rfc.md).

Точная семантика:

1. `with:` создаёт closures и передаёт их в вызов. Тела callbacks ещё не
   исполняются. Перед spawn `capture` валидирует callable contracts и
   совместимость опций; ошибочный набор аргументов не оставляет процесса.
   Отдельной пользовательской фазы конфигурации нет.
2. Повторное имя в multiblock и конфликт с одноимённым explicit keyword
   используют общие `AMB_MULTIBLOCK_DUPLICATE` и
   `AMB_MULTIBLOCK_KEYWORD_DUPLICATE`; динамические конфликты kwargs —
   обычную binding-ошибку до вызова. Неизвестные keywords и non-callable
   значения проверяются по общей сигнатуре. Non-null `input:` и stdin
   callback взаимоисключающие; `input: null` не мешает stdin callback.
   Output callback заменяет стандартный capture своего потока. Для маршрутов
   используются отдельные `stdout_to`/`stderr_to` (§7): callback допустим
   только при стандартном значении `:capture` для этого потока. Иначе —
   `ArgumentError` до spawn. При `stderr_to: :stdout` stderr callback запрещён,
   а stdout callback, если передан, читает объединённый поток.
3. Обработчики запускаются одновременно как управляемые библиотекой Sputnik
   tasks на общей strand вызова, используя семантику `task.async`. Они могут
   приостанавливаться на IO, каналах и других cancellation points. Это
   конкурентное IO, не обещание CPU-параллелизма callbacks.
4. `stdin` callback получает `io.Writer`; выходные callbacks — `io.Reader`.
   Используются общие `read!`, `read_all!`, `read_line!`, `each_chunk`,
   `write!`, `write_all!`, `puts!`, `close!`, а не новый IO-протокол.
   Библиотека выдаёт каждому обработчику владение его endpoint.
5. Выходы с маршрутом `:capture` и без callback одновременно собираются
   в память. При отсутствии input и stdin callback вход закрывается сразу;
   возврат из stdin callback закрывает его в ensure.
   `capture` ждёт завершения процесса **и** всех потребителей/producer.
6. Возвращённые callbacks значения попадают в `result.values[:stdin]`,
   `[:stdout]`, `[:stderr]` только для назначенных обработчиков. Для выходного
   потока с callback соответствующее `result.stdout`/`stderr` равно `null`.
   Неявного tee и второго consumer нет.
7. Если output callback успешно вернулся до EOF, библиотека дочитывает и
   отбрасывает остаток этого потока. Это позволяет прочитать только нужный
   префикс, не блокируя ребёнка на заполненном pipe. Явный `reader.close!()`
   означает отказ от оставшегося вывода: он может вызвать EPIPE у ребёнка.
8. Исключение callback отменяет остальные обработчики, запускает cleanup
   процесса и затем пробрасывается с сохранением исходной причины. См. §7.
   После завершения capture endpoints закрыты; они не должны уходить в
   продолжающую работу task. Задачи, созданные самим callback, он присоединяет
   до возврата; попытка использовать сохранённый endpoint позже даёт closed error.

Например, для streaming stdin с обычным накоплением stdout/stderr достаточно:

```sputnik
result = system.capture("processor") with:
  stdin |input|:
    chunks.each |chunk|:
      input.write_all!(chunk)

p(result.stdout_text())
```

Multiblock сам по себе не задаёт параллелизм: одновременное выполнение
callbacks — контракт именно `system.capture`. Порядок entries не задаёт
порядок обработки потоков. Если нужен диалог «отправить запрос — получить
ответ», callbacks синхронизируются
обычным `Channel`. Библиотека устраняет deadlock от последовательного drain
потоков, но не может исправить циклическое ожидание в пользовательском протоколе.

## 5. Async без второго набора методов

```sputnik
import task
from system import cmd

first = task.async:
  command = cmd"""
    bin/isputnik --eval #{first_code}
    """
  command.capture()

second = task.async:
  command = cmd"""
    bin/isputnik --eval #{second_code}
    """
  command.output()

first_result = first.wait()
second_text = second.wait()
```

Методы ожидают результат с точки зрения вызывающей task, освобождая worker
на время ожидания OS IO или завершения процесса. Отдельные `capture_async`,
`async: true`, process-specific future и обязательный `await` не нужны.
`task.spawn` используется по существующим правилам, когда нужна другая strand.

`handle.wait(timeout: ...)` ограничивает ожидание **TaskHandle**; это не
таймаут subprocess. Дедлайн subprocess задаётся самой операции:
`command.capture(timeout: ...)`. Отмена task, исполняющей capture/run/output,
отменяет принадлежащую ей операцию и запускает cleanup. Отмена только
наблюдателя `Process.wait` имеет другую семантику (§6).

## 6. Низкоуровневый `Process`

```sputnik
process = command.spawn()
try:
  result = process.communicate(input: payload)
ensure:
  process.close!()
```

`communicate(input: null, check: true, timeout: null, limit: 16777216,
record_input: false, drain_timeout: 1.0, kill_after: 1.0)` выполняет
управляемый одноразовый обмен на уже запущенном процессе. Это тот же engine,
что у `capture`, но без callable-параметров stdin/stdout/stderr. До него
нельзя вручную читать или писать ни один управляемый pipe; нарушение даёт `ProcessStateError` до
дополнительного IO. Повторный или конкурирующий `communicate` также запрещён.
`wait` после завершения communicate возвращает сохранённый status.

Для полного ручного контроля:

```sputnik
command.spawn() |process|:
  out_task = task.async:
    process.stdout.read_all!(limit: 16777216)
  err_task = task.async:
    process.stderr.read_all!(limit: 16777216)
  in_task = task.async:
    try:
      process.stdin.write_all!(payload)
    ensure:
      process.stdin.close!()

  in_task.wait()
  stdout = out_task.wait()
  stderr = err_task.wait()
  status = process.wait()
  status.check()
  [stdout, stderr, status]
```

Все три task здесь запускаются до первого wait. Пример демонстрирует ручное
владение при успешном обмене; production-код должен также отменять и
присоединять sibling tasks при ошибке. Для этого `capture` предпочтительнее.

`Process` предоставляет:

| Метод / getter | Контракт |
| --- | --- |
| `pid`, `command` | Идентичность запуска и immutable command |
| `stdin`, `stdout`, `stderr` | Родительские endpoints; `null`, если не `:pipe` |
| `status` | Сохранённый terminal `Status` или `null`, не блокирует |
| `running?()` | Нет наблюдённого terminal status; состояние может сразу измениться |
| `wait(timeout: null)` | Дождаться только exit; не читает pipes и не проверяет code |
| `communicate(...)` | Одноразовый одновременный обмен и exit status |
| `signal(sig)` | Послать сигнал этому живому ребёнку |
| `terminate()` / `kill()` | `signal(:term)` / `signal(:kill)` |
| `close!()` | Идемпотентный cleanup: закрыть stdin, остановить живого ребёнка, освободить endpoints |
| `closed?()` | Владение ресурсами закрыто; status остаётся доступным |

Процесс — синхронизированный runtime handle; несколько `wait` могут ожидать
один сохранённый status. У endpoints обычное владение IO: `task.async` на
той же strand не требует handoff; передача на новую strand требует
`adopt!()` отдельно для соответствующего endpoint. Параллельные операции
над одним endpoint не разрешены: нужен один consumer/producer; конкурирующий
доступ даёт `ResourceBusyError`.

`wait` не выполняет drain: при непрочитанном stdout/stderr ребёнок может
заблокироваться и не завершиться. Его timeout/отмена снимают только waiter,
оставляя процесс и потоки работающими. Повторный wait допустим. Reaper
сохраняет status независимо от наличия пользовательских waiters.

Блочная форма `spawn` возвращает значение блока и вызывает `close!()` в ensure.
Если блок вернулся с живым ребёнком, cleanup его останавливает. Успешное
завершение нужно явно дождаться внутри блока. Blockless spawn передаёт
обязанность вызвать `close!()` пользователю; GC не выполняет блокирующий
wait, а передаёт потерянный handle supervisor на cleanup.

## 7. Опции, ограничения и завершение

### 7.1. Настройки запуска

Все executing methods принимают `cwd`, `env`, `clear_env`, переопределяющие
настройки Command на один запуск. Значения по умолчанию в constructor —
`cwd: null`, `env: {}`, `clear_env: false`.

- `cwd` применяется только к ребёнку. `null` означает снимок cwd родителя
  при запуске. Никакого временного глобального `chdir`.
- `env` — overlay поверх снимка окружения родителя; значение `null` удаляет
  ключ. `clear_env: true` начинает с пустого env. Ключи — непустые строки без
  `=` и NUL, значения — строки без NUL. Само наследование env входит в запуск;
  чтение env в Sputnik-коде по-прежнему требует собственного `env.read`.
- Program с `/` разрешается относительно эффективного cwd ребёнка; без `/`
  ищется по эффективному child `PATH`. При отсутствии PATH используется
  фиксированный `/usr/bin:/bin`, пустой компонент PATH явно означает child cwd.
  Разрешённый executable проверяется до spawn и передаётся backend как
  конкретный путь; нельзя проверить один PATH, а выполнить другой.
- Родитель не передаёт посторонние fd. Только stdin/out/err и служебные
  дескрипторы строго на время spawn handshake; последние закрываются при exec.

`run` и `spawn` принимают маршруты `stdin_from`, `stdout_to`, `stderr_to`.
Имена `stdin`/`stdout`/`stderr` в `capture` всегда обозначают callbacks;
route options отделены от них и одинаково называются во всех методах:

| Параметр | Маршруты |
| --- | --- |
| `stdin_from` | `:null`, `:inherit`, `:pipe` (только spawn), открытый readable `fs.File` |
| `stdout_to` | `:inherit`, `:null`, `:pipe` (только spawn), открытый writable `fs.File` |
| `stderr_to` | То же, плюс `:stdout` — alias окончательного child stdout fd |

`capture` принимает `stdout_to`/`stderr_to`: `:capture` по умолчанию,
`:inherit`, `:null`, открытый writable `fs.File`; `stderr_to` также принимает
`:stdout`. При стандартном `:capture` non-null callback заменяет накопление
вывода обработкой через Reader. У capture stdin задаётся только `input:` или
stdin callback; параметра `stdin_from` у него нет.
`run` не принимает `:pipe`: вернуть Status, оставив недоступные пользователю
pipes, было бы ошибкой. Произвольные `io.Reader`/`Writer` не являются OS fd:
для них используется callback с копированием. Переданный `fs.File` дублируется
для ребёнка, исходный handle библиотека не закрывает; file offset общий по
правилам OS, поэтому одновременное ручное использование требует координации.

При `stderr_to: :stdout` объединение происходит в OS до чтения, а
`result.stderr`/`process.stderr` равно `null`. Для двух независимых pipes
глобальный порядок между stdout и stderr не обещается.

### 7.2. Ограничения памяти, EOF и broken pipe

- `limit: 16777216` — общий предел удерживаемых библиотекой байтов stdout,
  stderr и optional stdin transcript. `null` явно снимает лимит. Отрицательное
  значение неверно; ноль разрешает только пустой захват. Выходные callbacks
  и прямой файл не копируются библиотекой в бесконечный буфер; pipe/backpressure
  ограничивает очереди. Память, выделяемая пользовательским callback, этим
  лимитом не регулируется.
- Превышение — `ProcessOutputLimitError` с ограниченным partial result,
  остановкой producer и cleanup ребёнка. Вывод не обрезается молча.
- `record_input: true` сохраняет в `result.stdin` только байты, успешно
  переданные в OS pipe. Это **не** подтверждение, что ребёнок их прочитал.
  По умолчанию transcript отсутствует; заданный `input:` не копируется второй
  раз только ради результата. `input_bytes` считается в любом случае.
- IO остаётся бинарным. UTF-8 декодируется явно; incremental text reader
  должен сохранять неполную многобайтную последовательность между chunks.
- Raw write в закрытый stdin поднимает `BrokenPipeError`, не завершает Sputnik
  через SIGPIPE. Managed `input:` рассматривает ранний EPIPE как закрытие
  входа, продолжает drain и сохраняет `input_closed_early?()`. Если пользователь
  хочет проверить полную доставку в pipe, проверяет этот флаг. EPIPE внутри
  пользовательского stdin callback является его обычной ошибкой, если он
  сам её не обработал. Exit status ребёнка в любом случае сохраняется.

### 7.3. Timeout, отмена и cleanup

`timeout: null` означает отсутствие дедлайна. Для совместимости с `task` и
IO `Int` задаёт миллисекунды, `Float` — секунды; в примерах используются
`30.0`/`1.0`. Отрицательные, NaN и бесконечные значения запрещены, ноль —
немедленный дедлайн без запуска нового ребёнка.

В capture/run/output дедлайн начинается при входе в вызов, после обычного
вычисления аргументов и создания multiblock closures, до
подготовки OS ресурсов; включает spawn, запись, чтение и ожидание exit.
В communicate он начинается при вызове на существующем процессе. Один
монотонный абсолютный deadline сохраняется через все resume/retry.
`spawn` не принимает execution timeout: сразу возвращённый handle управляется
через wait/communicate и explicit lifecycle. Он принимает cleanup-настройки
`kill_after`, `drain_timeout`, `group`, сохраняемые в handle.

На timeout, отмене или ошибке managed операции:

1. Зафиксировать первичную причину; отменить callbacks и остановить stdin.
2. После освобождения readers callbacks переключить оставшиеся pipe reads
   в ограниченный drain/discard. Два consumer никогда не читают один fd.
3. Послать TERM управляемому ребёнку, если он ещё жив. Через
   `kill_after: 1.0` послать KILL, если требуется.
4. Закрыть endpoints, получить и сохранить terminal status, выполнить reap;
   после cleanup доставить исходную ошибку. Cleanup защищён от повторной
   отмены, однако ожидание остаётся кооперативным.

Timeout операции не обещает возврат ровно в указанный момент: syscall spawn
может быть непрерываемым, после него нужно остановить появившегося ребёнка;
cleanup также требует времени. Не завершающийся в OS процесс или callback,
игнорирующий cancellation points, не допускает обещания жёсткого времени
возврата. Вынесенный supervisor сохраняет обязанность reap даже при аварийном
unwind. Ошибка cleanup прикладывается к исходной, не заменяя её.

Exit ребёнка и EOF его pipes — разные события. Потомок может продолжать
держать stdout открытым. После наблюдения exit действует
`drain_timeout: 1.0` до завершения требуемых output consumers/EOF, даже если
общего timeout нет. Истечение даёт `ProcessDrainTimeoutError` с partial result
и закрывает endpoints. Длительный consumer может увеличить значение или
задать `null`; `null` явно допускает неограниченное ожидание EOF. Значения
`kill_after`/`drain_timeout` используют те же единицы; kill_after конечен и
неотрицателен, drain_timeout дополнительно допускает null.

`group: :inherit` по умолчанию сохраняет process group. Cleanup относится
к непосредственному ребёнку. `group: :new` создаёт отдельную группу;
cleanup посылает TERM/KILL этой группе, пока сохранена её OS identity.
Это полезно для shell и сборок, создающих дочерние процессы. Не обещается
остановка потомков, вышедших из группы через setsid/setpgid. TTY job control
в v1 отсутствует; терминальный ввод обычно требует `group: :inherit`.

Manual `signal` работает только с конкретным unreaped child, возвращает
`false` после его завершения, `true` при принятом OS запросе. Публичного
`signal(pid)` для произвольного PID в v1 нет. Supervisor синхронизирует
signal и reap, чтобы никогда не сигналить переиспользованный PID. Для
group cleanup сохраняет unreaped leader/эквивалентную identity guard до
последней посылки сигнала; после финализации не использует старый PGID.

### 7.4. Ошибки

Добавляемые имена регистрируются в общем error registry:

| Ошибка | Parent | Данные |
| --- | --- | --- |
| `ProcessError` | `IOError` | Базовая ошибка процесса |
| `ProcessSpawnError` | `ProcessError` | command, операция, OS errno, безопасное описание |
| `ProcessExitError` | `ProcessError` | status, command, result при наличии capture |
| `ProcessStateError` | `ProcessError` | Недопустимый переход/режим handle |
| `ProcessOutputLimitError` | `ProcessError` | limit, partial result, поток превышения |
| `ProcessTimeoutError` | `TimeoutError` | phase, command, partial result/status |
| `ProcessDrainTimeoutError` | `ProcessTimeoutError` | phase `:drain`, partial result |
| `ProcessCallbackError` | `ProcessError` | stream, original cause, partial result |

Отмена остаётся `CancelledError`, а не превращается в exit failure.
`BrokenPipeError`, `AlreadyClosedError`, `IsolationError`, `ResourceBusyError`,
`CodecDecodeError`, `CapabilityError` и `ReplayProviderError` переиспользуются.
Для отсутствующего executable `ProcessSpawnError` сохраняет ENOENT; запуск
никогда не возвращает `false` вместо ошибки.

При ошибке callbacks внешний вызов получает `ProcessCallbackError` с
исходным исключением в cause; обычные внутренние cancelled siblings не
заменяют причину. Отмена владеющей операции остаётся `CancelledError` без
callback wrapper. Supervisor атомарно фиксирует первую причину. Наблюдение
неуспешного exit само по себе не прерывает drain: `ProcessExitError`
поднимается только после IO; поэтому ошибка IO/лимита/callback имеет приоритет
перед финальной проверкой exit. Следующие cleanup failures сохраняются отдельно.
Partial result может иметь `status == null`, если ОС ещё не дала terminal status;
`complete?()` тогда false. `Result.check()` для incomplete result даёт
`ProcessStateError` вместо предположения об успешном выполнении.

## 8. Интеграция с существующим Sputnik

Эти пункты отделяют уже имеющиеся возможности от требуемых изменений.

| Найденная основа | Что добавить для `system` |
| --- | --- |
| `corpus/run/macro_string_tag/source.s`: tag получает `Ast.StringTemplate` с parts | Stdlib macro export `cmd`, командный parser, hygienic lowering в Command |
| `frontend/parser/parser.cpp`: `AstStringText`, `AstStringExpr`, `AstStringEscape` с source/value | Зафиксировать единый source-view adapter после dedent, включая escapes/raw text; не терять происхождение символов до cmd lexer |
| `runtime/io.h`, `runtime/io.cpp`: `RuntimeIoResource`, байты, ownership | OS pipe endpoints, доступные через общие Reader/Writer selectors |
| `RuntimePipe::State` — mutex/CV и очередь байтов в памяти | Не передавать `io.Pipe` ребёнку как fd; это другой транспорт |
| `runtime/reactor.*`: epoll/kqueue, `wait_async` | Ожидание OS pipe readiness и child exit, без потока на каждого ребёнка |
| `runtime/vm.cpp`: park/retry для одиночных read!/write! с бесконечным timeout | Resumable накопительные IO, finite deadlines, process pump и callbacks |
| `runtime/concurrency.*`: `task.async`, `task.spawn`, TaskHandle | Structured supervision внутренних callbacks и cleanup |
| `frontend/parser`, `frontend/binder`, `frontend/hir`: named multiblock и callable keywords | Nullable callable-параметры stdin/stdout/stderr в capture; обычное keyword lowering |
| `runtime/stdlib_registry.*`: descriptors и io handlers | Регистрация system, Command, Process, Status, Result, Args; named callable validation и вызов callbacks |
| `profile/capabilities.cpp`: уже есть `process.spawn`, `process.signal` | Проверки в process boundary, без новых capability aliases `system.*` |
| `profile/effects.cpp`: canonical effect `process` пока отсутствует | Добавить `process`; отразить process IO/wait как `process` + `async`, сохранение результата как `alloc` |
| `runtime/world.h`: provider для внешнего IO | Process provider boundary и отказ от реального spawn при replay |
| `tools/sputnik/main.cpp`: native stdlib codegen | Тот же runtime engine, native callbacks и parity с VM |

### 8.1. Макрос

Макрос не запускает процесс при компиляции и не получает shell/host capability.
Он компилирует статические слова в сегменты `Literal`/`ValueSlot`/`ArgsSlot`,
затем строит hygienic runtime call с expressions в исходном порядке.
Нельзя получить промежуточную строку через обычную интерполяцию и затем
снова разбить её: на этом теряется главное свойство API.

Source view нужно проверить на реальных AST tests: текущий пример sqlish
не обрабатывает все escape parts и не является готовым парсером `cmd`.
Если ранняя проверка Sputnik escapes отбрасывает command escape до macro
expansion, этот путь для tags должен сохранять source и делегировать
проверку provider. Это согласуется с source/cooked-разделением,
предложенным в [дизайне Regexp](DESIGN-stdlib-regexp-api-2026-07-08.md).
Пустая команда, неправильное кавычение и shell operator получают отдельные
diagnostic codes с source span; неправильный runtime ArgsSlot — exception.
Native stdlib macro provider должен поддержать те же exports/import aliases,
что пользовательский module; наличие native runtime descriptor само по себе
ещё не экспортирует макрос для стадии F1.5.

Короткая форма `command = cmd` с многострочным literal, затем
`command.output()` не требует postfix расширений tag. Прямую цепочку после
закрывающего delimiter (`...""".output()`) в v1 не обещаем: текущий
`match_string_tag` распознаёт ограниченную форму `AstPostfixChain`. Её
поддержку нужно отдельно закрепить AST/macro тестами, чтобы вызов метода
относился к **результату** тега. В примерах используется промежуточная команда.

### 8.2. OS backend и состояние операции

Предлагаемые новые файлы: `runtime/process.h`, `runtime/process.cpp`,
`runtime/stdlib_system.cpp`; общие IO adapters остаются в IO layer.

Backend предпочитает `posix_spawn` с file actions, явным env, cwd и signal
attributes на поддерживаемых платформах. Запрещён callback Sputnik/C++
пользователя между fork и exec. Если нужен fallback, post-fork часть должна
состоять только из заранее подготовленных async-signal-safe операций и
error-pipe handshake; capability/provider проверки выполняются до неё.
Не менять глобальный cwd, env или signal disposition родителя ради запуска.

Pipes создаются с close-on-exec. Только **родительские** endpoints становятся
nonblocking; stdin/out/err ребёнка имеют обычный blocking mode. Нужно
корректно обработать fd collisions с 0/1/2, частичный setup failure, EINTR,
EAGAIN, partial write, EOF и закрыть противоположные концы сразу после spawn.
В родительском процессе EPIPE не должен доставлять фатальный SIGPIPE:
используется согласованная с signal layer per-thread suppression strategy,
не временная глобальная установка SIG_IGN вокруг write.

Process operation хранит argv/env snapshot, фазу spawn, fd owners, offsets
записи, buffers, absolute deadlines, callbacks, terminal status, первую
ошибку и cleanup state. На resume syscall не должен повторно запускать
ребёнка, заново отправлять уже записанный префикс, терять прочитанные байты
или повторно выполнять пользовательский callback с начала.

Reactor обрабатывает только readiness и короткие state transitions;
пользовательский код работает на scheduler workers. Supervisor/reaper
наблюдает только принадлежащих runtime детей, не использует `waitpid(-1)`
для чужих процессов embedding host. На Linux можно использовать pidfd,
на macOS — process events kqueue, с зарегистрированным fallback wakeup
механизмом. Проверка немедленного exit закрывает гонку spawn/registration.
Child wait обязан быть event-driven; busy polling и блокирующий waitpid
на scheduler worker не удовлетворяют async-контракту.

POSIX допускает, что ошибка после успешного возврата spawn проявится exit 127;
по одному этому коду нельзя отличить её от выхода самой программы.
`ProcessSpawnError` создаётся только при известной ошибке запуска/handshake,
а неизвестный 127 остаётся обычным status. Для Linux/macOS нужно отдельно
проверить гарантии используемого backend и диагностировать errno там, где
оно доступно. [POSIX posix_spawn](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html).

### 8.3. Signal, capability, replay

Существующий [проект Signal](DESIGN-stdlib-signal-api-2026-07-04.md) пока
оставляет child reaping будущему модулю. Его observe-only CHLD должен
сосуществовать с reaper: пользовательское наблюдение не забирает exit
status. Нельзя включать SIG_IGN/SA_NOCLDWAIT для CHLD при управляемых детях;
изменение, лишающее runtime status, отклоняется. В embedding host с
несовместимой disposition запуск отклоняется или использует согласованный
host provider. Signal handlers не вызывают VM и не выполняют wait/drain.

Дети получают пустую runtime signal mask; dispositions, изменённые Sputnik
ради собственного runtime, восстанавливаются в default, включая SIGPIPE.
Неизменённые host dispositions сохраняются согласно платформенному
контракту; изменение signal state локально ребёнку. Нужно согласовать это с
будущими `Signal.ignore` и explicit inheritance options, не блокируя v1.

`process.spawn` проверяется для разрешённого executable до OS effects.
`process.signal` нужен для явных управляющих вызовов. Необходимое закрытие
принадлежащего операции ребёнка входит в spawn lease и не требует отдельного
granted signal, иначе отказ в capability делал бы cleanup невозможным.
`cwd`, file redirects, наследование env описываются в spawn request для host
policy. Открытие redirect file самим Sputnik остаётся обычным `fs` действием.

Разрешение spawn не ограничивает действия исполняемой программы Sputnik
capabilities: это host process с OS правами. Target check пути также не
обещает идентичность executable при concurrent filesystem replacement;
host sandbox или descriptor-based execution нужны для более сильной политики.
Команда и trace сохраняют privacy/taint метки; argv, env values, stdin и
вывод не попадают в диагностику и trace в открытом виде автоматически.

В replay реальный процесс **никогда** не запускается. В v1 без process-aware
provider — `ReplayProviderError` до создания ресурсов. Архитектура оставляет
provider операции spawn, read/write/EOF, exit, signal, cancellation, используя
логические process ids. Полная запись/воспроизведение интерактивного обмена
— отдельный этап; один сохранённый stdout не считается replay поддержкой.
VM и native обязаны одинаково проверять effects/capabilities/provider.

## 9. План реализации и приёмка

1. **Контракт и tag.** Закрепить exports, shape source view, диагностики,
   immutable argv и интерполяции без side effects. Добавить parser/macro
   fixtures, проверяющие argv без настоящих subprocess.
2. **OS foundation.** Endpoint adapters, spawn transaction, supervisor/reaper,
   Status, Process, cleanup, registry и policy boundaries. Сначала тестировать
   отдельным локальным helper executable с точными режимами поведения.
3. **Cooperative engine.** Сохраняемые continuations для накопительных IO и
   finite deadlines; communicate/run/capture/output. Отдельно проверить
   suspend/resume, ошибки и единичное выполнение side effects.
4. **Capture multiblock.** Nullable named callable-параметры, три concurrent
   tasks, обычный IO, возвращаемые значения, ошибки, отмена, передача владения
   и stdin transcript. `with:` lowering передаёт обычные keyword values;
   native dispatcher вызывает их через общий callable protocol, без новых
   block slots в ABI. Проверить также передачу `stdin: &handler` и callable
   из переменной: синтаксис multiblock не должен быть условием работы API.
5. **Native parity и документация.** Native lowering не должен скрыто вызывать
   VM или blocking capture. Добавить тесты backend equivalence, spec/registry
   синхронизацию и module sidecar после появления реализованных selectors.

Релиз v1 требует шагов 1–5. Нельзя обозначать blocking implementation как
поддержку async только потому, что вызов обёрнут в `task.async`.

Минимальная проверяемая матрица:

| Область | Проверки |
| --- | --- |
| argv | Пробелы, кавычки обоих видов, пустой slot, LF/CRLF, Unicode, literal backslash, `\#{`, NUL, вложенные выражения, side effect slot ровно один раз |
| Tag | Одинаковый argv с/без кавычек вокруг slot, concatenation, пустой Args, запрещённый Args context, alias `cmd as system`, runtime binding с тем же именем, диагностики статического shell syntax |
| Запуск | ENOENT, EACCES, неверный cwd, env overlay/delete/clear, child PATH, имена executable с пробелами, literal exit 127 |
| Потоки | Пустой/binary/invalid UTF-8 вывод, CRLF без преобразования, все fd modes, настоящий stderr merge, file offset/borrow, отсутствие fd leaks при любой фазе setup failure |
| Deadlock | Одновременно stdin/stdout/stderr больше pipe capacity; helper пишет много stderr до чтения stdin; всё проходит при одном worker |
| Capture | `with:` и callable keywords эквивалентны, null callbacks, unknown/duplicate/non-callable аргументы, route/callback conflicts до spawn, неназначенные streams захватываются, тела не исполняются при binding, все callbacks стартуют до join, значения в values |
| Reader lifecycle | Ранний возврат callback drain/discard, explicit close, EPIPE producer, borrowed stream после scope, competing read, cross-strand ownership |
| Лимиты | Ровно limit, limit+1, общий лимит двух streams/transcript, bounded partial, потоковый sink с большим выводом без линейного роста памяти |
| Отмена | Во время spawn, partial write, read, callback, wait, после exit до EOF; TERM ignored → KILL; repeated close; primary error не теряется |
| Ожидание | Несколько waiters, timeout одного не убивает процесс, wait после communicate, мгновенный exit до регистрации, отсутствие zombies и сигналов reused PID |
| Потомки | Child exit при открытом pipe потомка, drain_timeout, group new/inherit, TERM/KILL группе при удержанной identity, escaped descendant не выдаётся за убитого |
| Runtime | Ticker task прогрессирует при одном worker; тысячи маленьких процессов без thread-per-child; сохранённые offsets/deadlines после каждого resume |
| Политики | Отказ spawn/signal, разрешённый cleanup без signal grant, effect process, denied replay без OS side effects, trace redaction |
| Backend | Одна матрица VM/native на Linux/macOS, CHLD integration и fd baseline до/после каждого failure path |

Тестовый helper должен предоставлять режимы echo-argv, echo-bytes,
simultaneous-flood, close-stdin-early, exit-code, signal-exit, ignore-term,
fork-and-hold-pipe, delayed-read/write. Проверки синхронизируются pipes/каналами;
тайминги используются только как предельные watchdogs, не как способ
угадать очередность событий.

## 10. Граница v1 и основания решений

В v1 входят простое получение stdout, бинарный capture, три concurrent
callbacks, ручной Process, stdin transcript по запросу, redirects, timeout,
отмена, process group cleanup и tag с аргументами. Отложены PTY, управление
терминалом/jobs, detached daemons, public fork/exec замена Sputnik процесса,
сигналы произвольным PID, pipes между несколькими Command как DSL, Windows
и полноценный process replay provider. Для shell pipelines уже есть явный
`system.shell`, с exit semantics выбранного shell.

Для сравнения, Ruby `Open3.popen3` отдаёт отдельные pipes и предупреждает о
deadlock при чтении только одного выхода. Предлагаемый multiblock capture берёт
совместный запуск consumers на себя. [Ruby Open3](https://docs.ruby-lang.org/en/master/Open3.html#method-c-popen3).

Python `communicate` объединяет запись stdin, чтение обоих выходов и wait,
но накопление вывода требует памяти. Отсюда в этом проекте отдельные
streaming callbacks и явный общий предел хранения; это проектное решение
Sputnik. [Python subprocess](https://docs.python.org/3/library/subprocess.html#subprocess.Popen.communicate).

Ключевой пользовательский путь остаётся коротким:

```sputnik
from system import cmd

command = cmd"""
  bin/isputnik --eval "#{sputnik_code}"
  """
text = command.output()
```
