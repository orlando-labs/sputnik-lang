" .s is also used by assembly. This plugin opts into Sputnik for .s files;
" use :setfiletype asm when editing assembly, or disable this detector.
augroup sputnik_filetype
  autocmd!
  autocmd BufRead,BufNewFile *.spu,*.sputnik setfiletype sputnik
  autocmd BufRead,BufNewFile *.s if &l:filetype ==# '' || &l:filetype ==# 'asm' | setlocal filetype=sputnik | endif
augroup END
