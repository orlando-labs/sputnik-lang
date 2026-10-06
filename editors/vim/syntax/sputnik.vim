" Vim syntax file
" Language: Sputnik
" Based on editors/vscode/syntaxes/sputnik.tmLanguage.json.
if exists('b:current_syntax')
  finish
endif

let s:save_cpo = &cpo
set cpo&vim
syntax case match

" Vim's \h and \w are ASCII-only; include multibyte identifiers as well.
let s:head = '\%(\h\|[^\x00-\x7f]\)'
let s:part = '\%(\w\|[^\x00-\x7f]\)'
" Leave optional indexing and effect-row punctuation outside identifiers.
let s:suffix = '\%(!\%(\s*{\)\@!\|?\%(\s*\[\)\@!\)\?'
let s:name = s:head . s:part . '*' . s:suffix
let s:before = s:part . '\@1<!'
let s:after = '\%(' . s:part . '\|[?!]\)\@!'
let s:digits = '[0-9]\%(_\?[0-9]\)*'

function! s:Match(group, pattern, ...) abort
  execute 'syntax match ' . a:group . ' /' . escape(a:pattern, '/') . '/ '
        \ . (a:0 ? a:1 : '')
endfunction

" Define general matches first; more specific matches below take priority.
call s:Match('sputnikIdentifier', s:before . s:name)
syntax match sputnikDelimiter /[.,:;()[\]{}]/
syntax match sputnikOperator /\/\/=\|+=\|-=\|\*=\|\/=\|%=\|===\|<=>\|<<\|>>\|\*\*\|\/\/\|=\~\|!\~\|==\|!=\|<=\|>=\|->\|??\|[-+*/%<>=&|^]/
syntax match sputnikOperator /\.?\./
syntax match sputnikOperator /\.\.\.\?/
syntax match sputnikOperator /?\ze\s*\[/
syntax match sputnikOperator /\%(\[\s*\)\@<=?/
syntax match sputnikOperator /!\ze\s*{/

call s:Match('sputnikMember', '\.\@1<=' . s:name)
call s:Match('sputnikFunctionCall', s:before . s:name . '\ze\s*(')
" Uppercase ASCII, Greek and Cyrillic type names used by the Sputnik lexer.
call s:Match('sputnikType', s:before . '[A-ZΑ-ΩА-ЯЁ]' . s:part . '*\%(?\%(\s*\[\)\@!\)\?')
call s:Match('sputnikFunctionCall', '^\s*\zs' . s:name
      \ . '\ze\s\+\%(["''0-9:@$&\[{]\|\%(true\|false\|null\)' . s:after . '\)')

call s:Match('sputnikNumber', s:before . s:digits . s:part . '\@!')
call s:Match('sputnikFloat', s:before . s:digits
      \ . '\%(\.' . s:digits . '\%([eE][+-]\?' . s:digits . '\)\?\|[eE][+-]\?' . s:digits . '\)' . s:part . '\@!')
call s:Match('sputnikNumber', s:before . '0[xX][0-9a-fA-F]\%(_\?[0-9a-fA-F]\)*' . s:part . '\@!')
call s:Match('sputnikNumber', s:before . '0[bB][01]\%(_\?[01]\)*' . s:part . '\@!')
call s:Match('sputnikNumber', s:before . '0[oO][0-7]\%(_\?[0-7]\)*' . s:part . '\@!')

call s:Match('sputnikBoolean', s:before . '\%(true\|false\)' . s:after)
call s:Match('sputnikConstant', s:before . 'null' . s:after)
call s:Match('sputnikSelf', s:before . 'self' . s:after)
call s:Match('sputnikPlaceholder', s:before . '_[0-9]\+' . s:part . '\@!')
call s:Match('sputnikPlaceholder', s:before . '\$it\%([1-9][0-9]*\)\?'
      \ . '\%(' . s:part . '\|?\|!\%([=~]\)\@!\)\@!')
call s:Match('sputnikWildcard', s:before . '_' . s:part . '\@!')
syntax match sputnikLastValue /\$_/
call s:Match('sputnikInstanceVariable', '@' . s:name)
call s:Match('sputnikClassVariable', '@@' . s:name)
" In mode::fast, only the second colon starts the symbol.
call s:Match('sputnikSymbol', ':\%(:\)\@!' . s:name)

call s:Match('sputnikConditional', s:before . '\%(case!\|if\|elif\|elsif\|else\|unless\|case\|when\)' . s:after)
call s:Match('sputnikRepeat', s:before . '\%(while\|until\|loop\|do\)' . s:after)
call s:Match('sputnikControl', s:before . '\%(break\|return\)' . s:after)
call s:Match('sputnikException', s:before . '\%(try\|catch\|rescue\|ensure\|raise\|throw\)' . s:after)
call s:Match('sputnikWordOperator', s:before . '\%(and\|or\|not\|in\|as\)' . s:after)
call s:Match('sputnikDeclaration', s:before . '\%(class_method\|class_prop\|def\|class\|mixin\|prop\|attr\|package\|import\|include\|from\|export\|extend\)' . s:after)

" Contextual words remain ordinary identifiers outside these forms.
call s:Match('sputnikModifier', s:before . 'native' . s:after . '\ze\s\+\%(def\|class\)' . s:after)
call s:Match('sputnikModifier', s:before . 'macro' . s:after . '\ze\s\+def' . s:after)
call s:Match('sputnikModifier', s:before . 'string_tag' . s:after . '\ze\s\+macro\s\+def' . s:after)
call s:Match('sputnikConditional', s:before . 'pattern' . s:after . '\ze\s*(')
call s:Match('sputnikConditional', s:before . 'with' . s:after . '\ze\s*{')
call s:Match('sputnikDirective', '^\s*\zsnumeric' . s:after . '\ze\s*:')
call s:Match('sputnikControl', s:before . 'quote' . s:after . '\ze\s*:\s*$')
call s:Match('sputnikPropertyModifier', '^\s*\zs\%(get\|set\)' . s:after . '\ze\s*\%(([^\r\n]*)\)\?\s*:')
call s:Match('sputnikControl', '^\s*\zsnext' . s:after
      \ . '\ze\s*\%($\|#\|["''0-9@:$]\|' . s:head . '\)')

call s:Match('sputnikDeclaration', s:before . 'def' . s:after,
      \ 'nextgroup=sputnikFunction skipwhite')
call s:Match('sputnikFunction', '\%(\[\]=\?\|\*\*\|//\|<=>\|===\|=\~\|!\~\|==\|!=\|<=\|>=\|<<\|>>\|[-+*/%&|^<>]\|' . s:name . '\)', 'contained')
call s:Match('sputnikDeclaration', s:before . '\%(class\|mixin\)' . s:after,
      \ 'nextgroup=sputnikClassName skipwhite')
call s:Match('sputnikClassName', s:head . s:part . '*', 'contained')
call s:Match('sputnikDeclaration', s:before . '\%(prop\|class_prop\)' . s:after,
      \ 'nextgroup=sputnikPropertyName skipwhite')
call s:Match('sputnikDeclaration', s:before . 'attr' . s:after,
      \ 'nextgroup=sputnikPropertyName,sputnikAttrModifier skipwhite')
call s:Match('sputnikPropertyName', s:name, 'contained')
call s:Match('sputnikAttrModifier', '\%(var\|set\)' . s:after,
      \ 'contained nextgroup=sputnikPropertyName skipwhite')

" A callable reference owns its # accessor, which cannot start a comment.
call s:Match('sputnikCallableReference', '&' . s:head . s:part
      \ . '*\%(\.' . s:head . s:part . '*\)*[?!]\?\%(#' . s:name . '\)\?',
      \ 'transparent contains=sputnikReferenceReceiver,sputnikReferenceMethod,sputnikReferenceDelimiter')
call s:Match('sputnikReferenceReceiver', s:head . s:part . '*\ze[.#]', 'contained')
call s:Match('sputnikReferenceMethod', s:name . '\%(' . s:part . '\|[.#!?]\)\@!', 'contained')
syntax match sputnikReferenceDelimiter /[&.#]/ contained

syntax cluster sputnikExpression contains=sputnikIdentifier,sputnikDelimiter,sputnikOperator,sputnikMember,sputnikFunctionCall,sputnikType,sputnikNumber,sputnikFloat,sputnikBoolean,sputnikConstant,sputnikSelf,sputnikPlaceholder,sputnikWildcard,sputnikLastValue,sputnikInstanceVariable,sputnikClassVariable,sputnikSymbol,sputnikConditional,sputnikRepeat,sputnikControl,sputnikException,sputnikWordOperator,sputnikDeclaration,sputnikModifier,sputnikDirective,sputnikPropertyModifier,sputnikCallableReference,sputnikComment,sputnikShebang,sputnikDoubleString,sputnikSingleString,sputnikTextBlock,sputnikStringTag,sputnikRawTag,sputnikInlineConditional,sputnikNativeClass

call s:Match('sputnikThen', s:before . 'then' . s:after, 'contained')
execute 'syntax region sputnikInlineConditional transparent matchgroup=sputnikConditional start=/'
      \ . s:before . 'if' . s:after . '\ze[^#\r\n]*' . s:before . 'then' . s:after
      \ . '/ end=/' . s:before . 'else' . s:after . '/ end=/$/ contains=@sputnikExpression,sputnikThen'
call s:Match('sputnikOwnership', '\%(owned\|borrowed\|collected\)' . s:after, 'contained')
execute 'syntax region sputnikNativeClass transparent matchgroup=sputnikModifier start=/'
      \ . s:before . 'native\ze\s\+class' . s:after
      \ . '/ end=/$/ contains=@sputnikExpression,sputnikOwnership'

" Comments require whitespace before # (or the beginning of the line).
syntax match sputnikComment /\%(^\|\s\)\@1<=#.*/ contains=sputnikTodo,@Spell
syntax match sputnikShebang /\%^#!.*/
syntax keyword sputnikTodo TODO FIXME XXX NOTE contained

syntax match sputnikDoubleEscape /\\\%([nrt\\"#]\|u{[0-9a-fA-F]\{1,6}}\)/ contained
syntax match sputnikSingleEscape /\\\%([nrt\\"#']\|u{[0-9a-fA-F]\{1,6}}\)/ contained
syntax match sputnikRawEscape /\\./ contained
syntax region sputnikSingleString start=/'/ skip=/\\./ end=/'/ contains=sputnikSingleEscape,@Spell
syntax region sputnikDoubleString start=/"/ skip=/\\./ end=/"/ contains=sputnikDoubleEscape,sputnikInterpolation,@Spell
syntax region sputnikTextBlock start=/"""/ skip=/\\./ end=/"""/ contains=sputnikDoubleEscape,sputnikInterpolation,@Spell
call s:Match('sputnikStringTag', s:before . s:head . s:part . '*\ze["'']',
      \ 'nextgroup=sputnikDoubleString,sputnikTaggedSingleString,sputnikTextBlock')
syntax region sputnikTaggedSingleString start=/'/ skip=/\\./ end=/'/ contained contains=sputnikSingleEscape,sputnikInterpolation,@Spell
call s:Match('sputnikRawTag', s:before . 'r\ze["'']',
      \ 'nextgroup=sputnikRawDoubleString,sputnikRawSingleString,sputnikRawTextBlock')
syntax region sputnikRawSingleString start=/'/ skip=/\\./ end=/'/ contained contains=sputnikRawEscape
syntax region sputnikRawDoubleString start=/"/ skip=/\\./ end=/"/ contained contains=sputnikRawEscape
syntax region sputnikRawTextBlock start=/"""/ skip=/\\./ end=/"""/ contained contains=sputnikRawEscape

" Recursive braces and strings keep nested interpolation balanced.
syntax region sputnikInterpolationBrace transparent matchgroup=sputnikDelimiter start=/{/ end=/}/ contained contains=@sputnikExpression,sputnikInterpolationBrace
syntax region sputnikInterpolation matchgroup=sputnikInterpolationDelimiter start=/#{/ end=/}/ contained contains=@sputnikExpression,sputnikInterpolationBrace
syntax region sputnikMacroSplice matchgroup=sputnikInterpolationDelimiter start=/#{/ end=/}/ contained contains=@sputnikExpression,sputnikInterpolationBrace
syntax region sputnikMacroDefinition transparent start=/^\z([ \t]*\)\%(string_tag\s\+\)\?macro\s\+def\s\+/ end=/^\%(\z1[ \t]\)\@!\%(\s*#\)\@!\ze\s*\S/ contains=@sputnikExpression,sputnikMacroSplice

" Parse from the start so long text blocks and macro bodies stay accurate.
syntax sync fromstart

highlight default link sputnikDelimiter Delimiter
highlight default link sputnikOperator Operator
highlight default link sputnikMember Identifier
highlight default link sputnikFunctionCall Function
highlight default link sputnikType Type
highlight default link sputnikClassName Type
highlight default link sputnikNumber Number
highlight default link sputnikFloat Float
highlight default link sputnikBoolean Boolean
highlight default link sputnikConstant Constant
highlight default link sputnikSelf Identifier
highlight default link sputnikPlaceholder Special
highlight default link sputnikWildcard Special
highlight default link sputnikLastValue Special
highlight default link sputnikInstanceVariable Identifier
highlight default link sputnikClassVariable Identifier
highlight default link sputnikSymbol Constant
highlight default link sputnikConditional Conditional
highlight default link sputnikThen Conditional
highlight default link sputnikRepeat Repeat
highlight default link sputnikControl Statement
highlight default link sputnikException Exception
highlight default link sputnikWordOperator Operator
highlight default link sputnikDeclaration Keyword
highlight default link sputnikModifier StorageClass
highlight default link sputnikOwnership StorageClass
highlight default link sputnikDirective PreProc
highlight default link sputnikPropertyModifier StorageClass
highlight default link sputnikAttrModifier StorageClass
highlight default link sputnikFunction Function
highlight default link sputnikPropertyName Function
highlight default link sputnikReferenceReceiver Type
highlight default link sputnikReferenceMethod Function
highlight default link sputnikReferenceDelimiter Operator
highlight default link sputnikComment Comment
highlight default link sputnikShebang Comment
highlight default link sputnikTodo Todo
highlight default link sputnikDoubleEscape SpecialChar
highlight default link sputnikSingleEscape SpecialChar
highlight default link sputnikRawEscape SpecialChar
highlight default link sputnikSingleString String
highlight default link sputnikDoubleString String
highlight default link sputnikTextBlock String
highlight default link sputnikStringTag Function
highlight default link sputnikTaggedSingleString String
highlight default link sputnikRawTag Function
highlight default link sputnikRawSingleString String
highlight default link sputnikRawDoubleString String
highlight default link sputnikRawTextBlock String
highlight default link sputnikInterpolationDelimiter Special

let b:current_syntax = 'sputnik'
let &cpo = s:save_cpo
unlet s:save_cpo s:head s:part s:suffix s:name s:before s:after s:digits
delfunction s:Match
