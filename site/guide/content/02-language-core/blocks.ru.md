---
id: blocks
title: Блоки, лямбды и block suffix
summary: Блок передаётся вызову отдельным каналом и становится явным замыканием.
category: language-core
order: 20
prerequisites: [overview]
related: [functions, pattern-matching]
status: draft
spec_refs:
  - anchor: 4-postfix-выражения-чейнинг-и-block-suffix
    label: Spec §4
  - anchor: 5-блоки-лямбда-аргументы-и-placeholders
    label: Spec §5
---

# Блоки, лямбды и block suffix

Блок — это фрагмент кода, который передаётся вызову **отдельным каналом**, а не
позиционным аргументом. Это фирменная черта Sputnik: цепочки данных остаются в
одну читаемую линию, без callback-лесов.

## Block suffix

Блок пишется сразу после вызова, с параметрами в `|...|`:

```sputnik
def probe():
  xs = [0, 1, 2, 3]
  xs.filter_map |x|:
    if x % 2 == 0:
      x * 10
    else:
      null

probe()
# => [0, 20]
```

`filter_map` вызывает блок для каждого элемента; `null` из блока отбрасывает
элемент, любое другое значение попадает в результат. Блок здесь — обычное
выражение с телом.

## Лямбда-литералы

Тот же блок можно оформить как самостоятельное значение — лямбду `|params|: body`.
Лямбда захватывает лексическое окружение и является полноценным callable:

```sputnik
def probe():
  inc = |x|: x + 1
  add = |a, b|: a + b
  n = 100
  cap = |x|: x + n
  make = |k|: |x|: x * k
  triple = make(3)
  "#{inc(10)}:#{add(3, 4)}:#{cap(5)}:#{triple(7)}:#{(|x|: x * x)(6)}"

probe()
# => "11:7:105:21:36"
```

Здесь `cap` захватывает `n`, а `make` возвращает лямбду — замыкание над `k`.
Ведущий `|` всегда начинает лямбду, поэтому синтаксис однозначен и не конфликтует
с инфиксным побитовым `|`.

## Передача блока через `&`

Функция принимает блок явным параметром `&blk` и передаёт его дальше как `&blk`.
Внутри тела `blk` — обычный callable; `blk == null`, если блок не передали:

```sputnik
def each(xs, &blk):
  i = 0
  n = xs.length
  while i < n:
    blk(xs[i])
    i = i + 1
  xs

def my_map(xs, &blk):
  out = []
  each(xs) |x|:
    out.push!(blk(x))
  out

def probe():
  doubled = my_map([1, 2, 3]) |x|: x * 2
  total = 0
  each(doubled) |x|:
    total = total + x
  "#{doubled[0]}:#{doubled[1]}:#{doubled[2]}:sum=#{total}"

probe()
# => "2:4:6:sum=12"
```

> [!note]
> `&blk` — это блок как значение. Обычные позиционные аргументы и block-канал не
> смешиваются: у вызова ровно один блок, и он всегда идёт последним.

Дальше: [функции, методы и классы](guide:functions) используют блоки как
строительный материал для API коллекций.
