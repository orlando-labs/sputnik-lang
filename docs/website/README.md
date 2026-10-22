# Standalone website updates

The sibling `site/` directory is not a Git checkout. Its effect-iteration guide
and syntax-highlighter changes are preserved in `effect-iteration.patch` so
they travel with the language commit. From a clean copy of the website:

```sh
patch -p1 -d ../site < docs/website/effect-iteration.patch
python3 tools/sync_cheat_sheet.py --site-dir ../site/public
cp sputnik_unified_final_spec.md ../site/public/spec/sputnik_unified_final_spec.md
python3 ../site/tools/gen_guide_manifest.py
make -C ../site check
```

The patch is already applied in this workspace. It intentionally omits the
large generated spec and cheat-sheet copies: the commands above publish the
canonical sources from this repository.
