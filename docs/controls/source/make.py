"""Builds the IB3Pad control sheets (HTML -> PNG with Playwright)."""
import json, subprocess, pathlib

OUT = pathlib.Path(__file__).parent
W, H = 1600, 1100
CX, CY = 400, 330  # controller drawing origin on the canvas (drawing is 800 x 520)

# anchor points inside the 800 x 520 controller drawing
A = {
    'LT': (205, 28), 'RT': (595, 28), 'LB': (215, 78), 'RB': (585, 78),
    'LS': (232, 192), 'DP': (318, 300), 'RS': (482, 300),
    'Y': (566, 146), 'X': (524, 188), 'B': (608, 188), 'A': (566, 230),
    'BACK': (350, 190), 'START': (450, 190),
}
CHIP = {  # label, fill, text colour
    'LT': ('LT', '#3a3f4b', '#e9edf5'), 'RT': ('RT', '#3a3f4b', '#e9edf5'),
    'LB': ('LB', '#3a3f4b', '#e9edf5'), 'RB': ('RB', '#3a3f4b', '#e9edf5'),
    'LS': ('L', '#2b2f38', '#e9edf5'), 'RS': ('R', '#2b2f38', '#e9edf5'),
    'L3': ('L3', '#2b2f38', '#e9edf5'), 'R3': ('R3', '#2b2f38', '#e9edf5'),
    'DP': ('+', '#2b2f38', '#e9edf5'),
    'A': ('A', '#3fae5a', '#ffffff'), 'B': ('B', '#d9473f', '#ffffff'),
    'X': ('X', '#3b7bd8', '#ffffff'), 'Y': ('Y', '#e5b52e', '#1b1b1b'),
    'BACK': ('&#9783;', '#2b2f38', '#e9edf5'), 'START': ('&#9776;', '#2b2f38', '#e9edf5'),
}


def controller_svg():
    return '''
<svg class="pad" viewBox="0 0 800 520" width="800" height="520">
  <defs>
    <linearGradient id="body" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#2d323c"/><stop offset="1" stop-color="#1c2027"/>
    </linearGradient>
  </defs>
  <!-- triggers and bumpers -->
  <path d="M160 70 Q170 18 220 14 L262 14 Q282 16 284 40 L286 70 Z" fill="#3a3f4b" stroke="#4b5261" stroke-width="2"/>
  <path d="M640 70 Q630 18 580 14 L538 14 Q518 16 516 40 L514 70 Z" fill="#3a3f4b" stroke="#4b5261" stroke-width="2"/>
  <path d="M150 104 Q160 66 215 62 L300 66 Q312 70 312 90 L312 104 Z" fill="#454b58" stroke="#565e6e" stroke-width="2"/>
  <path d="M650 104 Q640 66 585 62 L500 66 Q488 70 488 90 L488 104 Z" fill="#454b58" stroke="#565e6e" stroke-width="2"/>
  <!-- body -->
  <path d="M150 100 Q240 84 320 96 L480 96 Q560 84 650 100 Q720 116 742 210 Q770 330 760 420
           Q752 488 700 498 Q648 506 612 446 L560 360 Q548 344 528 344 L272 344 Q252 344 240 360
           L188 446 Q152 506 100 498 Q48 488 40 420 Q30 330 58 210 Q80 116 150 100 Z"
        fill="url(#body)" stroke="#4b5261" stroke-width="2.5"/>
  <!-- centre plate -->
  <circle cx="400" cy="128" r="24" fill="#262a32" stroke="#4b5261" stroke-width="2"/>
  <path d="M388 128 L400 116 L412 128 L400 140 Z" fill="#e6c35a" opacity=".85"/>
  <!-- back / start -->
  <circle cx="350" cy="190" r="13" fill="#262a32" stroke="#5a6272" stroke-width="2"/>
  <circle cx="450" cy="190" r="13" fill="#262a32" stroke="#5a6272" stroke-width="2"/>
  <!-- left stick -->
  <circle cx="232" cy="192" r="54" fill="#15181d" stroke="#3b414d" stroke-width="2"/>
  <circle cx="232" cy="192" r="36" fill="#2a2f38" stroke="#5a6272" stroke-width="3"/>
  <!-- right stick -->
  <circle cx="482" cy="300" r="50" fill="#15181d" stroke="#3b414d" stroke-width="2"/>
  <circle cx="482" cy="300" r="34" fill="#2a2f38" stroke="#5a6272" stroke-width="3"/>
  <!-- d-pad -->
  <circle cx="318" cy="300" r="50" fill="#15181d" stroke="#3b414d" stroke-width="2"/>
  <path d="M306 262 h24 v26 h26 v24 h-26 v26 h-24 v-26 h-26 v-24 h26 Z" fill="#2a2f38" stroke="#5a6272" stroke-width="2"/>
  <!-- face buttons -->
  <circle cx="566" cy="146" r="19" fill="#e5b52e"/><text x="566" y="153" class="fb" fill="#1b1b1b">Y</text>
  <circle cx="524" cy="188" r="19" fill="#3b7bd8"/><text x="524" y="195" class="fb">X</text>
  <circle cx="608" cy="188" r="19" fill="#d9473f"/><text x="608" y="195" class="fb">B</text>
  <circle cx="566" cy="230" r="19" fill="#3fae5a"/><text x="566" y="237" class="fb">A</text>
  <text x="205" y="44" class="sh">LT</text><text x="595" y="44" class="sh">RT</text>
  <text x="232" y="91" class="sh">LB</text><text x="568" y="91" class="sh">RB</text>
</svg>'''


def chip(key):
    lab, fill, col = CHIP[key]
    return f'<span class="chip" style="background:{fill};color:{col}">{lab}</span>'


FACE = {'Y', 'X', 'B', 'A'}
SLOT = {'T0': (420, 186), 'T1': (850, 186), 'B0': (420, 905), 'B1': (850, 905)}


def label_html(item, x, y):
    chips = ''.join(chip(k) for k in item['keys'])
    return (f'<div class="lab" style="left:{x}px;top:{y}px">'
            f'<div class="chips">{chips}</div><div class="txt"><div class="t">{item["title"]}</div>'
            f'<div class="s">{item.get("sub", "")}</div></div></div>')


def spread(n, top=170, bottom=890):
    if n == 1:
        return [top]
    step = (bottom - top) / (n - 1)
    return [int(top + i * step) for i in range(n)]


def layout(sheet):
    labs, lines = [], []
    for side in ('L', 'R'):
        items = sheet['left' if side == 'L' else 'right']
        fixed = {'LT': 328, 'RT': 328, 'LB': 432, 'RB': 432}
        top = [i for i in items if i['anchor'] in fixed]
        rest = [i for i in items if i['anchor'] not in fixed]
        ys = {id(i): fixed[i['anchor']] for i in top}
        start = 560 if (top or side == 'R') else 170
        for i, y in zip(rest, spread(len(rest), start, min(890, start + (len(rest) - 1) * 125))):
            ys[id(i)] = y
        sheet['_ys_' + side] = ys
        for item in items:
            y = ys[id(item)]
            x = 40 if side == 'L' else W - 40 - 330
            labs.append(label_html(item, x, y))
            if item['anchor'] in FACE:
                continue
            ax, ay = A[item['anchor']]
            ex = 40 + 330 + 8 if side == 'L' else W - 40 - 330 - 8
            mx = CX - 14 if side == 'L' else CX + 800 + 14
            lines.append(f'<path d="M{ex} {y + 30} L{mx} {y + 30} L{CX + ax} {CY + ay}"/>'
                         f'<circle cx="{CX + ax}" cy="{CY + ay}" r="5"/>')
    for item in sheet.get('center', []):
        x, y = SLOT[item['slot']]
        labs.append(label_html(item, x, y))
        ax, ay = A[item['anchor']]
        top = item['slot'][0] == 'T'
        ex = x + (280 if item['slot'][1] == '0' else 50)
        ey = y + 76 if top else y
        lines.append(f'<path d="M{ex} {ey} L{CX + ax} {CY + ay}"/><circle cx="{CX + ax}" cy="{CY + ay}" r="5"/>')
    face = [i for i in sheet['right'] if i['anchor'] in FACE]
    if face:  # one bracket from the face-button labels to the face-button cluster
        ys = sorted(sheet['_ys_R'][id(i)] for i in face)
        bx = W - 40 - 330 - 14
        lines.append(f'<path d="M{bx + 8} {ys[0] + 8} L{bx} {ys[0] + 8} L{bx} {ys[-1] + 68} L{bx + 8} {ys[-1] + 68}"/>'
                     f'<path d="M{bx} {(ys[0] + ys[-1]) // 2 + 38} L{CX + 640} {CY + 188}"/>'
                     f'<circle cx="{CX + 640}" cy="{CY + 188}" r="5"/>')
    return ''.join(labs), f'<svg class="lines" width="{W}" height="{H}">{"".join(lines)}</svg>'


CSS = '''
*{box-sizing:border-box;margin:0;padding:0}
body{width:%dpx;height:%dpx;background:#101318;font-family:Inter,'DejaVu Sans',sans-serif;color:#e9edf5;
 position:relative;overflow:hidden}
.bg{position:absolute;inset:0;background:radial-gradient(ellipse at 50%% 55%%,#1d2330 0%%,#101318 62%%)}
.head{position:absolute;left:40px;right:40px;top:34px;display:flex;align-items:flex-end;justify-content:space-between;
 border-bottom:2px solid #e6c35a55;padding-bottom:18px}
.head h1{font-family:'Inter Display',Inter,sans-serif;font-weight:800;font-size:46px;letter-spacing:.5px}
.head h1 b{color:#e6c35a;font-weight:800}
.head .k{font-size:19px;color:#9aa3b5;text-align:right;line-height:1.35}
.head .k b{color:#e6c35a;font-weight:600}
.pad{position:absolute;left:%dpx;top:%dpx}
.pad .fb{font:700 20px Inter,sans-serif;text-anchor:middle;fill:#fff}
.pad .sh{font:700 15px Inter,sans-serif;text-anchor:middle;fill:#c9d0dd}
.lines{position:absolute;left:0;top:0;pointer-events:none}
.lines path{fill:none;stroke:#e6c35a;stroke-width:2;opacity:.65}
.lines circle{fill:#e6c35a}
.lab{position:absolute;width:330px;height:76px;display:flex;gap:12px;align-items:flex-start;
 background:#181c24ee;border:1px solid #2f3644;border-radius:12px;padding:10px 12px}
.lab.R{flex-direction:row}
.chips{display:flex;gap:4px;flex-shrink:0;padding-top:2px}
.chip{min-width:34px;height:34px;border-radius:17px;display:inline-flex;align-items:center;justify-content:center;
 font-weight:800;font-size:15px;padding:0 8px;border:1px solid #ffffff22}
.t{font-weight:700;font-size:19px;line-height:1.2}
.s{font-size:14.5px;color:#a7b0c2;line-height:1.3;margin-top:3px}
.foot{position:absolute;left:40px;right:40px;bottom:30px;display:flex;gap:14px}
.combo{flex:1;background:#181c24;border:1px solid #e6c35a44;border-radius:12px;padding:12px 16px;display:flex;
 gap:12px;align-items:center}
.combo .t{font-size:17px}.combo .s{font-size:14px}
.brand{position:absolute;right:40px;bottom:6px;font-size:12px;color:#5f6879}
''' % (W, H, CX, CY)


def page(sheet):
    labs, lines = layout(sheet)
    combos = ''.join(
        f'<div class="combo"><div class="chips">{"".join(chip(k) for k in c["keys"])}</div>'
        f'<div><div class="t">{c["title"]}</div><div class="s">{c.get("sub", "")}</div></div></div>'
        for c in sheet.get('combos', []))
    return f'''<!doctype html><html><head><meta charset="utf-8"><style>{CSS}</style></head><body>
<div class="bg"></div>
<div class="head"><h1>IB3Pad <b>{sheet["name"]}</b></h1><div class="k">{sheet["kicker"]}</div></div>
{controller_svg()}
{lines}
{labs}
<div class="foot">{combos}</div>
<div class="brand">IB3Pad 0.6 controller support for the Infinity Blade III PC port</div>
</body></html>'''


SHEETS = json.load(open(OUT / 'sheets.json'))
for s in SHEETS:
    (OUT / f'{s["file"]}.html').write_text(page(s))
