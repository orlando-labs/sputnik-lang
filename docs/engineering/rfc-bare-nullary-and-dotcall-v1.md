# Sputnik RFC: Bare-call для nullary-методов и chained callable-call `expr.()`

**Статус:** принято как проектное решение; правило расширено 2026-10-03
**Область:** surface syntax, postfix expressions, properties, callable values, dispatch/lowering, diagnostics
**Ключевые формы:** `obj.member`, `obj.member()`, `obj.member.()`, `obj.member.call()`, `obj.?.member`, `obj.?.member.?.()`

---

## 0. Краткое резюме

Sputnik принимает ограниченный **bare-call** для nullary-методов: member access вида `receiver.name` может выполнять implicit zero-argument send, если `name` резолвится в метод, принимающий ноль аргументов, в том числе с default-параметрами.

Одновременно вводится postfix callable-call segment:

```sputnik
expr.()
expr.(arg1, arg2)
expr.?.()
expr.?.(arg1, arg2)
```

Эта форма означает: **вызвать callable-значение, полученное выражением слева**, не теряя чейнинг и не требуя группировки выражения скобками в начале цепочки.

Итоговая триада:

```sputnik
obj.member       # read/query: property get OR implicit nullary method send
obj.member()     # explicit method send to member `member`
obj.member.()    # callable-call of the value produced by `obj.member`
```

`&target` сохраняет статус callable-reference контекста и никогда не вызывает target:

```sputnik
&Namespace.fn
&Class.method
&Class#method
```

---

## 1. Проблематика

В Sputnik уже существует красивая и строгая парадигма разделения:

```sputnik
prop size:
 @items.length

def size():
 @items.length
```

`prop` даёт field-like доступ:

```sputnik
collection.size
```

`def` даёт method-call доступ:

```sputnik
collection.size()
```

На уровне формальной модели это чисто: property — descriptor с getter/setter-поведением; method — ordinary callable member, вызываемый через call/send syntax.

Но на уровне пользовательского API появляется неудобство:

1. Потребитель API должен помнить, где член является `prop`, а где `def`.
2. Рефакторинг `prop -> def` или `def -> prop` меняет call sites.
3. Query-like методы вроде `size`, `empty?`, `present?`, `version`, `name`, `path`, `schema` выглядят тяжелее, чем должны.
4. В цепочках разница между `x.size` и `x.size()` создаёт шум, хотя семантически оба часто означают «получить значение».

Особенно проблемны API, где реализация естественно мигрирует между stored/computed value:

```sputnik
user.full_name
collection.size
settings.cache_dir
Build.version
```

Сегодня автор библиотеки должен выбрать surface shape заранее. Поздняя смена `prop` на `def` или обратно становится source-breaking даже если наблюдаемое значение не меняется.

---

## 2. Мотивация

### 2.1. Стабильность публичного API

Публичный API должен выражать семантическую роль члена, а не внутреннюю декларационную технику.

Если член концептуально является value/query, call site должен иметь устойчивую форму:

```sputnik
collection.size
user.full_name
Build.version
```

Внутренняя реализация может быть property descriptor:

```sputnik
class User:
 prop full_name:
  "#{@first} #{@last}"
```

или nullary method:

```sputnik
class User:
 def full_name():
  "#{@first} #{@last}"
```

Обе реализации должны быть refactor-compatible для read/query use case.

### 2.2. Ruby-like ergonomics без потери callable model

Sputnik уже ориентирован на Ruby-like object model, chaining, blocks и query-method suffixes `?` / `!`. Для такого языка естественна форма:

```sputnik
if users.empty?:
 render_empty_state()
```

Она читается лучше, чем:

```sputnik
if users.empty?():
 render_empty_state()
```

При этом Sputnik сохраняет explicit callable references через `&target`, поэтому можно отделить:

```sputnik
users.empty?     # invoke/read query
&Array#empty?    # reference to method, no invocation
```

### 2.3. Чистое различение invoke member vs invoke result

Главная опасность bare-call — callable-returning members.

Например:

```sputnik
factory.provider
```

может вернуть callable object или closure. Тогда нужен удобный способ вызвать это значение в chain без записи:

```sputnik
(factory.provider)()
```

Такая запись ломает локальность редактирования: чтобы добавить вызов, нужно возвращать каретку к началу выражения и ставить группирующие скобки.

Поэтому вводится Elixir-style postfix callable-call:

```sputnik
factory.provider.()
```

Он означает именно:

```sputnik
(factory.provider)()
```

но сохраняет chain-local editing.

---

## 3. Нормативное решение

### 3.1. Bare-call для nullary member methods

Member access:

```sputnik
receiver.name
```

разрешает implicit invocation, если `name` резолвится в метод, принимающий ноль аргументов.

```sputnik
class Collection:
 def size():
  @items.length

collection.size    # implicit nullary send: collection.size()
collection.size()  # explicit nullary send
```

Bare-callable method — метод, которому можно передать ноль аргументов.
Это включает пустую сигнатуру, default positional/keyword параметры,
`*args`, `**kwargs` и optional `&block`. Rest-параметры получают пустые
коллекции, block получает `null`; defaults вычисляются обычным binder'ом
при каждом вызове, в том же порядке и с теми же проверками, что при `name()`.

```sputnik
def format(mode = :short):
 ...

value.format       # valid: same invocation and defaults as value.format()
value.format()     # valid explicit call
```

Обязательный positional/keyword/named-callable параметр без default
по-прежнему делает bare-вызов ошибкой `AMB_BARE_NON_NULLARY` / `ArgumentError`.
Ошибки defaults, type hooks и тела метода распространяются как при explicit call.
Требующий block typed `&block as Fn[...]` сохраняет обычную entry-проверку:
без block оба написания дают `ArgumentError` (`AMB_BLOCK_REQUIRED`).

Изменение 2026-10-03 снимает ограничение на синтаксически пустую сигнатуру:
оно не предотвращало побочные эффекты (`!`-методы уже разрешены bare),
но заставляло добавление необязательного параметра ломать call sites.
Bare-форма теперь отражает допустимую арность вызова, а не форму декларации.

### 3.2. Explicit method call остаётся explicit method call

Форма:

```sputnik
receiver.name()
```

всегда означает explicit method send to selector `:name`.

Она не должна означать call of returned property value.

Если `name` является property, то:

```sputnik
receiver.name()
```

должно давать diagnostic:

```text
E_PROPERTY_CALLED_AS_METHOD
property `name` is not a method; use `receiver.name` or `receiver.name.()` if the property value is callable
```

Для вызова значения, возвращённого property или implicit-nullary member, используется:

```sputnik
receiver.name.()
```

### 3.3. Chained callable-call `expr.()`

Вводится postfix segment:

```sputnik
expr.()
expr.(arg1, arg2)
expr.(keyword: value)
expr.(*args, **kwargs)
```

Семантика:

```sputnik
expr.(args...)
```

эквивалентна:

```sputnik
(expr)(args...)
```

но является chain-preserving postfix form.

Пример:

```sputnik
factory.provider.().configure().start()
```

означает:

```sputnik
(factory.provider)().configure().start()
```

### 3.4. Safe callable-call `expr.?.()`

Вводится safe variant:

```sputnik
expr.?.()
expr.?.(arg1, arg2)
```

Семантика:

```sputnik
expr.?.(args...)
```

если `expr == null`, результат `null`; иначе вызывается callable value:

```sputnik
tmp = expr
if tmp == null:
 null
else:
 tmp(args...)
```

Пример:

```sputnik
factory.?.provider.?.().configure()
```

Здесь:

1. `factory.?.provider` безопасно читает/вызывает query-member `provider`;
2. `.?.()` безопасно вызывает полученный callable, если он не `null`;
3. дальнейшая цепочка продолжается от результата.

### 3.5. `.call()` остаётся ordinary method send

Форма:

```sputnik
expr.call()
```

не является специальным синтаксисом. Это обычный method send selector `:call`.

Это важно, потому что:

```sputnik
factory.provider.call()
```

значит:

1. вычислить `factory.provider`;
2. отправить результату метод `call()`.

А:

```sputnik
factory.provider.()
```

значит:

1. вычислить `factory.provider`;
2. вызвать результат через общий callable protocol / `HCall`.

Для ordinary callable objects эти формы могут быть наблюдаемо эквивалентны, но lowering различается:

```text
expr.call()  -> HSend(expr, :call, [])
expr.()      -> HCall(expr, [])
```

### 3.6. Callable references не меняются

`&target` — отдельный syntactic reference context. Он не производит invocation, даже если target является nullary method.

```sputnik
Build.version       # property get OR implicit nullary class-side send
&Build.version      # callable reference to class-side method, no invocation
&Build#version      # unbound instance method reference, no invocation
```

Если target является property getter/setter, callable reference для property остаётся отдельной будущей темой и не появляется автоматически в рамках этого решения.

### 3.7. `prop` сохраняет самостоятельную роль

После принятия bare-nullary `prop` перестаёт быть единственным способом получить field-like read syntax, но не становится ненужным.

`prop` нужен для:

1. assignment syntax;
2. read-write descriptors;
3. write-only descriptors;
4. validation/normalization on assignment;
5. future property metadata;
6. cached/lazy/observable properties;
7. property-specific reflection;
8. stable descriptor-level MOP.

Пример:

```sputnik
class Account:
 prop balance:
  get:
   @balance

  set(value):
   amount = Decimal(value)
   if amount < 0:
    raise ValueError("negative balance")
   @balance = amount

account.balance = 100
account.balance
```

Nullary `def` не становится assignable:

```sputnik
class Account:
 def balance():
  @balance

account.balance = 100  # invalid; no property setter
```

---

## 4. Surface syntax table

| Form | Meaning | Lowering intuition |
|---|---|---|
| `obj.member` | property get OR implicit nullary method send OR ordinary readable member | `HPropGet` / `HSend0Implicit` / member read |
| `obj.member()` | explicit method send | `HSend(obj, :member, [])` |
| `obj.member(arg)` | explicit method send with arguments | `HSend(obj, :member, [arg])` |
| `obj.member.()` | call value produced by `obj.member` | `HCall(HMemberReadOrImplicitSend(obj, :member), [])` |
| `obj.member.(arg)` | call value produced by `obj.member` with args | `HCall(..., [arg])` |
| `obj.member.call()` | ordinary send `:call` to value produced by `obj.member` | `HSend(..., :call, [])` |
| `&Class#member` | unbound instance method reference | `HUnboundMethodRef(Class, :member)` |
| `&Class.member` | class-side callable reference | `HCallableRef(...)` |
| `obj.?.member` | safe property get OR safe implicit nullary send | `HSafePropGet` / `HSafeSend0Implicit` |
| `obj.?.member()` | safe explicit method send | `HSafeSend(obj, :member, [])` |
| `obj.member.?.()` | safe call of returned callable value | `HSafeCall(...)` |

---

## 5. Parsing and grammar notes

### 5.1. New postfix segment

The postfix grammar receives one additional segment family:

```ebnf
PostfixSegment ::=
    "." Identifier CallArgs?
  | "." "(" ArgList? ")"
  | ".?." Identifier CallArgs?
  | ".?." "(" ArgList? ")"
  | "[" Expr "]"
  | ...
```

Examples:

```sputnik
expr.()
expr.(x, y)
expr.?.()
expr.?.(x, y)
```

The form is unambiguous because after `.` the parser sees `(` rather than an identifier.

### 5.2. `obj.member()` vs `obj.member.()`

The parser must preserve the distinction syntax-faithfully.

```sputnik
obj.member()
```

is a method-call segment.

```sputnik
obj.member.()
```

is a member-read/implicit-send segment followed by callable-call segment.

The AST must not erase this distinction.

---

## 6. Resolution model

### 6.1. Member read / implicit nullary send

For:

```sputnik
receiver.name
```

resolution proceeds conceptually as:

1. If `name` resolves to a readable property, perform property get.
2. Else if `name` resolves to a zero-argument-callable method, perform implicit zero-argument send.
3. Else if `name` resolves to another readable member kind supported by the object model, perform ordinary read.
4. Else use static diagnostic or dynamic missing-member/method path depending on receiver knowledge.

Property/method conflicts for the same selector remain forbidden by conservative conflict policy.

### 6.2. Explicit method call

For:

```sputnik
receiver.name(args...)
```

resolution must target a method/sendable selector `:name`.

If the selected member is a property, diagnostic is preferred over call-of-property-result.

### 6.3. Callable-call segment

For:

```sputnik
expr.(args...)
```

resolution first evaluates `expr`, then checks the resulting value against the callable protocol.

If the value is not callable, runtime raises:

```text
TypeError / E_NOT_CALLABLE
```

Static implementations may reject known-non-callable expressions earlier.

### 6.4. Dynamic dispatch and `method_missing`

Implicit nullary send participates in normal dispatch semantics.

For dynamic receivers:

```sputnik
obj.foo
```

if `foo` is not found as a property/readable member but may be a method, the dynamic path may perform zero-argument send and therefore may trigger:

```sputnik
method_missing(:foo)
```

This is intentional for DSL and open-world compatibility, but static receivers should prefer early diagnostics when a member is known not to exist.

---

## 7. HIR / lowering

Recommended HIR additions or conventions:

```text
HMemberRead(receiver, selector)
HPropGet(receiver, selector)
HSend0Implicit(receiver, selector)
HSafeSend0Implicit(receiver, selector)
HCall(callable, pos_args[], kw_args[], block?)
HSafeCall(callable, pos_args[], kw_args[], block?)
```

Alternatively, `HMemberRead` may be binder-resolved into existing `HSend`/`HCall`/property nodes while preserving enough metadata for diagnostics and disassembly.

Canonical lowerings:

```sputnik
obj.size
```

```text
HSend0Implicit(obj, :size)
```

if `size` is a nullary method.

```sputnik
obj.size()
```

```text
HSend(obj, :size, [], {}, null)
```

```sputnik
obj.size.()
```

```text
tmp = HMemberReadOrImplicitSend(obj, :size)
HCall(tmp, [], {}, null)
```

```sputnik
obj.size.call()
```

```text
tmp = HMemberReadOrImplicitSend(obj, :size)
HSend(tmp, :call, [], {}, null)
```

Safe lowering:

```sputnik
obj.?.provider.?.()
```

```text
tmp1 = null_guard(obj) ? null : HMemberReadOrImplicitSend(obj, :provider)
tmp2 = tmp1 == null ? null : HCall(tmp1, [], {}, null)
```

---

## 8. Native/frozen performance model

This feature does not require a slow path when type information is available.

### 8.1. Static/native path

If receiver type is known and the member resolves to a nullary method, compiler can lower:

```sputnik
collection.size
```

to the same direct call as:

```sputnik
collection.size()
```

Possible optimizations:

1. direct method entry call;
2. monomorphic inline cache;
3. devirtualized call in frozen-world profile;
4. inlining if method body is known and safe;
5. intrinsic lowering for stdlib primitives such as collection size.

### 8.2. Dynamic/open-world path

If receiver type is unknown, implicit nullary send is an ordinary dynamic send:

```text
SEND0_IMPLICIT receiver, :size
```

This is not meaningfully slower than:

```text
SEND receiver, :size, argc=0
```

and can share inline-cache infrastructure.

### 8.3. World mutation and invalidation

Because implicit nullary sends depend on method/property tables, world mutations that add/remove/replace methods or properties must invalidate affected caches.

This is not a new category of invalidation; it is the same dispatch-relevant mutation already needed for ordinary method sends, `method_missing`, properties, open classes and mixins.

---

## 9. Diagnostics

### 9.1. Required diagnostics

```text
E_BARE_NON_NULLARY_METHOD
method `name` requires arguments; call it with explicit arguments or take a callable reference explicitly
```

```text
E_PROPERTY_CALLED_AS_METHOD
property `name` is not a method; use `obj.name` or `obj.name.()` if the property value is callable
```

```text
E_AMBIGUOUS_MEMBER_KIND
member `name` cannot be both property and method in the same lookup surface
```

```text
E_NOT_CALLABLE
left side of `.()` is not callable
```

```text
E_DOT_CALL_TARGET_REQUIRED
`.()` must follow an expression; it cannot start an expression
```

### 9.2. Recommended warnings

Bare calls of zero-argument-callable `!` methods are ordinary sends and do not
produce a dedicated warning. The suffix communicates mutation convention, not
an alternate call grammar.

```text
W_EXPENSIVE_BARE_CALL
method marked expensive/io/async should be called with explicit parentheses
```

`W_EXPENSIVE_BARE_CALL` depends on future effect/cost annotations and is not required for v1.

### 9.3. Good fix-it suggestions

For:

```sputnik
factory.provider()
```

when `provider` is a property returning callable:

```text
Use `factory.provider.()` to call the property value.
```

For:

```sputnik
obj.format
```

when `format` has a required parameter without a default:

```text
Method `format` is not bare-callable because it requires arguments. Use `obj.format(...)`.
```

For:

```sputnik
cache.clear!
```

when `clear!` is zero-argument-callable, the expression is an ordinary send and
produces no diagnostic.

---

## 10. Examples

### 10.1. Refactoring property to method

Before:

```sputnik
class Collection:
 prop size:
  @items.length

collection.size
```

After:

```sputnik
class Collection:
 def size():
  @items.length

collection.size
```

Call sites do not change.

### 10.2. Explicit method call still works

```sputnik
collection.size
collection.size()
```

Both are valid when `size` accepts zero arguments, including via defaults.

The first is query/read syntax; the second is explicit invocation syntax.

### 10.3. Callable-returning property

```sputnik
class Factory:
 prop provider:
  | |: Service()

factory.provider       # returns callable
factory.provider.()    # calls returned callable
factory.provider()     # diagnostic: property is not a method
```

### 10.4. Callable-returning nullary method

```sputnik
class Factory:
 def provider():
  | |: Service()

factory.provider       # implicit call provider(), returns callable
factory.provider()     # explicit call provider(), returns callable
factory.provider.()    # implicit call provider(), then call returned callable
```

### 10.5. Ordinary `.call()`

```sputnik
factory.provider.call()
```

This sends `:call` to the value returned by `factory.provider`.

```sputnik
factory.provider.()
```

This invokes the value returned by `factory.provider` through the generic callable protocol.

### 10.6. Safe chain

```sputnik
service.?.factory.?.provider.?.().start()
```

Reading left to right:

1. safely read/call `factory` from `service`;
2. safely read/call `provider` from the factory;
3. safely call the returned callable provider;
4. call `start()` on the produced service.

---

## 11. Pros

### 11.1. Better API refactoring

The biggest benefit is that public query-like API no longer exposes whether implementation is `prop` or nullary `def`.

```sputnik
user.name
user.full_name
collection.size
```

can survive internal rewrites between descriptor and method forms.

### 11.2. Cleaner query syntax

Methods ending in `?` become visually natural:

```sputnik
if users.empty?:
 ...
```

This improves readability for predicates and cheap query methods.

### 11.3. Better chaining ergonomics

`expr.()` avoids disruptive grouping:

```sputnik
factory.provider.().configure().start()
```

instead of:

```sputnik
(factory.provider)().configure().start()
```

### 11.4. Callable references stay explicit

Because `&target` remains a separate context, bare-call does not steal the ability to refer to methods:

```sputnik
obj.size       # invoke/read
&Class#size    # reference
```

### 11.5. Performance model is straightforward

For statically known receivers, implicit nullary sends can lower to the same code as explicit zero-argument sends.

For dynamic receivers, the operation is an ordinary zero-argument send with inline-cache support.

### 11.6. Strong distinction between member-call and result-call

The accepted syntax gives a simple rule:

```sputnik
x.y()   # call member y
x.y.()  # call result of x.y
```

This is more precise than allowing `x.y()` to sometimes mean “call property result”.

---

## 12. Cons

### 12.1. Bare access can run user code

After this change:

```sputnik
obj.name
```

may execute code.

That code may allocate, compute, dispatch dynamically or trigger `method_missing`.

This is already true for properties, but the feature expands the surface where it can happen.

### 12.2. More semantic weight on naming conventions

The language cannot know whether a nullary method is cheap and pure.

Bad APIs may expose expensive or effectful methods as bare-callable:

```sputnik
socket.read
random.next
cache.clear!
```

The language should rely on style, lint and possibly future effect annotations.

### 12.3. `prop` vs `def` distinction becomes less visible at use sites

This is the point of the feature, but it also hides assignability and descriptor semantics.

A reader cannot tell from:

```sputnik
account.balance
```

whether `balance` is a property or nullary method.

If assignment exists:

```sputnik
account.balance = 10
```

then it must be property-backed. A nullary method alone is not assignable.

### 12.4. Parser and HIR gain one new postfix form

`expr.()` is simple but still a new form. It must be preserved through AST and diagnostics.

### 12.5. Dynamic `method_missing` gets broader reach

In dynamic contexts, `obj.foo` may now attempt zero-arg method dispatch and reach `method_missing(:foo)`.

This benefits DSLs but can make typos more dynamic unless static diagnostics or linting catch them.

---

## 13. Tradeoffs

### 13.1. Why not keep `obj.prop()` as call-result?

Because after bare-nullary it becomes ambiguous and misleading.

If `obj.name()` sometimes calls method `name`, sometimes calls result of property `name`, and sometimes competes with implicit nullary send, users cannot reason locally.

The accepted rule is sharper:

```sputnik
obj.name()   # method call
obj.name.()  # result call
```

### 13.2. Why not use only `.call()`?

`.call()` is useful and remains available, but it is ordinary method dispatch.

`expr.()` is generic callable invocation:

```text
expr.()     -> HCall(expr, [])
expr.call() -> HSend(expr, :call, [])
```

This matters for closures, native callable references, class objects and any value callable through the runtime callable protocol without exposing ordinary public method `call`.

### 13.3. Why include methods with defaults?

Bare member access already invokes ordinary methods, including mutating `!`
methods. Requiring a syntactically empty signature adds no effect guarantee.
Accepting every zero-argument-callable signature preserves call sites when
an API adds optional parameters. `obj.format` and `obj.format()` use identical
argument binding and default evaluation; `&obj.format` remains reference syntax.
Methods that require caller arguments still diagnose instead of returning a method.

### 13.4. Why allow explicit `obj.size()` too?

Because explicitness is sometimes useful:

1. to communicate that work is being done;
2. to avoid style debates in effectful code;
3. to preserve compatibility with users who prefer method-call shape;
4. to make `!` methods visually active.

Bare-call is accepted as ergonomic read/query syntax, not as a ban on parentheses.

### 13.5. Bare identifiers and implicit `self`

Resolution is lexical first. A local, parameter, capture, import or
module-level binding named `f` remains an ordinary value read and is never
invoked implicitly.

Only when no such binding exists and the current procedure has an object
receiver does bare `f` become the member read `self.f`. It therefore invokes a
zero-argument-callable method (or reads a property) through ordinary dynamic
linearization. Without an object receiver, an unresolved bare identifier is
an error. This preserves first-class lexical functions while allowing
controllers and mixin-heavy DSLs to use inherited APIs without repetitive
`self.` prefixes.

---

## 14. Style guidance

Recommended bare-call style:

```sputnik
collection.size
collection.empty?
user.full_name
Build.version
node.parent
path.dirname
```

Recommended action style:

```sputnik
cache.clear!
user.save!
db.connect
socket.read
random.next
clock.now
```

For a zero-argument-callable member, the bare form is idiomatic even when the
operation is:

1. mutating;
2. effectful;
3. expensive;
4. time-varying;
5. surprising as a field-like read;
6. semantically an action rather than a query.

`?` query methods are good candidates for bare-call if they are cheap and side-effect-free:

```sputnik
users.empty?
config.valid?
connection.open?
```

`!` methods should generally use the bare form; the suffix already makes the
mutation convention visible:

```sputnik
cache.clear!
record.save!
```

---

## 15. Compatibility impact

This is a compatibility-affecting change relative to the previous property-only model.

### 15.1. Source behavior changes

Previously:

```sputnik
obj.name
```

would not call ordinary method `name`.

After this RFC, it may call `name()` if the method is zero-argument-callable.

### 15.2. Property call-result change

Previously, a design could allow:

```sputnik
obj.prop()
```

to mean call of result returned by `obj.prop`.

This RFC rejects that interpretation and reserves:

```sputnik
obj.prop()
```

for method call syntax only.

Call-result must be written:

```sputnik
obj.prop.()
```

or, if grouping is preferred:

```sputnik
(obj.prop)()
```

### 15.3. Migration assistance

Compiler should provide targeted fix-its:

```sputnik
obj.prop()
```

if `prop` is known property returning callable:

```sputnik
obj.prop.()
```

if user wanted method call but `prop` is property:

```sputnik
# define `def prop():` or call an actual method
```

---

## 16. Conformance tests

### 16.1. Positive tests

```sputnik
class Box:
 def size():
  10

box = Box()
assert box.size == 10
assert box.size() == 10
```

```sputnik
class Box:
 prop provider:
  | |: 42

box = Box()
assert box.provider.() == 42
```

```sputnik
class Box:
 def provider():
  | |: 42

box = Box()
assert box.provider.() == 42
assert box.provider().() == 42
```

```sputnik
class Box:
 prop value:
  10

box = Box()
assert box.value == 10
```

```sputnik
class Build:
 class_method def version():
  "1.0"

assert Build.version == "1.0"
assert Build.version() == "1.0"
```

```sputnik
maybe_provider = null
assert maybe_provider.?.() == null
```

The positive suite also covers all-default positional/keyword signatures,
rest/keyword-rest and optional block parameters, inherited and implicit-self
members, class-side calls, safe navigation, repeated cache hits, defaults
with effects evaluated exactly once, and dependencies on earlier defaults.

### 16.2. Negative tests

```sputnik
class Box:
 def format(mode):
  "x"

box = Box()
box.format
### E_BARE_NON_NULLARY_METHOD or equivalent diagnostic
```

```sputnik
class Box:
 prop provider:
  | |: 42

box = Box()
box.provider()
### E_PROPERTY_CALLED_AS_METHOD
```

```sputnik
x = 10
x.()
### E_NOT_CALLABLE
```

```sputnik
.()
### E_DOT_CALL_TARGET_REQUIRED
```

```sputnik
class Box:
 prop name:
  "a"

 def name():
  "b"
### E_AMBIGUOUS_MEMBER_KIND / E_PROP_NAME_CONFLICT
```

---

## 17. Open questions / resolved decisions

1. Whether instance-bound method reference should receive a dedicated spelling such as `obj.&method`.
2. Whether property getter/setter callable references should become first-class and what spelling they should use.
3. Whether effect annotations should influence bare-call diagnostics.
4. Whether formatter should normalize query-like explicit calls `obj.empty?()` to `obj.empty?`.
5. Whether top-level nullary functions should ever receive bare-call syntax. This RFC says no.
6. Resolved: a zero-argument-callable `!` method accepts ordinary bare-call
   syntax without a dedicated warning or strict-profile error.
7. Whether `expr.()` should support block suffix directly:

```sputnik
provider.() |x|:
 ...
```

or only ordinary call arguments in v1.

---

## 18. Final accepted rule

Sputnik adopts this rule set:

```sputnik
obj.member       # property get OR implicit nullary method send
obj.member()     # explicit method send only
obj.member.()    # call returned callable value
obj.member.call()# ordinary `call` method send to returned value
```

Safe variants:

```sputnik
obj.?.member
obj.?.member()
obj.?.member.?.()
```

Callable references:

```sputnik
&Namespace.fn
&Class.method
&Class#method
```

never perform actual invocation.

`prop` remains the descriptor mechanism for assignability, validation, getter/setter behavior, property reflection and future descriptor-level features.

The accepted bare-call rule includes all-default, rest and optional-block signatures; required arguments remain an error.
