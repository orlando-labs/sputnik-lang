" Vim already assigns *.am to ELF. Override that default for Amber, while
" preserving more specific detections such as Automake's Makefile.am.
autocmd BufRead,BufNewFile *.am if &l:filetype ==# '' || &l:filetype ==# 'elf' | setlocal filetype=amber | endif
