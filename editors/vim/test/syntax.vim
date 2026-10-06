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

let s:fixture = tempname() . '.s'
try
  " Exercise BufNewFile, then BufRead, using the installed runtime path.
  execute 'edit ' . fnameescape(s:fixture)
  call assert_equal('sputnik', &l:filetype)
  call assert_equal('sputnik', &l:syntax)
  call assert_equal('sputnik', b:current_syntax)
  call assert_equal('# %s', &l:commentstring)
  call assert_equal('b:#', &l:comments)
  call writefile(['if true:'], s:fixture)
  bwipeout!
  execute 'edit ' . fnameescape(s:fixture)
  call assert_equal('sputnik', &l:filetype)
  call s:Group(1, 'if', 'sputnikConditional')

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
  call s:Group(1, '@bytes', 'sputnikInstanceVariable')
  call s:Group(1, '+=', 'sputnikOperator')
  call s:Group(1, 'if', 'sputnikConditional')
  call s:Group(1, 'then', 'sputnikThen')
  call s:Group(1, 'else', 'sputnikConditional')
  call s:Group(1, 'Bytes', 'sputnikType')
  call s:Group(1, '===', 'sputnikOperator')
  call s:Group(1, 'count', 'sputnikMember')
  call s:Group(2, 'then', 'sputnikIdentifier')
  call s:Group(3, 'if_ready?', 'sputnikIdentifier')
  call s:Group(4, 'if?', 'sputnikIdentifier')
  call s:Group(5, 'IF', 'sputnikType')
  call s:Group(6, 'and', 'sputnikWordOperator')
  call s:Group(6, 'or', 'sputnikWordOperator')
  call s:Group(7, 'not', 'sputnikWordOperator')
  call s:Group(7, 'in', 'sputnikWordOperator')
  call s:Group(8, 'true', 'sputnikBoolean')
  call s:Group(9, 'raise', 'sputnikException')
  call s:Group(10, 'else', 'sputnikConditional')
  call s:Group(11, 'return', 'sputnikControl')
  call s:Group(11, 'null', 'sputnikConstant')
  call s:Group(12, 'catch', 'sputnikException')
  call s:Group(13, 'throw', 'sputnikException')
  call s:Group(14, 'case!', 'sputnikConditional')
  call s:Group(15, 'while', 'sputnikRepeat')
  call s:Group(16, 'break', 'sputnikControl')

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
  call s:Group(1, 'get', 'sputnikFunctionCall')
  call s:Group(1, '&', 'sputnikReferenceDelimiter')
  call s:Group(1, 'Items', 'sputnikReferenceReceiver')
  call s:Group(1, '#', 'sputnikReferenceDelimiter')
  call s:Group(1, 'index', 'sputnikReferenceMethod')
  call s:Group(1, 'route handler', 'sputnikComment')
  call s:Group(2, 'liveness', 'sputnikReferenceMethod')
  call s:Group(3, 'Users', 'sputnikReferenceReceiver')
  call s:Group(3, 'find', 'sputnikReferenceMethod')
  call s:Group(4, 'app', 'sputnikReferenceReceiver')
  call s:Group(4, 'ready?', 'sputnikReferenceMethod')
  call s:Group(5, '#', '')
  call s:Group(6, 'ordinary comment', 'sputnikComment')
  call s:Group(6, 'TODO', 'sputnikTodo')
  call s:Group(7, '# whole-line comment', 'sputnikComment')
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
    call s:Group(1, s:token, 'sputnikNumber')
  endfor
  for s:token in ['1.5', '2e3', '1_2.3_4e-5_6']
    call s:Group(1, s:token, 'sputnikFloat')
  endfor
  call s:Group(2, ':ready', 'sputnikSymbol')
  call s:Group(3, ':fast', 'sputnikSymbol')
  call s:Group(3, 'call', 'sputnikFunctionCall')
  call s:Group(4, '$_', 'sputnikLastValue')
  for s:token in ['$it', '$it1', '$it2', '_1', '_2']
    call s:Group(5, s:token, 'sputnikPlaceholder')
  endfor
  call s:Group(6, '_', 'sputnikWildcard')
  call s:Group(7, '$it', 'sputnikPlaceholder')
  call s:Group(7, '!=', 'sputnikOperator')
  call s:Group(8, '$it1', 'sputnikPlaceholder')
  call s:Group(8, '!~', 'sputnikOperator')
  call s:Group(9, '$it2', 'sputnikPlaceholder')
  call s:Group(9, '?', 'sputnikOperator')
  call s:Group(10, 'α42', 'sputnikIdentifier')
  call s:Group(10, 'value2', 'sputnikIdentifier')
  for s:token in ['$it0', '$it01', '$item', '$it_1', '$it2x', '$it?', '$it!', '$it1?', '$it?[0]', '$itя', '$it1é']
    call s:Source([s:token])
    for s:column in range(1, strlen(s:token))
      call assert_notequal('sputnikPlaceholder', synIDattr(synID(1, s:column, 1), 'name'), s:token)
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
    call s:Group(1, s:token, 'sputnikOperator')
  endfor
  call s:Group(2, '=~', 'sputnikOperator')
  call s:Group(2, '!~', 'sputnikOperator')
  call s:Group(3, '<<', 'sputnikOperator')
  call s:Group(3, '>>', 'sputnikOperator')
  call s:Group(4, '..', 'sputnikOperator')
  call s:Group(5, '...', 'sputnikOperator')
  call s:Group(6, '.?.', 'sputnikOperator')
  call s:Group(6, 'member', 'sputnikFunctionCall')
  call s:Group(7, '?', 'sputnikOperator')
  call s:Group(8, '<=>', 'sputnikFunction')
  call s:Group(9, '[]=', 'sputnikFunction')
  for s:token in ['??', '->', '!']
    call s:Group(10, s:token, 'sputnikOperator')
  endfor
  for s:occurrence in range(3)
    call s:Group(11, '?', 'sputnikOperator', s:occurrence)
  endfor
  call s:Group(12, 'Bytes', 'sputnikType')
  call s:Group(12, 'Данные', 'sputnikType')
  call s:Group(12, '?', 'sputnikOperator')
  call s:Group(12, '?', 'sputnikOperator', 1)
  call s:Group(13, '([{}]),:;', 'sputnikDelimiter')

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
  call s:Group(1, 'native', 'sputnikModifier')
  call s:Group(1, 'read', 'sputnikFunction')
  call s:Group(1, 'as', 'sputnikWordOperator')
  call s:Group(2, 'Handle', 'sputnikClassName')
  call s:Group(2, 'owned', 'sputnikOwnership')
  call s:Group(3, 'prop', 'sputnikDeclaration')
  call s:Group(3, 'name', 'sputnikPropertyName')
  call s:Group(4, 'get', 'sputnikPropertyModifier')
  call s:Group(5, 'set', 'sputnikPropertyModifier')
  call s:Group(6, 'var', 'sputnikAttrModifier')
  call s:Group(6, 'email', 'sputnikPropertyName')
  call s:Group(7, 'set', 'sputnikAttrModifier')
  call s:Group(7, 'label', 'sputnikPropertyName')
  call s:Group(8, 'pass', 'sputnikIdentifier')
  call s:Group(8, 'noop', 'sputnikIdentifier')
  call s:Group(9, 'as', 'sputnikWordOperator')
  call s:Group(10, 'next', 'sputnikControl')
  call s:Group(11, 'next', 'sputnikIdentifier')
  for s:token in ['native', 'macro', 'string_tag', 'owned', 'var']
    call s:Group(12, s:token, 'sputnikIdentifier')
  endfor
  call s:Group(13, 'numeric', 'sputnikDirective')
  call s:Group(14, 'pattern', 'sputnikConditional')
  call s:Group(14, 'with', 'sputnikConditional')
  call s:Group(15, 'Пользователь', 'sputnikClassName')
  call s:Group(16, 'готов?', 'sputnikFunction')
  call s:Group(16, 'α', 'sputnikIdentifier')
  call s:Group(16, '@масса', 'sputnikInstanceVariable')
  call s:Group(16, '@@счётчик', 'sputnikClassVariable')
  call s:Group(16, 'self', 'sputnikSelf')
  call s:Group(17, 'count', 'sputnikPropertyName')

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
  call s:Group(1, 'hello', 'sputnikDoubleString')
  call s:Group(1, '#{', 'sputnikInterpolationDelimiter')
  call s:Group(1, 'then', 'sputnikThen')
  call s:Group(1, 'name', 'sputnikIdentifier')
  call s:Group(1, 'tail', 'sputnikDoubleString')
  call s:Group(2, 'inner', 'sputnikDoubleString')
  call s:Group(2, '1', 'sputnikNumber')
  call s:Group(2, 'end', 'sputnikDoubleString')
  call s:Group(3, '#{not_interpolated}', 'sputnikSingleString')
  call s:Group(4, '\#', 'sputnikDoubleEscape')
  call s:Group(4, 'literal', 'sputnikDoubleString')
  call s:Group(4, '\n', 'sputnikDoubleEscape')
  call s:Group(4, '\u{1F600}', 'sputnikDoubleEscape')
  call s:Group(5, 'r', 'sputnikRawTag')
  call s:Group(5, '\d', 'sputnikRawEscape')
  call s:Group(5, '#{not_interpolated}#not-a-comment', 'sputnikRawDoubleString')
  call s:Group(6, '#{not_interpolated}', 'sputnikRawSingleString')
  call s:Group(7, 'sql', 'sputnikStringTag')
  call s:Group(8, 'SELECT', 'sputnikTextBlock')
  call s:Group(8, 'id', 'sputnikIdentifier', 1)
  call s:Group(10, 'r', 'sputnikRawTag', 1)
  call s:Group(11, '#{not_interpolated}', 'sputnikRawTextBlock')
  call s:Group(14, 'value', 'sputnikIdentifier')
  call s:Group(14, 'trailing', 'sputnikTextBlock')
  call s:Group(16, 'cmd', 'sputnikStringTag')
  call s:Group(16, 'hello', 'sputnikTaggedSingleString')
  call s:Group(16, 'name', 'sputnikIdentifier')
  call s:Group(17, '42', 'sputnikNumber')
  call s:Group(17, 'done', 'sputnikComment')

  call s:Source([
        \ '#!/usr/bin/env sputnik',
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
  call s:Group(1, '#!/usr/bin/env sputnik', 'sputnikShebang')
  call s:Group(2, 'string_tag', 'sputnikModifier')
  call s:Group(2, 'macro', 'sputnikModifier')
  call s:Group(2, 'assert', 'sputnikFunction')
  call s:Group(3, '#{', 'sputnikInterpolationDelimiter')
  call s:Group(3, 'check', 'sputnikIdentifier')
  call s:Group(4, 'failed', 'sputnikDoubleString')
  call s:Group(5, 'outside macro comment', 'sputnikComment')
  call s:Group(6, '1', 'sputnikNumber')
  call s:Group(7, 'ordinary #{comment}', 'sputnikComment')
  call s:Group(9, 'quote', 'sputnikControl')
  call s:Group(10, '#{', 'sputnikInterpolationDelimiter')
  call s:Group(10, 'x', 'sputnikIdentifier')
  call s:Group(12, '#{outside_macro}', 'sputnikComment')

  call assert_equal(synIDtrans(hlID('Function')), synIDtrans(hlID('sputnikFunction')))
  call assert_equal(synIDtrans(hlID('String')), synIDtrans(hlID('sputnikTextBlock')))
  call assert_equal(synIDtrans(hlID('Comment')), synIDtrans(hlID('sputnikComment')))

  " Sourcing twice is harmless and does not change cpoptions.
  let s:cpo = &cpo
  runtime syntax/sputnik.vim
  call assert_equal(s:cpo, &cpo)
  call assert_equal('sputnik', b:current_syntax)
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
echom 'Sputnik Vim syntax tests: ok'
qa!
