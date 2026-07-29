# Amber v20.9 — Named Multiblock Suffix и именованные callable-параметры

**Статус:** Draft language addition
**Целевая версия:** Amber v20.9
**Дата проекта:** 2026-07-28
**Область:** surface syntax, parameter binding, call semantics, AST/HIR lowering, diagnostics, formatter
**Зависимости:** существующие `&target` callable references, канонический callable-call `fn(args...)`, keyword arguments, block suffix, trailing block-pass `&name`

---

## 0. Резюме решения

Amber v20.9 вводит два связанных расширения:

1. **именованные callable-параметры** в сигнатуре:

```amber
def request(url, &success:, &error:):
 ...
```

2. **named multiblock suffix** `with:` в месте вызова:

```amber
request(url) with:
 success |response|:
  render(response)

 error |problem|:
  report(problem)
```

`with:` не создаёт несколько нативных block channels. Каждая его entry создаёт обычное closure/callable-значение и передаёт его как именованный keyword-аргумент.

Следующая форма является нормативной для передачи уже существующих функций или методов:

```amber
request(
 url,
 success: &success_handler,
 error: &error_handler
)
```

Здесь `&success_handler` и `&error_handler` являются **callable reference expressions**. Отсутствие `&` означало бы обычное чтение значения binding'а, а не извлечение callable reference из function/method target.

Если локальная переменная уже содержит callable-значение, повторный `&` не требуется:

```amber
success_callback = &Handlers.success
error_callback = &Handlers.error

request(
 url,
 success: success_callback,
 error: error_callback
)
```

---

## 1. Мотивация

Для API с несколькими callbacks без языковой поддержки приходится строить mutable provider/registrar object:

```amber
class CallbackProvider:
 attr var success
 attr var error

 def success(&block):
  @success = block

 def error(&block):
  @error = block
```

и затем выполнять конфигурационный блок:

```amber
provider = CallbackProvider()
configure(provider)
provider.success(result)
```

Такая модель имеет лишние сущности и лишнюю фазу mutation:

- callback provider не является частью предметной области;
- callback names маскируются под method sends;
- требуется mutable регистрация closures;
- ошибки отсутствующих callbacks обнаруживаются поздно;
- сигнатура вызываемого метода не описывает callback contract;
- tooling видит общий DSL-блок, а не набор именованных callable arguments.

Amber уже имеет все необходимые семантические примитивы:

- `&target` создаёт callable reference;
- block body создаёт closure;
- callable вызывается через `fn(args...)`;
- keyword arguments имеют именованную binding-модель;
- block suffix привязывается к ближайшему call segment.

Поэтому multiblock должен быть синтаксическим слоем над **именованными callable keyword arguments**, а не новым параллельным механизмом dispatch.

---

## 2. Цели и нецели

### 2.1. Цели

Расширение должно:

- позволять объявить несколько именованных callback-параметров;
- позволять передать несколько inline blocks без provider object;
- сохранять обязательность `&target` при извлечении callable reference;
- сохранять closures и callable references как ordinary first-class values;
- не менять базовый call ABI;
- не вводить signature-directed parsing call-site кода;
- поддерживать статические diagnostics для missing, duplicate и non-callable arguments;
- сохранять существующий единственный anonymous block channel;
- хорошо работать с keyword spread, reflection и dynamic dispatch.

### 2.2. Нецели

Amber v20.9 не вводит:

- специальные значения или control flow для имён `success`, `error`, `item`, `commit` и т. п.;
- guarantee, что callback будет вызван ровно один раз;
- автоматическое преобразование callback API в `Result` или exceptions;
- несколько нативных block slots в VM ABI;
- syntax-directed async/await semantics для callbacks;
- instance-bound reference spelling `&obj.method`;
- произвольные expressions после prefix `&`;
- скрытый provider object;
- signature-dependent reinterpretation обычного block body.

---

## 3. Существующие инварианты Amber

Это дополнение сохраняет следующие правила языка.

### 3.1. Callable reference

```amber
&target
```

создаёт immutable callable reference object и не вызывает target.

Примеры:

```amber
&success_handler
&Http.handle_success
&User.find
&User#full_name
```

Существующие ограничения `&target` сохраняются:

```amber
&foo()       # invalid: target не может быть call expression
&(foo + bar) # invalid: произвольное expression запрещено
&obj.method  # invalid в v20.9: bound instance reference не введён
```

### 3.2. Callable invocation

Callable-значение вызывается канонически:

```amber
callback(value)
```

или через chain-preserving dot-call:

```amber
expr.(value)
expr.?.(value)
```

### 3.3. Anonymous block channel

Существующий trailing block suffix и trailing block-pass остаются отдельным каналом:

```amber
items.map |item|:
 transform(item)
```

```amber
mapper = &transform
items.map(&mapper)
```

Trailing `&name` в argument list означает **block-pass**, а не обычный positional argument.

### 3.4. Keyword values

В форме:

```amber
success: &success_handler
```

`&success_handler` разбирается как expression, являющееся значением keyword argument `success`.

Это принципиально отличается от:

```amber
request(url, &handler)
```

где trailing `&handler` направляется в anonymous block channel.

---

## 4. Терминология

| Термин | Значение |
|---|---|
| Callable value | Любое значение, которое может быть вызвано через общий `HCall`/`CALL` protocol. |
| Callable reference | Callable object, созданный prefix-формой `&target`. |
| Closure | Callable object, созданный block/lambda body и захватывающий lexical environment. |
| Anonymous block channel | Существующий единственный optional block slot вызова. |
| Named callable parameter | Keyword-параметр, объявленный формой `&name:` и обязанный содержать callable value. |
| Multiblock suffix | `with:`-suite после call segment, содержащий несколько named block entries. |
| Named block entry | Одна entry внутри `with:`, создающая closure для соответствующего keyword name. |

---

## 5. Именованные callable-параметры

### 5.1. Обязательная форма

```amber
def request(url, &success:, &error:):
 ...
```

`&success:` и `&error:` являются required keyword parameters с callable contract.

В теле callable-параметры являются обычными lexical bindings:

```amber
def request(url, &success:, &error:):
 response = Http.get(url)

 if response.ok?:
  success(response)
 else:
  error(response.error)
```

### 5.2. Опциональная форма

```amber
def request(url, &success:, &error: null):
 ...
```

Форма `&name: null` объявляет nullable optional callable parameter.

```amber
def request(url, &success:, &error: null):
 response = Http.get(url)

 if response.ok?:
  success(response)
 else:
  error.?.(response.error)
```

Default может быть callable reference:

```amber
def request(url, &success:, &error: &Logger.report_error):
 ...
```

### 5.3. Type annotation

Named callable parameter использует обычное место type annotation keyword-параметра:

```amber
def request(url, &success as SuccessHandler:, &error as ErrorHandler:):
 ...
```

Конкретная структура callable type определяется активным typed profile. В untyped core marker `&` всё равно обеспечивает runtime callable validation.

### 5.4. Параметрическая семантика

Нормативно:

1. `&name:` занимает keyword namespace имени `name`.
2. В одной сигнатуре разрешено несколько named callable parameters.
3. Они участвуют в duplicate/missing keyword checks как ordinary keyword parameters.
4. После binding/default evaluation каждое non-null значение проверяется через callable protocol.
5. Не-callable значение даёт `TypeError`.
6. Required `&name:` не принимает `null`.
7. `&name: null` принимает callable или `null`.
8. При наличии type annotation сначала применяется обычный typecheck, затем callable check, если typed profile не доказал callable contract статически.
9. Callable check выполняется до auto-assign commit и до исполнения тела.
10. В теле `name` — обычное read-only parameter binding.

### 5.5. Ограничения v20.9

Не разрешаются:

```amber
def f(&@success:)       # invalid: auto-assign marker не комбинируется с &
def f(&@@success:)      # invalid
def f(&(a, b):)         # invalid: pattern вместо single name
def f(&success: *rest)  # invalid
```

Для хранения callback в поле используется явное присваивание:

```amber
class Subscription:
 def init(&on_item:, &on_error: null):
  @on_item = on_item
  @on_error = on_error
```

### 5.6. Сосуществование с anonymous block parameter

Существующий bare block parameter сохраняется:

```amber
def around(&block):
 block()
```

Named callable parameters не считаются anonymous block parameters, поэтому формально допустима сигнатура:

```amber
def operation(&success:, &error:, &body):
 ...
```

При этом:

- `&success:` и `&error:` — keyword callable parameters;
- финальный `&body` — единственный anonymous block parameter;
- bare `&body` по-прежнему обязан быть последним параметром;
- bare anonymous block parameters по-прежнему допускаются не более одного.

---

## 6. Передача существующих callable targets

### 6.1. Нормативная форма

```amber
def success_handler(response):
 render(response)

def error_handler(problem):
 report(problem)

request(
 url,
 success: &success_handler,
 error: &error_handler
)
```

Prefix `&` обязателен, когда source spelling обозначает function/method target, из которого требуется получить callable reference.

### 6.2. Namespace и class-side methods

```amber
request(
 url,
 success: &Handlers.success,
 error: &Handlers.error
)
```

### 6.3. Unbound instance method

```amber
request(
 url,
 success: &Renderer#render,
 error: &Renderer#render_error
)
```

Такой reference ожидает receiver первым positional argument при последующем вызове. API author обязан согласовать фактическую callback arity.

### 6.4. Уже материализованный callable

```amber
success_callback = &Handlers.success
error_callback = &Handlers.error

request(
 url,
 success: success_callback,
 error: error_callback
)
```

Здесь второй `&` не нужен: locals уже содержат callable values.

### 6.5. Различие с trailing block-pass

```amber
request(url, success: &success_handler)
```

означает: передать callable reference как keyword value `success`.

```amber
request(url, &success_callback)
```

означает: передать содержимое local `success_callback` в anonymous block channel.

Trailing block-pass не удовлетворяет `&success:` и не создаёт именованный callback.

### 6.6. Значение без `&`

```amber
request(url, success: success_handler)
```

означает обычное чтение lexical binding `success_handler`.

Форма валидна только если это binding уже содержит callable value. Она не должна неявно извлекать function reference и не должна превращать bare identifier read в скрытый call/reference operation.

---

## 7. Named multiblock suffix `with:`

### 7.1. Базовая форма

```amber
request(url) with:
 success |response|:
  render(response)

 error |problem|:
  report(problem)
```

Каждая entry создаёт closure и передаёт его под именем entry.

Наблюдаемое значение вызова эквивалентно обычному keyword call:

```amber
success_callback = <closure |response|:
 render(response)
>

error_callback = <closure |problem|:
 report(problem)
>

request(
 url,
 success: success_callback,
 error: error_callback
)
```

Angle-bracket spelling выше является только псевдокодом lowering и не вводится в surface syntax.

### 7.2. Placeholder form

```amber
request(url) with:
 success:
  render(_1)

 error:
  report(_1)
```

Для entry без `|...|` действуют обычные block placeholder rules:

- `_1`, `_2`, ... доступны только внутри этой entry;
- arity определяется максимальным placeholder index;
- индексы должны быть плотными;
- placeholders read-only.

### 7.3. Pattern parameters

Named block entry использует обычные block patterns:

```amber
consume(stream) with:
 item |{id:, payload:}|:
  process(id, payload)

 failed |Error(message:)|:
  log(message)
```

Pattern mismatch при вызове closure даёт обычный `MatchError`.

### 7.4. Произвольные имена

Язык не назначает специальной семантики именам entries:

```amber
transaction(db) with:
 body:
  update_records()

 commit |receipt|:
  log(receipt)

 rollback |problem|:
  compensate(problem)
```

```amber
stream(socket) with:
 item |packet|:
  consume(packet)

 closed:
  finalize()

 failed |problem|:
  report(problem)
```

### 7.5. `with` как contextual keyword

`with` не становится глобально зарезервированным identifier.

Он распознаётся как multiblock suffix только в форме:

```text
<completed call segment> with:
 <indented named block entries>
```

В остальных позициях `with` может оставаться обычным именем, если это допускается общей identifier grammar.

### 7.6. Привязка к вызову

Как и ordinary block suffix, `with:` относится к ближайшему завершённому call segment слева.

```amber
client.request(url) with:
 success:
  use(_1)
 error:
  report(_1)
```

`with:` не может применяться к plain value read, property read или произвольному expression без call segment.

Невалидно:

```amber
callback with:
 success:
  null
```

Валидно:

```amber
callback() with:
 success:
  null
```

если вызываемый callable принимает соответствующий named callable parameter.

### 7.7. Минимальная entry count

`with:` обязан содержать хотя бы одну named block entry.

```amber
request(url) with:
 # invalid: empty multiblock suite
```

### 7.8. Duplicate names

Повтор имени внутри одного `with:` является compile-time error:

```amber
request(url) with:
 success:
  first()

 success:
  second()
```

### 7.9. Конфликт с explicit keyword

Невалидно:

```amber
request(url, success: &existing_handler) with:
 success:
  inline_handler(_1)
```

Это duplicate keyword argument `success`.

### 7.10. Конфликт с keyword spread

```amber
request(url, **options) with:
 success:
  handle(_1)
```

Если `options` после keyword normalization содержит `success`, вызов завершается `KeywordArgumentError` до dispatch.

### 7.11. Сосуществование с anonymous block suffix

В v20.9 один call site не может одновременно иметь ordinary anonymous block suffix и `with:` suffix.

Невалидно:

```amber
operation() |value|:
 use(value)
with:
 success:
  done()
```

API, которому нужен основной body и дополнительные callbacks, должен дать body имя:

```amber
transaction(db) with:
 body:
  update_records()

 commit:
  log("committed")

 rollback |problem|:
  report(problem)
```

### 7.12. Сосуществование с trailing block-pass

В v20.9 вызов с trailing anonymous block-pass не может дополнительно иметь `with:`:

```amber
operation(&body) with:
 success:
  done()
# invalid
```

Эквивалентный API следует вызвать либо через ordinary named callable arguments, либо полностью через `with:` entries.

---

## 8. Нормативная грамматика

Нотация ниже является проектной EBNF и должна быть согласована с фактической grammar naming reference parser.

### 8.1. Параметры

```ebnf
NamedCallableKeywordParameter
  ::= "&" Identifier TypeAnnotation? ":" DefaultExpression?

AnonymousBlockParameter
  ::= "&" Identifier
```

Различение контекстно и детерминировано:

- `&name:` — named callable keyword parameter;
- `&name: expr` — optional/defaulted named callable keyword parameter;
- `&name` без `:` — existing anonymous block parameter.

### 8.2. Multiblock suffix

```ebnf
MultiblockSuffix
  ::= "with" ":" NEWLINE INDENT NamedBlockEntry+ DEDENT

NamedBlockEntry
  ::= Identifier ExplicitBlockParameters? ":" Suite

ExplicitBlockParameters
  ::= "|" BlockPatternList? "|"
```

`Suite` использует существующие one-line/multiline block suite rules.

### 8.3. Call postfix integration

Концептуально:

```ebnf
CallExpression
  ::= PostfixExpression CallSegment MultiblockSuffix?
```

Parser обязан создавать multiblock suffix только после syntactically complete call segment.

### 8.4. Keyword-value callable reference

В keyword value position:

```ebnf
KeywordArgument
  ::= Identifier ":" Expression
```

поэтому:

```amber
success: &success_handler
```

разбирается как:

```text
KeywordArgument(
 name = :success,
 value = CallableReference(target = success_handler)
)
```

Это не block-pass production.

---

## 9. Порядок вычисления

Для вызова:

```amber
receiver.request(
 url_expr(),
 mode: mode_expr(),
 **options_expr()
) with:
 success |response|:
  use(response)

 error |problem|:
  report(problem)
```

нормативный порядок:

1. вычислить receiver/callee;
2. вычислить explicit positional arguments слева направо;
3. вычислить explicit keyword values слева направо;
4. вычислить keyword spread operands и построить kwargs view по существующим правилам;
5. создать closure objects для `with:` entries сверху вниз;
6. нормализовать и объединить keyword names;
7. обнаружить duplicate keyword names;
8. выполнить call preflight;
9. связать explicit arguments;
10. вычислить defaults слева направо по сигнатуре;
11. выполнить type checks;
12. выполнить named callable checks;
13. выполнить auto-assign commit;
14. dispatch и исполнение тела.

Создание closure не исполняет его body.

Если duplicate обнаружен между keyword spread и multiblock entry, ни один multiblock body не выполняется.

---

## 10. Runtime semantics

### 10.1. Callable contract

Значение удовлетворяет named callable parameter, если общий callable protocol принимает `HCall`/`CALL` для этого value.

Это включает как минимум:

- closures;
- callable references;
- lambdas;
- class objects;
- runtime objects с поддерживаемым callable protocol.

Marker `&name:` не ограничивает argument только объектами, созданными prefix `&`. Он требует **callable value**, а не конкретный representation kind.

### 10.2. Missing named callable

```amber
def request(url, &success:, &error:):
 ...

request(url, success: &handle)
```

даёт missing keyword/named block diagnostic для `error`.

Runtime class остаётся совместимой с existing keyword binding model: `KeywordArgumentError` или существующий более точный missing-keyword subtype, если он уже определён implementation.

### 10.3. Non-callable argument

```amber
request(url, success: 42, error: &handle_error)
```

даёт:

```text
TypeError: named callable argument `success` must be callable; got Int
```

### 10.4. Optional callback

```amber
def request(url, &success:, &error: null):
 ...
```

позволяет:

```amber
request(url, success: &handle_success)
```

и:

```amber
request(
 url,
 success: &handle_success,
 error: null
)
```

Если nullable behavior не объявлен через exact default `null` или nullable type annotation, explicit `null` считается non-callable error.

### 10.5. Callback invocation count

Named callable parameter может быть вызван:

- ни разу;
- один раз;
- несколько раз;
- синхронно;
- позднее, если closure/reference был сохранён;
- из другого task/strand только если обычные isolation/shareability rules разрешают это.

Язык не связывает имя `success` с exactly-once semantics.

### 10.6. Exceptions и control flow

Исключение из callback распространяется по обычным правилам вызова.

`return` внутри inline named block завершает сам closure/block, а не внешний caller.

`break` внутри named block не получает специального значения и подчиняется общим ограничениям block control flow.

---

## 11. AST

Parser должен сохранять surface distinction.

### 11.1. Named callable parameter

Рекомендуемый node:

```text
AstNamedCallableParameter(
 name,
 type_annotation?,
 default_expr?,
 nullable_by_null_default,
 span
)
```

Он не должен немедленно стираться в generic keyword parameter, потому что tooling и diagnostics должны видеть callable intent.

### 11.2. Multiblock suffix

Рекомендуемые nodes:

```text
AstMultiblockSuffix(
 entries: [AstNamedBlockEntry...],
 span
)
```

```text
AstNamedBlockEntry(
 name,
 parameters?,
 body,
 span
)
```

### 11.3. Callable reference keyword value

Существующий AST callable reference сохраняется:

```text
AstKeywordArgument(
 name = :success,
 value = AstCallableReference(target = success_handler)
)
```

Parser не должен преобразовывать `success: &handler` в block-pass node.

---

## 12. HIR lowering

### 12.1. Базовое lowering

```amber
request(url) with:
 success |response|:
  render(response)

 error |problem|:
  report(problem)
```

может понижаться в:

```text
HCall(
 callee = request,
 positional = [url],
 keywords = [
  HKeywordArg(:success, HClosure(params=[response], body=...)),
  HKeywordArg(:error, HClosure(params=[problem], body=...))
 ],
 block = null,
 source_multiblock = true
)
```

### 12.2. Signature lowering

`&success:` понижается в keyword parameter metadata с callable requirement:

```text
HParameter(
 name = :success,
 kind = keyword_required,
 constraint = callable,
 source_named_callable = true
)
```

`&error: null`:

```text
HParameter(
 name = :error,
 kind = keyword_optional,
 default = HConstNull,
 constraint = callable_or_null,
 source_named_callable = true
)
```

### 12.3. ABI

Reference implementation не обязана добавлять новый call ABI slot.

Named multiblock entries передаются через существующую keyword argument representation.

Anonymous block channel остаётся отдельным existing slot.

### 12.4. Optional optimized representation

Compiler/VM вправе использовать специализированные opcodes или metadata:

```text
MAKE_CLOSURE
CALL_KW
CALL_KW_MULTIBLOCK
CHECK_CALLABLE
```

но наблюдаемое поведение должно совпадать с ordinary keyword call lowering.

---

## 13. Reflection, `send` и `method_missing`

### 13.1. Reflective send

Named callable arguments могут передаваться через ordinary reflective keyword path:

```amber
send(
 client,
 :request,
 url,
 success: &success_handler,
 error: &error_handler
)
```

`with:` может syntactically следовать за `send(...)`, поскольку это completed call segment:

```amber
send(client, :request, url) with:
 success:
  use(_1)
 error:
  report(_1)
```

Lowering остаётся ordinary keywords + closures.

### 13.2. `method_missing`

Если dispatch попадает в `method_missing`, named callable arguments передаются как ordinary keywords согласно существующей reflective dispatch semantics.

Multiblock identity может сохраняться только как source/tooling metadata и не обязана быть доступна runtime method body.

---

## 14. Formatter

Canonical formatter spelling:

```amber
request(
 url,
 success: &success_handler,
 error: &error_handler
)
```

Inline multiblock:

```amber
request(url) with:
 success |response|:
  render(response)

 error |problem|:
  report(problem)
```

Правила:

1. `with:` остаётся на строке completed call segment, если длина строки допускает.
2. Entries имеют один уровень indentation относительно `with:`.
3. Entry body имеет ещё один уровень indentation.
4. Между entries formatter может сохранять одну пустую строку для читаемости.
5. Formatter не переписывает `with:` в explicit keyword closures и обратно.
6. Formatter не удаляет `&` из callable reference keyword values.
7. Formatter не добавляет `&` к locals, которые уже содержат callable values.
8. Duplicate names не форматируются как valid program и должны диагностироваться до canonical output.

---

## 15. Diagnostics

| Код / Error | Фаза | Ситуация |
|---|---|---|
| `AMB_MULTIBLOCK_TARGET` | parser/binder | `with:` не следует за completed call segment. |
| `AMB_MULTIBLOCK_EMPTY` | parser | `with:` не содержит entries. |
| `AMB_MULTIBLOCK_ENTRY` | parser | Suite содержит форму, не являющуюся named block entry. |
| `AMB_MULTIBLOCK_DUPLICATE` | binder | Имя entry повторяется внутри одного `with:`. |
| `AMB_MULTIBLOCK_KEYWORD_DUPLICATE` | binder/runtime | Entry конфликтует с explicit keyword или keyword spread. |
| `AMB_MULTIBLOCK_ANON_BLOCK_CONFLICT` | parser | На одном call site присутствуют `with:` и anonymous block suffix/block-pass. |
| `AMB_NAMED_CALLABLE_PARAM_FORM` | parser | `&name:` использует pattern, auto-assign или другую запрещённую форму. |
| `AMB_NAMED_CALLABLE_MISSING` | binder/runtime | Required named callable parameter не передан. |
| `AMB_NAMED_CALLABLE_TYPE` | binder/runtime | Переданное значение не callable. |
| `AMB_CALLABLE_REFERENCE_TARGET` | binder | `&target` не является допустимым callable reference target. |
| `KeywordArgumentError` | runtime | Duplicate/missing/unknown keyword после normalization. |
| `TypeError` | runtime | Значение named callable argument не callable. |

### 15.1. Missing callback

```amber
request(url, success: &handle_success)
```

```text
KeywordArgumentError: missing named callable argument `error`
```

### 15.2. Non-callable value

```amber
request(url, success: 42, error: &handle_error)
```

```text
TypeError: named callable argument `success` must be callable; got Int
```

### 15.3. Duplicate explicit/multiblock

```amber
request(url, success: &handle_success) with:
 success:
  use(_1)
```

```text
KeywordArgumentError: duplicate keyword argument `success`
```

### 15.4. Ошибочный trailing block-pass

```amber
request(url, &success_handler)
```

при сигнатуре:

```amber
def request(url, &success:, &error:):
 ...
```

не заполняет `success`. Рекомендуемая diagnostic hint:

```text
missing named callable arguments `success`, `error`;
trailing `&success_handler` supplies the anonymous block channel.
Use `success: &success_handler`.
```

---

## 16. Совместимость

### 16.1. Source compatibility

- `&name:` ранее не являлся valid parameter form, поэтому новая grammar не меняет смысл valid old programs.
- `with:` является contextual suffix и не резервирует `with` глобально.
- Existing `&name` anonymous block parameter остаётся без изменений.
- Existing trailing `&name` block-pass остаётся без изменений.
- Existing callable reference syntax `&target` остаётся без изменений.
- Ordinary keyword parameters продолжают принимать callable values без marker `&`.

### 16.2. Runtime compatibility

Новый ABI не требуется. Минимальная implementation может lowering'ить всё расширение в:

- ordinary closures;
- ordinary keyword argument merge;
- ordinary parameter checks;
- ordinary `HCall`.

### 16.3. Library compatibility

Существующий API:

```amber
def request(url, success:, error:):
 ...
```

может сразу вызываться через `with:` даже до миграции signature marker, если implementation разрешает multiblock lowering для ordinary keyword parameters:

```amber
request(url) with:
 success:
  use(_1)
 error:
  report(_1)
```

Разница состоит в diagnostics:

- `success:` принимает любое value;
- `&success:` требует callable value.

Рекомендуемая migration для callback-oriented public API:

```amber
def request(url, &success:, &error:):
 ...
```

---

## 17. Взаимодействие с `Result` и exceptions

Named multiblock не заменяет `Result`.

Для синхронной exactly-one-result операции предпочтительным может оставаться:

```amber
case! request(url):
 when Ok(response):
  render(response)
 when Err(problem):
  report(problem)
```

Multiblock особенно уместен для:

- streams;
- lifecycle APIs;
- visitor APIs;
- transactions;
- builders;
- event-oriented calls;
- APIs, где callback может быть сохранён;
- нескольких возможных callback invocations.

Выбор является library design decision, а не языковым правилом.

---

## 18. Отклонённые альтернативы

### 18.1. Mutable callback provider

```amber
operation() |on|:
 on.success:
  ...
 on.error:
  ...
```

Отклонено как базовая языковая модель, потому что требует provider object, mutation и signature-indirect registration.

Такой DSL по-прежнему может быть реализован библиотекой.

### 18.2. Несколько нативных block channels

Модель:

```text
CALL positional kwargs success_block error_block
```

отклонена, потому что усложняет:

- VM ABI;
- reflective send;
- method_missing;
- forwarding;
- arity metadata;
- bytecode verifier;
- FFI boundaries;
- tooling.

Keyword callable lowering даёт ту же выразительность без нового ABI.

### 18.3. Неявные nested blocks без `with:`

```amber
request(url):
 success:
  ...
 error:
  ...
```

Отклонено из-за неоднозначности с ordinary anonymous block body, внутри которого вызываются методы `success` и `error`.

Parser не должен выбирать AST по runtime signature.

### 18.4. Keyword `on:`

```amber
request(url) on:
 success:
  ...
```

Не принято как canonical form, потому что `on` слишком event-specific и хуже подходит для `body`, `commit`, `rollback`, `visit`, `missing` и builder-style contracts.

### 18.5. `callbacks:` map

```amber
request(url, callbacks: {
 success: &success_handler,
 error: &error_handler
})
```

Остаётся library-level option, но не даёт signature-level keyword checking и inline block ergonomics.

### 18.6. Требовать `&` перед inline entry name

```amber
request(url) with:
 &success |response|:
  ...
```

Отклонено.

Inline entry уже является closure declaration, поэтому дополнительный `&` не создаёт reference и визуально смешивает declaration с `&target` extraction.

`&` остаётся обязательным там, где действительно извлекается reference из function/method target:

```amber
success: &success_handler
```

---

## 19. Conformance anchors

Минимальный conformance corpus должен включать:

### 19.1. Parse/run success

- два required named callable parameters;
- required + optional nullable callback;
- callable reference keyword values;
- namespace/class method references;
- locals containing prebuilt callable values;
- multiblock entries с explicit parameters;
- multiblock entries с `_1`, `_2`;
- block pattern parameters;
- arbitrary entry names;
- multiblock call through reflective `send`;
- keyword spread без конфликтов;
- callback, вызываемый несколько раз;
- callback, не вызываемый ни разу.

### 19.2. Parse/bind failures

- empty `with:`;
- duplicate entry;
- `with:` без call target;
- malformed entry;
- `&@name:`;
- pattern после `&` в signature;
- multiblock + anonymous block suffix;
- multiblock + trailing block-pass;
- explicit keyword + same multiblock entry;
- unsupported callable reference target.

### 19.3. Runtime failures

- missing required named callable;
- non-callable explicit value;
- non-callable default;
- duplicate from keyword spread;
- wrong unbound instance method receiver/arity;
- callback body pattern mismatch.

### 19.4. AST/HIR goldens

Goldens должны доказывать:

- `success: &handler` создаёт `AstCallableReference`, не block-pass;
- `&success:` создаёт `AstNamedCallableParameter`;
- `with:` создаёт `AstMultiblockSuffix`;
- HIR lowering использует keyword closures;
- anonymous block slot остаётся `null` для pure multiblock call;
- source spans entries сохраняются для diagnostics/stack traces.

---

## 20. Рекомендуемые точки интеграции в unified specification

После принятия решение следует интегрировать в следующие разделы Amber spec:

1. **Design anchors** — упомянуть named multiblock как sugar над callable keyword arguments.
2. **Postfix expressions and block suffix** — добавить `with:` как отдельный call suffix.
3. **Callable references `&...`** — добавить нормативный пример `success: &success_handler` и отличие от trailing block-pass.
4. **Blocks/lambda arguments/placeholders** — распространить block parameter rules на named block entries.
5. **Parameters and signatures** — добавить `&name:` и `&name: default`.
6. **Argument evaluation order** — включить создание multiblock closures после explicit/spread argument evaluation.
7. **Reflective send/method_missing** — зафиксировать lowering в ordinary keywords.
8. **AST/HIR** — добавить syntax-faithful nodes и lowering.
9. **Diagnostics** — добавить `AMB_MULTIBLOCK_*` и `AMB_NAMED_CALLABLE_*`.
10. **Formatter/conformance corpus** — добавить canonical output и golden cases.

---

## 21. Итоговое нормативное ядро

Amber v20.9 принимает следующую основную форму объявления:

```amber
def request(url, &success:, &error:):
 response = Http.get(url)

 if response.ok?:
  success(response)
 else:
  error(response.error)
```

Передача существующих callable targets:

```amber
request(
 url,
 success: &success_handler,
 error: &error_handler
)
```

Передача уже материализованных callable values:

```amber
success_callback = &success_handler
error_callback = &error_handler

request(
 url,
 success: success_callback,
 error: error_callback
)
```

Inline multiblock:

```amber
request(url) with:
 success |response|:
  render(response)

 error |problem|:
  report(problem)
```

Нормативная семантическая формула:

```text
named multiblock entry
  = closure value
  = named keyword callable argument
  != additional native block channel
```

И ключевое различие `&`:

```text
success: &handler
  => callable reference expression в keyword value

..., &handler
  => existing trailing anonymous block-pass
```

---

## 22. Draft acceptance decision

Проект предлагает принять для Amber v20.9 следующие решения:

1. Ввести contextual call suffix `with:`.
2. Разрешить одну или несколько named block entries внутри `with:`.
3. Lowering'ить каждую entry в ordinary closure keyword argument.
4. Ввести signature marker `&name:` для required named callable keyword parameter.
5. Ввести `&name: default` для optional/defaulted named callable keyword parameter.
6. Разрешить несколько `&name:` parameters в одной signature.
7. Сохранить bare `&name` как единственный existing anonymous block parameter.
8. Сохранить trailing `&name` в call arguments как existing anonymous block-pass.
9. Считать `success: &success_handler` callable reference expression, а не block-pass.
10. Не требовать `&` для locals, уже содержащих callable values.
11. Не вводить несколько native block channels или новый call ABI.
12. Запретить сочетание `with:` и anonymous block suffix/block-pass на одном call site в v20.9.
13. Использовать existing keyword duplicate/missing machinery и `TypeError` для callable validation.
14. Сохранить `with` contextual, а не globally reserved.
15. Не назначать специальной semantics именам `success`, `error` и другим entry names.
