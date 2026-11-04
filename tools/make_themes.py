# Builds the theme CSS for Index.html: colour tokens per theme, and for the cute themes a scattered
# background pattern and a header logo made from Microsoft Fluent Emoji (MIT licence), fetched from
# a pinned commit so the result is reproducible. Usage: python -I make_themes.py <out.css>
import base64, re, sys, urllib.request

COMMIT = '1ffb34c752ecf5d402f04cfb4b392c77f57c54bc'
BASE = f'https://raw.githubusercontent.com/microsoft/fluentui-emoji/{COMMIT}/assets/'
EMOJI = {
    'heart': 'Pink heart/Flat/pink_heart_flat.svg',
    'ribbon': 'Ribbon/Flat/ribbon_flat.svg',
    'sparkles': 'Sparkles/Flat/sparkles_flat.svg',
    'butterfly': 'Butterfly/Flat/butterfly_flat.svg',
    'moon': 'Crescent moon/Flat/crescent_moon_flat.svg',
    'clover': 'Four leaf clover/Flat/four_leaf_clover_flat.svg',
    'cloud': 'Cloud/Flat/cloud_flat.svg',
    'strawberry': 'Strawberry/Flat/strawberry_flat.svg',
    'hibiscus': 'Hibiscus/Flat/hibiscus_flat.svg',
    'star': 'Star/Flat/star_flat.svg',
    'blossom': 'Cherry blossom/Flat/cherry_blossom_flat.svg',
}

def fetch(name):
    url = BASE + urllib.request.quote(EMOJI[name])
    return urllib.request.urlopen(url, timeout=30).read().decode('utf-8')

def inner(svg, prefix):
    """The emoji's drawing without its outer <svg>, ids made unique with a prefix."""
    svg = re.sub(r'<\?xml[^>]*>', '', svg).strip()
    body = re.sub(r'^<svg[^>]*>|</svg>\s*$', '', svg, flags=re.S).strip()
    body = re.sub(r'id="([^"]+)"', lambda m: f'id="{prefix}{m.group(1)}"', body)
    body = re.sub(r'url\(#([^)]+)\)', lambda m: f'url(#{prefix}{m.group(1)})', body)
    body = re.sub(r'href="#([^"]+)"', lambda m: f'href="#{prefix}{m.group(1)}"', body)
    return body

def data_uri(svg):
    return 'url("data:image/svg+xml;base64,' + base64.b64encode(svg.encode('utf-8')).decode('ascii') + '")'

# a 260 px tile: each emoji drawn once as a symbol, then placed a few times, small and tilted
SPOTS = [(22, 26, 30, -14, 0), (150, 14, 22, 12, 1), (96, 104, 34, 6, 2), (206, 132, 26, -20, 0),
         (28, 184, 22, 16, 1), (160, 214, 28, -6, 2)]

def pattern(names, opacity):
    defs = ''.join(f'<symbol id="s{k}" viewBox="0 0 32 32">{inner(fetch(n), f"e{k}")}</symbol>' for k, n in enumerate(names))
    uses = ''.join(f'<use href="#s{k % len(names)}" x="{x}" y="{y}" width="{s}" height="{s}" transform="rotate({r} {x + s / 2} {y + s / 2})"/>'
                   for x, y, s, r, k in SPOTS)
    return data_uri(f'<svg xmlns="http://www.w3.org/2000/svg" width="260" height="260"><defs>{defs}</defs><g opacity="{opacity}">{uses}</g></svg>')

def logo(name):
    return data_uri(fetch(name))

# name: (label, colour tokens, cute: (pattern emoji, logo emoji, pattern opacity) or None)
THEMES = {
    'dark':     ('Dark',     dict(bg='#0f0f10', panel='#18181b', line='#2a2a2f', text='#ececee', dim='#8b8b93', accent='#e8b04a', on='#141414', scheme='dark'), None),
    'gray':     ('Gray',     dict(bg='#2b2d31', panel='#35373c', line='#46494f', text='#f0f1f3', dim='#a3a6ad', accent='#8ab4f8', on='#10131a', scheme='dark'), None),
    'white':    ('White',    dict(bg='#f6f6f8', panel='#ffffff', line='#e3e3e8', text='#1b1b1f', dim='#6b6b76', accent='#2f6feb', on='#ffffff', scheme='light'), None),
    'pink':     ('Pink',     dict(bg='#fff0f6', panel='#ffffff', line='#ffd1e3', text='#5a2440', dim='#b0728f', accent='#ff5fa2', on='#ffffff', scheme='light'), (['heart', 'ribbon', 'sparkles'], 'ribbon', .22)),
    'lavender': ('Lavender', dict(bg='#f5f0ff', panel='#ffffff', line='#e2d6ff', text='#3d2a66', dim='#8a78b3', accent='#9b6bff', on='#ffffff', scheme='light'), (['butterfly', 'moon', 'sparkles'], 'butterfly', .2)),
    'mint':     ('Mint',     dict(bg='#ecfbf4', panel='#ffffff', line='#c9efdc', text='#1f4a39', dim='#6a9a86', accent='#25b07e', on='#ffffff', scheme='light'), (['clover', 'cloud', 'sparkles'], 'clover', .22)),
    'peach':    ('Peach',    dict(bg='#fff4ec', panel='#ffffff', line='#ffdcc6', text='#5b3220', dim='#b07d63', accent='#ff8a5b', on='#ffffff', scheme='light'), (['strawberry', 'hibiscus', 'star'], 'strawberry', .22)),
    'sky':      ('Sky',      dict(bg='#eef6ff', panel='#ffffff', line='#cfe3ff', text='#1d3557', dim='#6f8db3', accent='#4b9dff', on='#ffffff', scheme='light'), (['cloud', 'star', 'sparkles'], 'cloud', .24)),
    'berry':    ('Berry night', dict(bg='#1c1220', panel='#2a1a2f', line='#43294a', text='#ffe6f2', dim='#c79ab3', accent='#ff7ab8', on='#2a0d1c', scheme='dark'), (['blossom', 'heart', 'moon'], 'blossom', .16)),
}

out = ['/* ---------- Themes ----------',
       '   Colour tokens per theme (Settings → Theme). The cute themes add a scattered background pattern and',
       '   a header logo made from Microsoft Fluent Emoji (MIT licence, github.com/microsoft/fluentui-emoji,',
       f'   commit {COMMIT[:7]}), a rounder shape and the Nunito font (SIL Open Font Licence, Google Fonts).',
       '   Generated; see NOTES.md. */']
for key, (label, t, cute) in THEMES.items():
    # any element can carry data-theme (the Settings swatches preview themes that way)
    sel = ':root, [data-theme="dark"]' if key == 'dark' else f'[data-theme="{key}"]'
    rules = [f'--bg: {t["bg"]}', f'--panel: {t["panel"]}', f'--line: {t["line"]}', f'--text: {t["text"]}',
             f'--dim: {t["dim"]}', f'--accent: {t["accent"]}', f'--on-accent: {t["on"]}', f'color-scheme: {t["scheme"]}']
    if cute:
        names, logo_name, opacity = cute
        rules += [f'--pattern: {pattern(names, opacity)}', f'--logo: {logo(logo_name)}', '--logo-size: 26px', '--radius: 16px',
                  '--btn-radius: 999px', '--field-radius: 12px', '--font: "Nunito", ui-rounded, "SF Pro Rounded", system-ui, sans-serif']
    else:  # every token is set by every theme, so nothing leaks in from an outer one
        rules += ['--pattern: none', '--logo: none', '--logo-size: 0px', '--radius: 8px', '--btn-radius: 6px', '--field-radius: 6px',
                  '--font: system-ui, -apple-system, "Segoe UI", sans-serif']
    out.append(f'  {sel} {{ ' + '; '.join(rules) + '; }')
css = '\n'.join(out) + '\n'
open(sys.argv[1], 'w', encoding='utf-8', newline='\n').write(css)
print(len(css), 'bytes')
print(', '.join(f'{k}:{v[0]}' for k, v in THEMES.items()))
