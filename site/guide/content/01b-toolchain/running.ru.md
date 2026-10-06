---
id: running
title: Запуск и сборка с sputnik
summary: Как запустить, собрать в нативный бинарник и заглянуть в стадии компиляции.
category: toolchain
order: 15
prerequisites: []
related: [overview, functions]
status: draft
---

# Запуск и сборка с sputnik

`sputnik` — это компилятор и раннер Sputnik. Одной командой он выполняет программу,
другой — собирает автономный нативный исполняемый файл. Ниже — рабочий цикл на
проверенных примерах.

## Сборка тулчейна

Компилятор и рантайм собираются одним `make`. Нужен `clang++` (по умолчанию),
хосты — Linux и macOS; внешних зависимостей для сборки компилятора нет.

```sh
make build/sputnik     # собрать только sputnik
make build            # собрать весь тулчейн (sputnik, sputniktest, тесты)
```

Готовый бинарник появится в `build/sputnik`.

## Запуск программы

`sputnik <файл.s>` выполняет модуль и печатает значение последнего выражения
верхнего уровня в его каноническом виде (repr).

```sputnik
def greet(name):
  "Hello, #{name}!"

greet("Sputnik")
```

```sh
build/sputnik hello.s
# => "Hello, Sputnik!"
```

> [!note]
> Строка в выводе показана в кавычках — это repr значения, ровно как его печатает
> рантайм. Целое `42` печатается как `42`, строка — как `"..."`.

## Сборка нативного исполняемого файла

`sputnik build <файл.s> -o <путь>` компилирует программу в автономный нативный
бинарник. В stdout печатается JSON-отчёт о сборке, рядом сохраняется
сгенерированный C++ (`<путь>.native.cpp`), а по указанному пути — исполняемый
файл.

```sh
build/sputnik build hello.s -o hello
./hello
# => "Hello, Sputnik!"
```

## Цели и опции сборки

| Опция | Значения | Назначение |
| --- | --- | --- |
| `--target` | `native`, `native-debug`, `bytecode-wrapper` | Форма артефакта: нативный код, нативный с отладкой, или обёртка над байткодом. |
| `--entry` | `auto`, `init`, `main`, `main-only` | Какая точка входа исполняется: модульная инициализация и/или `main`. `auto` выбирает по наличию `main`. |
| `--out-dir` | `<каталог>` | Куда складывать артефакты. |
| `--grant` | `<cap[=target]>` | Выдать capability (например, `net.connect`, `fs.read`) собранному бинарнику. |

```sh
# байткод-обёртка вместо нативного кода
build/sputnik build hello.s --target bytecode-wrapper -o hello.bc
```

## Сборка проекта из нескольких файлов

Для проекта используется манифест `sputnik.build.json`: он описывает исходники,
цели и кэш. `sputnik build` принимает манифест вместо одиночного файла.

```sh
build/sputnik build sputnik.build.json \
  --out-dir build/out \
  --cache-dir build/cache
```

## Стадии компиляции и инструменты разбора

Полезно, чтобы понять, во что опускается синтаксис. Каждая стадия — отдельная
подкоманда, печатающая свою форму программы; программу они не запускают.

```sh
build/sputnik lex   source.s   # токены
build/sputnik parse source.s   # дерево разбора
build/sputnik hir   source.s   # высокоуровневый IR (видна явная lowering-форма)
build/sputnik mir   source.s   # среднеуровневый IR
build/sputnik bc    source.s   # байткод
build/sputnik native-dump source.s   # сгенерированный нативный код
```

Артефакты `.sputnikbc` — это данные, а не доверенный код: перед исполнением их
структуру проверяет верификатор.

```sh
build/sputnik verify   module.sputnikbc --json   # проверить байткод
build/sputnik metadata module.sputnikbc --json   # прочитать метаданные модуля
```

## Прогон конформанс-набора

Корпус проверяемых программ гоняется через `sputniktest`:

```sh
build/sputniktest run corpus              # весь корпус
build/sputniktest run corpus --bundle M11 # отдельный бандл
```

Дальше: как устроены сами программы — начните с [обзора языка](guide:overview) и
[функций и классов](guide:functions).
