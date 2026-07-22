import re, glob
# Reverse the accentify sweep: text_color ARGUS_ACCENT -> ARGUS_TEXT (cream),
# ARGUS_ACCENT_DIM -> ARGUS_TEXT_DIM. ONLY bare-macro text_color calls (exactly
# what accentify produced); leaves argus_accent() titles and non-text uses alone.
pat = re.compile(r'(set_style_text_color\s*\([^,]+,\s*)ARGUS_ACCENT(_DIM)?\b')
def repl(m):
    return m.group(1) + ('ARGUS_TEXT_DIM' if m.group(2) else 'ARGUS_TEXT')
tot=0; files=0
for f in glob.glob('src/*.cpp'):
    s=open(f,encoding='utf-8',errors='replace').read()
    new,n=pat.subn(repl,s)
    if n:
        open(f,'w',encoding='utf-8').write(new); tot+=n; files+=1; print(f"{n:3d}  {f}")
print(f"--- reverted {tot} text_color spots across {files} files to cream ---")
