---
id: functions
title: Функции, методы и классы
summary: def, one-liner-форма, поля конструктора @attr, методы класса и bare-nullary send.
category: language-core
order: 30
prerequisites: [overview]
related: [blocks, pattern-matching]
status: draft
spec_refs:
  - anchor: 8-функции-методы-классы-и-объектная-модель
    label: Spec §8
---

# Функции, методы и классы

## `def`

Функция объявляется через `def`. Поддерживаются многострочная и однострочная
формы; тело — это выражение, и его значение становится результатом:

```sputnik
def add(a, b):
  a + b

def double(x): x * 2

def probe():
  add(double(3), 4)

probe()
# => 10
```

## Классы и поля конструктора

`class` объявляет тип. Приём `@attr` прямо в сигнатуре `init` сразу заводит поле
экземпляра — не нужно вручную перекладывать аргументы:

```sputnik
class Collection:
  def init(@count): pass

  def size():
    @count

def probe():
  items = Collection(40)
  items.size()

probe()
# => 40
```

Объект класса вызывается как конструктор: `Collection(40)` создаёт экземпляр и
присваивает `@count`.

## Bare-nullary send

Метод без аргументов можно вызывать без скобок — это **bare-nullary send**.
Формы `items.size` и `items.size()` эквивалентны. То же работает для методов
класса, объявленных через `class_method`:

```sputnik
class Collection:
  def init(@count): pass
  def size(): @count

class Build:
  class_method def version():
    20

def probe():
  items = Collection(40)
  items.size + items.size() + Build.version + Build.version()

probe()
# => 120
```

> [!note]
> Bare-nullary send работает для методов, полей и class-методов. Скобки нужны
> только когда вы передаёте аргументы или блок.

Дальше: диспетчеризация по форме аргументов — это
[pattern matching](guide:pattern-matching) и multi-clause `def`.
