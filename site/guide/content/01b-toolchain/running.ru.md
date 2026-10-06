---
id: running
title: Запуск и сборка с amberc
summary: Как запустить, собрать в нативный бинарник и заглянуть в стадии компиляции.
category: toolchain
order: 15
prerequisites: []
related: [overview, functions]
status: draft
---

# Запуск и сборка с amberc

`amberc` — это компилятор и раннер Amber. Одной командой он выполняет программу,
другой — собирает автономный нативный исполняемый файл. Ниже — рабочий цикл на
проверенных примерах.

## Сборка тулчейна

Компилятор и рантайм собираются одним `make`. Нужен `clang++` (по умолчанию),
хосты — Linux и macOS; внешних зависимостей для сборки компилятора нет.

```sh
make build/amberc     # собрать только amberc
make build            # собрать весь тулчейн (amberc, ambertest, тесты)
```

Готовый бинарник появится в `build/amberc`.

## Запуск программы

`amberc <файл.am>` выполняет модуль и печатает значение последнего выражения
верхнего уровня в его каноническом виде (repr).

```amber
def greet(name):
  "Hello, #{name}!"

greet("Amber")
```

```sh
build/amberc hello.am
# => "Hello, Amber!"
```

> [!note]
> Строка в выводе показана в кавычках — это repr значения, ровно как его печатает
> рантайм. Целое `42` печатается как `42`, строка — как `"..."`.

## Сборка нативного исполняемого файла

`amberc build <файл.am> -o <путь>` компилирует программу в автономный нативный
бинарник. В stdout печатается JSON-отчёт о сборке, рядом сохраняется
сгенерированный C++ (`<путь>.native.cpp`), а по указанному пути — исполняемый
файл.

```sh
build/amberc build hello.am -o hello
./hello
# => "Hello, Amber!"
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
build/amberc build hello.am --target bytecode-wrapper -o hello.bc
```

## Сборка проекта из нескольких файлов

Для проекта используется манифест `amber.build.json`: он описывает исходники,
цели и кэш. `amberc build` принимает манифест вместо одиночного файла.

```sh
build/amberc build amber.build.json \
  --out-dir build/out \
  --cache-dir build/cache
```

## Стадии компиляции и инструменты разбора

Полезно, чтобы понять, во что опускается синтаксис. Каждая стадия — отдельная
подкоманда, печатающая свою форму программы; программу они не запускают.

```sh
build/amberc lex   source.am   # токены
build/amberc parse source.am   # дерево разбора
build/amberc hir   source.am   # высокоуровневый IR (видна явная lowering-форма)
build/amberc mir   source.am   # среднеуровневый IR
build/amberc bc    source.am   # байткод
build/amberc native-dump source.am   # сгенерированный нативный код
```

Артефакты `.amberbc` — это данные, а не доверенный код: перед исполнением их
структуру проверяет верификатор.

```sh
build/amberc verify   module.amberbc --json   # проверить байткод
build/amberc metadata module.amberbc --json   # прочитать метаданные модуля
```

## Прогон конформанс-набора

Корпус проверяемых программ гоняется через `ambertest`:

```sh
build/ambertest run corpus              # весь корпус
build/ambertest run corpus --bundle M11 # отдельный бандл
```

Дальше: как устроены сами программы — начните с [обзора языка](guide:overview) и
[функций и классов](guide:functions).
