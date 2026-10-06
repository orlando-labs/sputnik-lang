---
id: pattern-matching
title: Pattern matching
summary: case/when по типам, литералам, диапазонам и деструктуризация map.
category: semantics
order: 40
prerequisites: [functions]
related: [blocks]
status: draft
spec_refs:
  - anchor: 9-pattern-matching-v1
    label: Spec §9
  - anchor: 10-multi-clause-def
    label: Spec §10
---

# Pattern matching

`case` — это выражение: оно проверяет значение по клаузам `when` сверху вниз и
возвращает тело первой совпавшей. Деструктуризация, guard'ы и матчеры типов —
часть языка, а не протокол поверх `if`.

## Матчеры типов

Имя типа в `when` совпадает, если значение этого типа:

```amber
def label(v):
  case v:
    when Int:
      "int"
    when Str:
      "str"
    when Float:
      "float"
    else:
      "other"

def probe():
  "#{label(5)}:#{label("hi")}:#{label(1.5)}:#{label(true)}"

probe()
# => "int:str:float:other"
```

## Порядок клауз и литералы

Клаузы проверяются по порядку, поэтому более узкий матчер ставят выше. Литералы
и диапазоны тоже допустимы в `when`:

```amber
def classify(n):
  case n:
    when 1..5:
      "low"
    when Int:
      "int-arm"
    else:
      "other"

def probe():
  "#{classify(3)}:#{classify(7)}"

probe()
# => "low:int-arm"
```

`classify(3)` попадает в диапазон `1..5`, а `7` — уже в общий `Int`.

## Деструктуризация map

`when` умеет разбирать map по именованным ключам и связывать их со свежими
именами. Это работает вместе с name-indifferent-доступом (`:key` и `"key"`):

```amber
def probe():
  p = Json.parse("{\"user_id\": 42, \"name\": \"Ada\"}")
  case p:
    when {user_id: id, name: n}:
      "#{n}##{id}"
    else:
      "no-match"

probe()
# => "Ada#42"
```

> [!tip]
> Матчер типа совпадает и с самим типом: `case Int: when Int:` истинно. Это
> удобно, когда вы диспетчеризуете по значениям-типам.

Связанная тема — как блоки участвуют в фильтрации и проекции:
[блоки и block suffix](guide:blocks).
