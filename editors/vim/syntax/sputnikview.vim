" Sputnik external template body, with Sputnik expressions/control headers.
if exists('b:current_syntax')
  finish
endif
runtime syntax/sputnik.vim
syntax region sputnikViewText start=/\%^/ end=/\%$/ contains=sputnikDoubleEscape,sputnikInterpolation,sputnikTemplateFor,sputnikTemplateComment,sputnikTemplatePercent,@Spell
highlight default link sputnikViewText String
let b:current_syntax = 'sputnikview'
