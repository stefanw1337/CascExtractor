CASC Extractor
==============

Browse a local WoW install (any CASC build, incl. the Forever beta), tick files or folders, extract them.
Files that fail are skipped and listed in _extract_failed.txt in the output folder.

1. Start CascExtractor.exe. Press Open. The first time it downloads the community listfile (~150 MB).
2. Tick folders/files in the tree (click the box or press Space), or use the Filter box:
     plain text       path contains it          e.g.  character/scourge/female
     * and ?          wildcards on full path    e.g.  character/*/female/*_hd_face*.blp
     a number         file ID                   e.g.  3459505
     re:...           regular expression        e.g.  re:_hd_\d+\.blp$
   Separate several with ;   Press Enter or "Check matching".
   "Check from list..." ticks every path / file ID listed in a .txt (one per line).
3. Press "Extract checked". Files already extracted are skipped, so re-running is cheap.

If it is closed or crashes mid-job: start it, press Open, then "Resume last job".
A file that crashed the program is skipped automatically next time.

Built with CascLib by Ladislav Zezula (MIT).
