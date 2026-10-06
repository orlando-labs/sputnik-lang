# Sputnik v20.3.1 Draft Patch: Attribute Property Sugar

**Status:** proposed normative extension for the main Sputnik language specification
**Target base:** Sputnik v20.3 computed property descriptors patch
**Patch scope:** property declarations, class member declarations, parser grammar, AST/HIR lowering, diagnostics and conformance tests
**Non-goals:** computed properties, validation logic, module properties, local properties, automatic visibility modifiers, implicit public fields

---

# 0. Integration note

This patch introduces **attribute property sugar**.

The purpose of the feature is to provide a concise declaration form for exposing instance storage through standard property descriptors without requiring explicit getter/setter bodies.

The extension preserves all v20.3 property semantics:

* `prop` remains the canonical property descriptor mechanism;
* `attr` is purely syntactic sugar;
* `attr` always lowers into a property descriptor;
* `attr` does not introduce public fields;
* `@field` remains internal instance storage;
* external member names remain property names;
* no implicit public accessor generation is introduced.

---

# 1. Motivation

Current v20.3 code frequently requires boilerplate:

```sputnik
class User:
  def init(@email):
    noop

  prop email:
    get:
      @email
```

or

```sputnik
class Box:
  def init(@value):
    noop

  prop value:
    get:
      @value

    set(v):
      @value = v
```

The overwhelming majority of such declarations merely forward access to an instance field.

This patch introduces a concise declaration form:

```sputnik
class User:
  attr email
```

and

```sputnik
class Box:
  attr var value
```

while preserving the full property model.

---

# 2. Design principles

## 2.1. No automatic public fields

The following declaration:

```sputnik
def init(@x):
  noop
```

does not create a public member named `x`.

Therefore:

```sputnik
class A:
  def init(@x):
    noop

A(42).x
```

remains invalid unless a property or attribute declaration exists.

Instance storage and public API remain separate concepts.

---

## 2.2. `attr` lowers to `prop`

Normative rule:

> Every `attr` declaration lowers into an equivalent property descriptor declaration.

There is no runtime distinction between:

```sputnik
attr email
```

and:

```sputnik
prop email:
  get:
    @email
```

---

## 2.3. `attr` is not a field declaration

The following declarations occupy the same external member namespace:

```sputnik
attr email
```

```sputnik
prop email:
  ...
```

Therefore they conflict.

---

# 3. Surface syntax

## 3.1. Getter-only attribute

Canonical form:

```sputnik
attr name
```

Meaning:

```sputnik
prop name:
  get:
    @name
```

Example:

```sputnik
class User:
  def init(@email):
    noop

  attr email
```

---

## 3.2. Read-write attribute

Canonical form:

```sputnik
attr var name
```

Meaning:

```sputnik
prop name:
  get:
    @name

  set(value):
    @name = value
```

Example:

```sputnik
class Box:
  def init(@value):
    noop

  attr var value
```

---

## 3.3. Setter-only attribute

Canonical form:

```sputnik
attr set name
```

Meaning:

```sputnik
prop name:
  set(value):
    @name = value
```

Example:

```sputnik
class Credentials:
  attr set password
```

---

# 4. Explicit backing storage

## 4.1. Motivation

Sometimes the public member name should differ from the internal storage name.

Example:

```sputnik
@raw_email
```

should be exposed as:

```sputnik
email
```

This patch introduces explicit storage binding.

---

## 4.2. Syntax

Getter-only:

```sputnik
attr email from @raw_email
```

Read-write:

```sputnik
attr var email from @raw_email
```

Setter-only:

```sputnik
attr set email from @raw_email
```

---

## 4.3. Lowering

### Getter-only

```sputnik
attr email from @raw_email
```

lowers to:

```sputnik
prop email:
  get:
    @raw_email
```

### Read-write

```sputnik
attr var email from @raw_email
```

lowers to:

```sputnik
prop email:
  get:
    @raw_email

  set(value):
    @raw_email = value
```

### Setter-only

```sputnik
attr set email from @raw_email
```

lowers to:

```sputnik
prop email:
  set(value):
    @raw_email = value
```

---

# 5. Default storage resolution

If no explicit storage is specified:

```sputnik
attr email
```

the backing storage defaults to:

```sputnik
@email
```

Likewise:

```sputnik
attr var value
```

defaults to:

```sputnik
@value
```

and:

```sputnik
attr set password
```

defaults to:

```sputnik
@password
```

Normative rule:

> Missing `from @field` is equivalent to `from @<attribute_name>`.

---

# 6. Restrictions

## 6.1. Only instance fields are allowed

Valid:

```sputnik
attr email from @raw_email
```

Invalid:

```sputnik
attr email from @@raw_email
```

Invalid:

```sputnik
attr email from self.email
```

Invalid:

```sputnik
attr email from storage()
```

Invalid:

```sputnik
attr email from obj.field
```

The storage target must be a direct instance field token.

Reference grammar:

```ebnf
AttrStorage ::= "from" InstanceFieldName

InstanceFieldName ::= "@" Identifier
```

---

## 6.2. Computed behavior requires `prop`

The following is intentionally unsupported:

```sputnik
attr full_name from "#{@first} #{@last}"
```

Any computed behavior requires an ordinary property:

```sputnik
prop full_name:
  "#{@first} #{@last}"
```

---

# 7. Name conflicts

## 7.1. Conflict with property descriptors

Invalid:

```sputnik
class User:
  attr email

  prop email:
    @email
```

Required diagnostic:

```text
E_MEMBER_NAME_CONFLICT
external member `email` declared multiple times
```

---

## 7.2. Conflict with another attribute

Invalid:

```sputnik
class User:
  attr email
  attr var email
```

Required diagnostic:

```text
E_MEMBER_NAME_CONFLICT
external member `email` declared multiple times
```

---

## 7.3. Internal storage names do not conflict

Valid:

```sputnik
class User:
  attr email from @raw_email
```

The external member:

```sputnik
email
```

and the storage field:

```sputnik
@raw_email
```

belong to different namespaces.

---

# 8. Grammar additions

```ebnf
AttrDef ::= "attr" AttrMode? Identifier AttrStorage?

AttrMode ::= "var"
           | "set"

AttrStorage ::= "from" InstanceFieldName

InstanceFieldName ::= "@" Identifier
```

Interpretation:

```text
attr x
```

means getter-only.

```text
attr var x
```

means getter + setter.

```text
attr set x
```

means setter-only.

---

# 9. AST

Recommended AST node:

```text
AstAttrDef(
  name,
  mode,
  storage_field?,
  span
)
```

where:

```text
mode = GET_ONLY
     | GET_SET
     | SET_ONLY
```

Parser output must preserve the original `attr` declaration.

Parser must not immediately rewrite the node into `AstPropertyDef`.

---

# 10. HIR lowering

Before semantic lowering:

```sputnik
attr var email from @raw_email
```

After lowering:

```text
HPropertyDef(
  name = "email",
  getter = HFieldRead("@raw_email"),
  setter = HFieldWrite("@raw_email")
)
```

`attr` introduces no new runtime primitive.

All runtime behavior is inherited from property descriptors.

---

# 11. Diagnostics

## Missing field name

Invalid:

```sputnik
attr from @email
```

Diagnostic:

```text
E_ATTR_EXPECTED_NAME
attribute declaration requires a member name
```

---

## Missing storage target

Invalid:

```sputnik
attr email from
```

Diagnostic:

```text
E_ATTR_EXPECTED_STORAGE_FIELD
expected instance field after `from`
```

---

## Invalid storage target

Invalid:

```sputnik
attr email from foo()
```

Diagnostic:

```text
E_ATTR_INVALID_STORAGE
attribute storage must be an instance field token
```

---

# 12. Conformance tests

Valid:

```sputnik
class User:
  attr email
```

Valid:

```sputnik
class User:
  attr var email
```

Valid:

```sputnik
class User:
  attr set password
```

Valid:

```sputnik
class User:
  attr email from @raw_email
```

Valid:

```sputnik
class User:
  attr var email from @raw_email
```

Invalid:

```sputnik
class User:
  attr email
  prop email:
    @email
```

Invalid:

```sputnik
attr email from foo()
```

Invalid:

```sputnik
attr email from self.email
```

Invalid:

```sputnik
attr email from @@shared
```

---

# 13. Acceptance decision

This patch is accepted with the following final decisions:

1. `attr` is introduced as property sugar.
2. `attr name` defines a getter-only property.
3. `attr var name` defines a read-write property.
4. `attr set name` defines a setter-only property.
5. `from @field` specifies explicit backing storage.
6. Missing storage defaults to `@<name>`.
7. `attr` lowers into ordinary property descriptors.
8. `attr` does not create public fields.
9. `attr` and `prop` occupy the same external member namespace.
10. Duplicate external member declarations are errors.
11. Storage targets are restricted to direct instance field tokens.
12. Computed behavior remains the responsibility of `prop`.
