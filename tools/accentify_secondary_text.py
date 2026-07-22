import re, sys, glob, os
DRY = '--apply' not in sys.argv
# Only text_color calls, only NEUTRAL greys (R==G==B). Bright -> ACCENT, dim -> ACCENT_DIM.
pat = re.compile(r'(set_style_text_color\s*\([^,]+,\s*)lv_color_make\(\s*0x([0-9A-Fa-f]{2})\s*,\s*0x([0-9A-Fa-f]{2})\s*,\s*0x([0-9A-Fa-f]{2})\s*\)')
def repl(m):
    r,g,b = int(m.group(2),16), int(m.group(3),16), int(m.group(4),16)
    if not (r==g==b):            # keep semantic (non-neutral) colors
        return m.group(0)
    macro = 'ARGUS_ACCENT' if r >= 0x99 else 'ARGUS_ACCENT_DIM'
    return m.group(1) + macro
total=0; files=0
for f in glob.glob('src/*.cpp'):
    s = open(f, encoding='utf-8', errors='replace').read()
    new, n = pat.subn(repl, s)
    if n:
        # ensure theme.h include present
        if '#include "theme.h"' not in new:
            new = new.replace('#include "'+os.path.basename(f).replace('.cpp','.h')+'"',
                              '#include "'+os.path.basename(f).replace('.cpp','.h')+'"\n#include "theme.h"', 1)
        total+=n; files+=1
        print(f"{n:3d}  {f}")
        if not DRY: open(f,'w',encoding='utf-8').write(new)
print(f"--- {'DRY-RUN ' if DRY else 'APPLIED '}{total} replacements across {files} files ---")
