" Vim syntax file
" Language: Amber
" Based on editors/vscode/syntaxes/amber.tmLanguage.json.
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
call s:Match('amberIdentifier', s:before . s:name)
syntax match amberDelimiter /[.,:;()[\]{}]/
syntax match amberOperator /\/\/=\|+=\|-=\|\*=\|\/=\|%=\|===\|<=>\|<<\|>>\|\*\*\|\/\/\|=\~\|!\~\|==\|!=\|<=\|>=\|->\|??\|[-+*/%<>=&|^]/
syntax match amberOperator /\.?\./
syntax match amberOperator /\.\.\.\?/
syntax match amberOperator /?\ze\s*\[/
syntax match amberOperator /\%(\[\s*\)\@<=?/
syntax match amberOperator /!\ze\s*{/

call s:Match('amberMember', '\.\@1<=' . s:name)
call s:Match('amberFunctionCall', s:before . s:name . '\ze\s*(')
" Uppercase ASCII, Greek and Cyrillic type names used by the Amber lexer.
call s:Match('amberType', s:before . '[A-ZΑ-ΩА-ЯЁ]' . s:part . '*\%(?\%(\s*\[\)\@!\)\?')
call s:Match('amberFunctionCall', '^\s*\zs' . s:name
      \ . '\ze\s\+\%(["''0-9:@$&\[{]\|\%(true\|false\|null\)' . s:after . '\)')

call s:Match('amberNumber', s:before . s:digits . s:part . '\@!')
call s:Match('amberFloat', s:before . s:digits
      \ . '\%(\.' . s:digits . '\%([eE][+-]\?' . s:digits . '\)\?\|[eE][+-]\?' . s:digits . '\)' . s:part . '\@!')
call s:Match('amberNumber', s:before . '0[xX][0-9a-fA-F]\%(_\?[0-9a-fA-F]\)*' . s:part . '\@!')
call s:Match('amberNumber', s:before . '0[bB][01]\%(_\?[01]\)*' . s:part . '\@!')
call s:Match('amberNumber', s:before . '0[oO][0-7]\%(_\?[0-7]\)*' . s:part . '\@!')

call s:Match('amberBoolean', s:before . '\%(true\|false\)' . s:after)
call s:Match('amberConstant', s:before . 'null' . s:after)
call s:Match('amberSelf', s:before . 'self' . s:after)
call s:Match('amberPlaceholder', s:before . '_[0-9]\+' . s:part . '\@!')
call s:Match('amberPlaceholder', s:before . '\$it\%([1-9][0-9]*\)\?'
      \ . '\%(' . s:part . '\|?\|!\%([=~]\)\@!\)\@!')
call s:Match('amberWildcard', s:before . '_' . s:part . '\@!')
syntax match amberLastValue /\$_/
call s:Match('amberInstanceVariable', '@' . s:name)
call s:Match('amberClassVariable', '@@' . s:name)
" In mode::fast, only the second colon starts the symbol.
call s:Match('amberSymbol', ':\%(:\)\@!' . s:name)

call s:Match('amberConditional', s:before . '\%(case!\|if\|elif\|elsif\|else\|unless\|case\|when\)' . s:after)
call s:Match('amberRepeat', s:before . '\%(while\|until\|loop\|do\)' . s:after)
call s:Match('amberControl', s:before . '\%(break\|return\)' . s:after)
call s:Match('amberException', s:before . '\%(try\|catch\|rescue\|ensure\|raise\|throw\)' . s:after)
call s:Match('amberWordOperator', s:before . '\%(and\|or\|not\|in\|as\)' . s:after)
call s:Match('amberDeclaration', s:before . '\%(class_method\|class_prop\|def\|class\|mixin\|prop\|attr\|package\|import\|include\|from\|export\|extend\)' . s:after)

" Contextual words remain ordinary identifiers outside these forms.
call s:Match('amberModifier', s:before . 'native' . s:after . '\ze\s\+\%(def\|class\)' . s:after)
call s:Match('amberModifier', s:before . 'macro' . s:after . '\ze\s\+def' . s:after)
call s:Match('amberModifier', s:before . 'string_tag' . s:after . '\ze\s\+macro\s\+def' . s:after)
call s:Match('amberConditional', s:before . 'pattern' . s:after . '\ze\s*(')
call s:Match('amberConditional', s:before . 'with' . s:after . '\ze\s*{')
call s:Match('amberDirective', '^\s*\zsnumeric' . s:after . '\ze\s*:')
call s:Match('amberControl', s:before . 'quote' . s:after . '\ze\s*:\s*$')
call s:Match('amberPropertyModifier', '^\s*\zs\%(get\|set\)' . s:after . '\ze\s*\%(([^\r\n]*)\)\?\s*:')
call s:Match('amberControl', '^\s*\zsnext' . s:after
      \ . '\ze\s*\%($\|#\|["''0-9@:$]\|' . s:head . '\)')

call s:Match('amberDeclaration', s:before . 'def' . s:after,
      \ 'nextgroup=amberFunction skipwhite')
call s:Match('amberFunction', '\%(\[\]=\?\|\*\*\|//\|<=>\|===\|=\~\|!\~\|==\|!=\|<=\|>=\|<<\|>>\|[-+*/%&|^<>]\|' . s:name . '\)', 'contained')
call s:Match('amberDeclaration', s:before . '\%(class\|mixin\)' . s:after,
      \ 'nextgroup=amberClassName skipwhite')
call s:Match('amberClassName', s:head . s:part . '*', 'contained')
call s:Match('amberDeclaration', s:before . '\%(prop\|class_prop\)' . s:after,
      \ 'nextgroup=amberPropertyName skipwhite')
call s:Match('amberDeclaration', s:before . 'attr' . s:after,
      \ 'nextgroup=amberPropertyName,amberAttrModifier skipwhite')
call s:Match('amberPropertyName', s:name, 'contained')
call s:Match('amberAttrModifier', '\%(var\|set\)' . s:after,
      \ 'contained nextgroup=amberPropertyName skipwhite')

" A callable reference owns its # accessor, which cannot start a comment.
call s:Match('amberCallableReference', '&' . s:head . s:part
      \ . '*\%(\.' . s:head . s:part . '*\)*[?!]\?\%(#' . s:name . '\)\?',
      \ 'transparent contains=amberReferenceReceiver,amberReferenceMethod,amberReferenceDelimiter')
call s:Match('amberReferenceReceiver', s:head . s:part . '*\ze[.#]', 'contained')
call s:Match('amberReferenceMethod', s:name . '\%(' . s:part . '\|[.#!?]\)\@!', 'contained')
syntax match amberReferenceDelimiter /[&.#]/ contained

syntax cluster amberExpression contains=amberIdentifier,amberDelimiter,amberOperator,amberMember,amberFunctionCall,amberType,amberNumber,amberFloat,amberBoolean,amberConstant,amberSelf,amberPlaceholder,amberWildcard,amberLastValue,amberInstanceVariable,amberClassVariable,amberSymbol,amberConditional,amberRepeat,amberControl,amberException,amberWordOperator,amberDeclaration,amberModifier,amberDirective,amberPropertyModifier,amberCallableReference,amberComment,amberShebang,amberDoubleString,amberSingleString,amberTextBlock,amberStringTag,amberRawTag,amberInlineConditional,amberNativeClass

call s:Match('amberThen', s:before . 'then' . s:after, 'contained')
execute 'syntax region amberInlineConditional transparent matchgroup=amberConditional start=/'
      \ . s:before . 'if' . s:after . '\ze[^#\r\n]*' . s:before . 'then' . s:after
      \ . '/ end=/' . s:before . 'else' . s:after . '/ end=/$/ contains=@amberExpression,amberThen'
call s:Match('amberOwnership', '\%(owned\|borrowed\|collected\)' . s:after, 'contained')
execute 'syntax region amberNativeClass transparent matchgroup=amberModifier start=/'
      \ . s:before . 'native\ze\s\+class' . s:after
      \ . '/ end=/$/ contains=@amberExpression,amberOwnership'

" Comments require whitespace before # (or the beginning of the line).
syntax match amberComment /\%(^\|\s\)\@1<=#.*/ contains=amberTodo,@Spell
syntax match amberShebang /\%^#!.*/
syntax keyword amberTodo TODO FIXME XXX NOTE contained

syntax match amberDoubleEscape /\\\%([nrt\\"#]\|u{[0-9a-fA-F]\{1,6}}\)/ contained
syntax match amberSingleEscape /\\\%([nrt\\"#']\|u{[0-9a-fA-F]\{1,6}}\)/ contained
syntax match amberRawEscape /\\./ contained
syntax region amberSingleString start=/'/ skip=/\\./ end=/'/ contains=amberSingleEscape,@Spell
syntax region amberDoubleString start=/"/ skip=/\\./ end=/"/ contains=amberDoubleEscape,amberInterpolation,@Spell
syntax region amberTextBlock start=/"""/ skip=/\\./ end=/"""/ contains=amberDoubleEscape,amberInterpolation,@Spell
call s:Match('amberStringTag', s:before . s:head . s:part . '*\ze["'']',
      \ 'nextgroup=amberDoubleString,amberTaggedSingleString,amberTextBlock')
syntax region amberTaggedSingleString start=/'/ skip=/\\./ end=/'/ contained contains=amberSingleEscape,amberInterpolation,@Spell
call s:Match('amberRawTag', s:before . 'r\ze["'']',
      \ 'nextgroup=amberRawDoubleString,amberRawSingleString,amberRawTextBlock')
syntax region amberRawSingleString start=/'/ skip=/\\./ end=/'/ contained contains=amberRawEscape
syntax region amberRawDoubleString start=/"/ skip=/\\./ end=/"/ contained contains=amberRawEscape
syntax region amberRawTextBlock start=/"""/ skip=/\\./ end=/"""/ contained contains=amberRawEscape

" Recursive braces and strings keep nested interpolation balanced.
syntax region amberInterpolationBrace transparent matchgroup=amberDelimiter start=/{/ end=/}/ contained contains=@amberExpression,amberInterpolationBrace
syntax region amberInterpolation matchgroup=amberInterpolationDelimiter start=/#{/ end=/}/ contained contains=@amberExpression,amberInterpolationBrace
syntax region amberMacroSplice matchgroup=amberInterpolationDelimiter start=/#{/ end=/}/ contained contains=@amberExpression,amberInterpolationBrace
syntax region amberMacroDefinition transparent start=/^\z([ \t]*\)\%(string_tag\s\+\)\?macro\s\+def\s\+/ end=/^\%(\z1[ \t]\)\@!\%(\s*#\)\@!\ze\s*\S/ contains=@amberExpression,amberMacroSplice

" Parse from the start so long text blocks and macro bodies stay accurate.
syntax sync fromstart

highlight default link amberDelimiter Delimiter
highlight default link amberOperator Operator
highlight default link amberMember Identifier
highlight default link amberFunctionCall Function
highlight default link amberType Type
highlight default link amberClassName Type
highlight default link amberNumber Number
highlight default link amberFloat Float
highlight default link amberBoolean Boolean
highlight default link amberConstant Constant
highlight default link amberSelf Identifier
highlight default link amberPlaceholder Special
highlight default link amberWildcard Special
highlight default link amberLastValue Special
highlight default link amberInstanceVariable Identifier
highlight default link amberClassVariable Identifier
highlight default link amberSymbol Constant
highlight default link amberConditional Conditional
highlight default link amberThen Conditional
highlight default link amberRepeat Repeat
highlight default link amberControl Statement
highlight default link amberException Exception
highlight default link amberWordOperator Operator
highlight default link amberDeclaration Keyword
highlight default link amberModifier StorageClass
highlight default link amberOwnership StorageClass
highlight default link amberDirective PreProc
highlight default link amberPropertyModifier StorageClass
highlight default link amberAttrModifier StorageClass
highlight default link amberFunction Function
highlight default link amberPropertyName Function
highlight default link amberReferenceReceiver Type
highlight default link amberReferenceMethod Function
highlight default link amberReferenceDelimiter Operator
highlight default link amberComment Comment
highlight default link amberShebang Comment
highlight default link amberTodo Todo
highlight default link amberDoubleEscape SpecialChar
highlight default link amberSingleEscape SpecialChar
highlight default link amberRawEscape SpecialChar
highlight default link amberSingleString String
highlight default link amberDoubleString String
highlight default link amberTextBlock String
highlight default link amberStringTag Function
highlight default link amberTaggedSingleString String
highlight default link amberRawTag Function
highlight default link amberRawSingleString String
highlight default link amberRawDoubleString String
highlight default link amberRawTextBlock String
highlight default link amberInterpolationDelimiter Special

let b:current_syntax = 'amber'
let &cpo = s:save_cpo
unlet s:save_cpo s:head s:part s:suffix s:name s:before s:after s:digits
delfunction s:Match
