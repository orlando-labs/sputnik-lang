" Run: vim -Nu NONE -i NONE -n -es -S editors/vim/test/syntax.vim
set nocompatible
set encoding=utf-8
set viminfo=
set ignorecase
let s:runtime = fnamemodify(expand('<sfile>:p'), ':h:h')
execute 'set runtimepath^=' . fnameescape(s:runtime)
filetype plugin on
syntax enable

function! s:Source(lines) abort
  %delete _
  call setline(1, a:lines)
  syntax sync fromstart
endfunction

function! s:Group(lnum, text, expected, ...) abort
  let l:occurrence = a:0 ? a:1 : 0
  let l:index = -1
  for l:count in range(l:occurrence + 1)
    let l:index = stridx(getline(a:lnum), a:text, l:index + 1)
    if l:index < 0
      call assert_report('Missing token ' . string(a:text) . ' on line ' . a:lnum)
      return
    endif
  endfor
  for l:column in range(l:index + 1, l:index + strlen(a:text))
    call assert_equal(a:expected, synIDattr(synID(a:lnum, l:column, 1), 'name'),
          \ printf('line %d, column %d, token %s', a:lnum, l:column, string(a:text)))
    if synIDattr(synID(a:lnum, l:column, 1), 'name') !=# a:expected
      return
    endif
  endfor
endfunction

let s:fixture = tempname() . '.am'
try
  " Exercise BufNewFile, then BufRead, using the installed runtime path.
  execute 'edit ' . fnameescape(s:fixture)
  call assert_equal('amber', &l:filetype)
  call assert_equal('amber', &l:syntax)
  call assert_equal('amber', b:current_syntax)
  call assert_equal('# %s', &l:commentstring)
  call assert_equal('b:#', &l:comments)
  call writefile(['if true:'], s:fixture)
  bwipeout!
  execute 'edit ' . fnameescape(s:fixture)
  call assert_equal('amber', &l:filetype)
  call s:Group(1, 'if', 'amberConditional')

  call s:Source([
        \ '@bytes += if Bytes === data then data.count else data.to_str.bytesize',
        \ 'then = 1',
        \ 'if_ready? = 2',
        \ 'if? = false',
        \ 'IF = true',
        \ 'ready and enabled or fallback',
        \ 'not empty? and item in items',
        \ 'if true:',
        \ ' raise "nope"',
        \ 'else:',
        \ ' return null',
        \ 'value = catch(:done):',
        \ ' throw :done, 42',
        \ 'case! value:',
        \ ' while ready:',
        \ '  break',
        \ ])
  call s:Group(1, '@bytes', 'amberInstanceVariable')
  call s:Group(1, '+=', 'amberOperator')
  call s:Group(1, 'if', 'amberConditional')
  call s:Group(1, 'then', 'amberThen')
  call s:Group(1, 'else', 'amberConditional')
  call s:Group(1, 'Bytes', 'amberType')
  call s:Group(1, '===', 'amberOperator')
  call s:Group(1, 'count', 'amberMember')
  call s:Group(2, 'then', 'amberIdentifier')
  call s:Group(3, 'if_ready?', 'amberIdentifier')
  call s:Group(4, 'if?', 'amberIdentifier')
  call s:Group(5, 'IF', 'amberType')
  call s:Group(6, 'and', 'amberWordOperator')
  call s:Group(6, 'or', 'amberWordOperator')
  call s:Group(7, 'not', 'amberWordOperator')
  call s:Group(7, 'in', 'amberWordOperator')
  call s:Group(8, 'true', 'amberBoolean')
  call s:Group(9, 'raise', 'amberException')
  call s:Group(10, 'else', 'amberConditional')
  call s:Group(11, 'return', 'amberControl')
  call s:Group(11, 'null', 'amberConstant')
  call s:Group(12, 'catch', 'amberException')
  call s:Group(13, 'throw', 'amberException')
  call s:Group(14, 'case!', 'amberConditional')
  call s:Group(15, 'while', 'amberRepeat')
  call s:Group(16, 'break', 'amberControl')

  call s:Source([
        \ 'get "/v1/items/:id", to: &Items#index # route handler',
        \ 'ready = &liveness',
        \ 'callable = &Users.find',
        \ 'callable = &app.Users#ready?',
        \ 'foo#bar',
        \ 'foo # ordinary comment TODO',
        \ '# whole-line comment',
        \ 'value = 3#not_comment',
        \ ])
  call s:Group(1, 'get', 'amberFunctionCall')
  call s:Group(1, '&', 'amberReferenceDelimiter')
  call s:Group(1, 'Items', 'amberReferenceReceiver')
  call s:Group(1, '#', 'amberReferenceDelimiter')
  call s:Group(1, 'index', 'amberReferenceMethod')
  call s:Group(1, 'route handler', 'amberComment')
  call s:Group(2, 'liveness', 'amberReferenceMethod')
  call s:Group(3, 'Users', 'amberReferenceReceiver')
  call s:Group(3, 'find', 'amberReferenceMethod')
  call s:Group(4, 'app', 'amberReferenceReceiver')
  call s:Group(4, 'ready?', 'amberReferenceMethod')
  call s:Group(5, '#', '')
  call s:Group(6, 'ordinary comment', 'amberComment')
  call s:Group(6, 'TODO', 'amberTodo')
  call s:Group(7, '# whole-line comment', 'amberComment')
  call s:Group(8, '#', '')

  call s:Source([
        \ 'values = [42, 1_000, 1.5, 2e3, 1_2.3_4e-5_6, 0xFF, 0b1010, 0o755]',
        \ 'state = :ready',
        \ 'call(mode::fast)',
        \ 'last = $_',
        \ 'items.map: $it + $it1 + $it2 + _1 + _2',
        \ 'when _: null',
        \ '$it!=0',
        \ '$it1!~pattern',
        \ '$it2[?0]',
        \ 'α42 + value2 + 0xFG',
        \ ])
  for s:token in ['42', '1_000', '0xFF', '0b1010', '0o755']
    call s:Group(1, s:token, 'amberNumber')
  endfor
  for s:token in ['1.5', '2e3', '1_2.3_4e-5_6']
    call s:Group(1, s:token, 'amberFloat')
  endfor
  call s:Group(2, ':ready', 'amberSymbol')
  call s:Group(3, ':fast', 'amberSymbol')
  call s:Group(3, 'call', 'amberFunctionCall')
  call s:Group(4, '$_', 'amberLastValue')
  for s:token in ['$it', '$it1', '$it2', '_1', '_2']
    call s:Group(5, s:token, 'amberPlaceholder')
  endfor
  call s:Group(6, '_', 'amberWildcard')
  call s:Group(7, '$it', 'amberPlaceholder')
  call s:Group(7, '!=', 'amberOperator')
  call s:Group(8, '$it1', 'amberPlaceholder')
  call s:Group(8, '!~', 'amberOperator')
  call s:Group(9, '$it2', 'amberPlaceholder')
  call s:Group(9, '?', 'amberOperator')
  call s:Group(10, 'α42', 'amberIdentifier')
  call s:Group(10, 'value2', 'amberIdentifier')
  for s:token in ['$it0', '$it01', '$item', '$it_1', '$it2x', '$it?', '$it!', '$it1?', '$it?[0]', '$itя', '$it1é']
    call s:Source([s:token])
    for s:column in range(1, strlen(s:token))
      call assert_notequal('amberPlaceholder', synIDattr(synID(1, s:column, 1), 'name'), s:token)
    endfor
  endfor

  call s:Source([
        \ 'a ** b // c % d <=> e',
        \ 'a =~ pattern or a !~ pattern',
        \ 'a << 2 >> 1',
        \ 'first..last',
        \ 'first...last',
        \ 'value.?.member()',
        \ 'items?[0]',
        \ 'def <=>(other): 0',
        \ 'def []=(key, value): value',
        \ 'value ?? fallback -> result !{io}',
        \ 'xs[?i] + user[?:email] + items[ ?0]',
        \ 'Bytes?[0] + Данные?[0]',
        \ '([{}]),:;',
        \ ])
  for s:token in ['**', '//', '%', '<=>']
    call s:Group(1, s:token, 'amberOperator')
  endfor
  call s:Group(2, '=~', 'amberOperator')
  call s:Group(2, '!~', 'amberOperator')
  call s:Group(3, '<<', 'amberOperator')
  call s:Group(3, '>>', 'amberOperator')
  call s:Group(4, '..', 'amberOperator')
  call s:Group(5, '...', 'amberOperator')
  call s:Group(6, '.?.', 'amberOperator')
  call s:Group(6, 'member', 'amberFunctionCall')
  call s:Group(7, '?', 'amberOperator')
  call s:Group(8, '<=>', 'amberFunction')
  call s:Group(9, '[]=', 'amberFunction')
  for s:token in ['??', '->', '!']
    call s:Group(10, s:token, 'amberOperator')
  endfor
  for s:occurrence in range(3)
    call s:Group(11, '?', 'amberOperator', s:occurrence)
  endfor
  call s:Group(12, 'Bytes', 'amberType')
  call s:Group(12, 'Данные', 'amberType')
  call s:Group(12, '?', 'amberOperator')
  call s:Group(12, '?', 'amberOperator', 1)
  call s:Group(13, '([{}]),:;', 'amberDelimiter')

  call s:Source([
        \ 'native def read(fd as Int) -> Bytes',
        \ 'native class Handle from "ext.Handle" owned:',
        \ 'prop name:',
        \ ' get: @name',
        \ ' set(value): @name = value',
        \ 'attr var email from @raw_email',
        \ 'attr set label',
        \ 'pass = noop',
        \ 'import app.models as models',
        \ 'next item',
        \ 'next = item',
        \ 'native = macro + string_tag + owned + var',
        \ 'numeric: strict',
        \ 'pattern(item) with {x: 1}',
        \ 'class Пользователь < Model:',
        \ ' def готов?(α): @масса + @@счётчик + self',
        \ 'class_prop count:',
        \ ])
  call s:Group(1, 'native', 'amberModifier')
  call s:Group(1, 'read', 'amberFunction')
  call s:Group(1, 'as', 'amberWordOperator')
  call s:Group(2, 'Handle', 'amberClassName')
  call s:Group(2, 'owned', 'amberOwnership')
  call s:Group(3, 'prop', 'amberDeclaration')
  call s:Group(3, 'name', 'amberPropertyName')
  call s:Group(4, 'get', 'amberPropertyModifier')
  call s:Group(5, 'set', 'amberPropertyModifier')
  call s:Group(6, 'var', 'amberAttrModifier')
  call s:Group(6, 'email', 'amberPropertyName')
  call s:Group(7, 'set', 'amberAttrModifier')
  call s:Group(7, 'label', 'amberPropertyName')
  call s:Group(8, 'pass', 'amberIdentifier')
  call s:Group(8, 'noop', 'amberIdentifier')
  call s:Group(9, 'as', 'amberWordOperator')
  call s:Group(10, 'next', 'amberControl')
  call s:Group(11, 'next', 'amberIdentifier')
  for s:token in ['native', 'macro', 'string_tag', 'owned', 'var']
    call s:Group(12, s:token, 'amberIdentifier')
  endfor
  call s:Group(13, 'numeric', 'amberDirective')
  call s:Group(14, 'pattern', 'amberConditional')
  call s:Group(14, 'with', 'amberConditional')
  call s:Group(15, 'Пользователь', 'amberClassName')
  call s:Group(16, 'готов?', 'amberFunction')
  call s:Group(16, 'α', 'amberIdentifier')
  call s:Group(16, '@масса', 'amberInstanceVariable')
  call s:Group(16, '@@счётчик', 'amberClassVariable')
  call s:Group(16, 'self', 'amberSelf')
  call s:Group(17, 'count', 'amberPropertyName')

  call s:Source([
        \ 'message = "hello #{if ok then {name: user.name} else null} tail"',
        \ 'nested = "#{"inner #{ {x: 1} }"} end"',
        \ 'literal = ''#{not_interpolated}''',
        \ 'escaped = "\#{literal} \"quote\" \n \u{1F600}"',
        \ 'x = r"\d+#{not_interpolated}#not-a-comment"',
        \ 'x = r''\d+#{not_interpolated}''',
        \ 'query = sql"""',
        \ ' SELECT * FROM users WHERE id = #{id}',
        \ ' """',
        \ 'raw = r"""',
        \ ' #{not_interpolated} \d+',
        \ ' """',
        \ 'block = """',
        \ ' #{ {key: "#{value}"} } trailing',
        \ ' """',
        \ 'command = cmd''hello #{name}''',
        \ 'after = 42 # done',
        \ ])
  call s:Group(1, 'hello', 'amberDoubleString')
  call s:Group(1, '#{', 'amberInterpolationDelimiter')
  call s:Group(1, 'then', 'amberThen')
  call s:Group(1, 'name', 'amberIdentifier')
  call s:Group(1, 'tail', 'amberDoubleString')
  call s:Group(2, 'inner', 'amberDoubleString')
  call s:Group(2, '1', 'amberNumber')
  call s:Group(2, 'end', 'amberDoubleString')
  call s:Group(3, '#{not_interpolated}', 'amberSingleString')
  call s:Group(4, '\#', 'amberDoubleEscape')
  call s:Group(4, 'literal', 'amberDoubleString')
  call s:Group(4, '\n', 'amberDoubleEscape')
  call s:Group(4, '\u{1F600}', 'amberDoubleEscape')
  call s:Group(5, 'r', 'amberRawTag')
  call s:Group(5, '\d', 'amberRawEscape')
  call s:Group(5, '#{not_interpolated}#not-a-comment', 'amberRawDoubleString')
  call s:Group(6, '#{not_interpolated}', 'amberRawSingleString')
  call s:Group(7, 'sql', 'amberStringTag')
  call s:Group(8, 'SELECT', 'amberTextBlock')
  call s:Group(8, 'id', 'amberIdentifier', 1)
  call s:Group(10, 'r', 'amberRawTag', 1)
  call s:Group(11, '#{not_interpolated}', 'amberRawTextBlock')
  call s:Group(14, 'value', 'amberIdentifier')
  call s:Group(14, 'trailing', 'amberTextBlock')
  call s:Group(16, 'cmd', 'amberStringTag')
  call s:Group(16, 'hello', 'amberTaggedSingleString')
  call s:Group(16, 'name', 'amberIdentifier')
  call s:Group(17, '42', 'amberNumber')
  call s:Group(17, 'done', 'amberComment')

  call s:Source([
        \ '#!/usr/bin/env amberc',
        \ 'string_tag macro def assert(check):',
        \ ' if not #{check}:',
        \ '  raise "failed #{check}"',
        \ '# outside macro comment',
        \ 'plain = 1',
        \ ' # ordinary #{comment}',
        \ 'macro def outer(x):',
        \ ' quote:',
        \ '  #{ {key: x} }',
        \ 'plain = 2',
        \ '#{outside_macro}',
        \ ])
  call s:Group(1, '#!/usr/bin/env amberc', 'amberShebang')
  call s:Group(2, 'string_tag', 'amberModifier')
  call s:Group(2, 'macro', 'amberModifier')
  call s:Group(2, 'assert', 'amberFunction')
  call s:Group(3, '#{', 'amberInterpolationDelimiter')
  call s:Group(3, 'check', 'amberIdentifier')
  call s:Group(4, 'failed', 'amberDoubleString')
  call s:Group(5, 'outside macro comment', 'amberComment')
  call s:Group(6, '1', 'amberNumber')
  call s:Group(7, 'ordinary #{comment}', 'amberComment')
  call s:Group(9, 'quote', 'amberControl')
  call s:Group(10, '#{', 'amberInterpolationDelimiter')
  call s:Group(10, 'x', 'amberIdentifier')
  call s:Group(12, '#{outside_macro}', 'amberComment')

  call assert_equal(synIDtrans(hlID('Function')), synIDtrans(hlID('amberFunction')))
  call assert_equal(synIDtrans(hlID('String')), synIDtrans(hlID('amberTextBlock')))
  call assert_equal(synIDtrans(hlID('Comment')), synIDtrans(hlID('amberComment')))

  " Sourcing twice is harmless and does not change cpoptions.
  let s:cpo = &cpo
  runtime syntax/amber.vim
  call assert_equal(s:cpo, &cpo)
  call assert_equal('amber', b:current_syntax)
  setlocal filetype=
  call assert_equal(&g:commentstring, &l:commentstring)
  call assert_equal(&g:comments, &l:comments)
  bwipeout!
  execute 'edit ' . fnameescape(fnamemodify(s:fixture, ':h') . '/Makefile.am')
  call assert_equal('automake', &l:filetype)
  call assert_equal('', v:errmsg)
catch
  call assert_report(v:exception . ' at ' . v:throwpoint)
finally
  call delete(s:fixture)
endtry

if !empty(v:errors)
  set verbose=1
  for s:error in v:errors
    echom s:error
  endfor
  cquit
endif
set verbose=1
echom 'Amber Vim syntax tests: ok'
qa!
