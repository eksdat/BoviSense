"""Números da seção 4.2 do artigo, a partir do registro serial de bancada.
Fase de cada amostra pelo indicador de preparo gravado pelo firmware (0x008)."""
import sys, math, statistics as st, re
from collections import Counter
L = open(sys.argv[1], "rb").read().decode("utf8", "replace").splitlines()
COLS = "t,modo,estado,Tb,Ub,Tf,Uf,ITU,mv,IG,eI,eG,nivel,duty,flags".split(",")
INT = ("t","estado","mv","eI","eG","nivel","duty","flags")
F_PREP, F_PART, F_FALHA = 0x008, 0x020, 0x001 | 0x002 | 0x004
segs, cur, ult, base = [], [], None, {}
for l in L:
    m = re.search(r"ITU_base=([\d.]+)", l)
    if m: base[len(segs)] = float(m.group(1))
    p = l.strip().split(",")
    if len(p) != 15 or not p[0].isdigit(): continue
    try: r = {k: (int(v) if k in INT else (v if k == "modo" else float(v))) for k, v in zip(COLS, p)}
    except ValueError: continue
    if ult is not None and r["t"] < ult: segs.append(cur); cur = []
    r["sess"] = len(segs); cur.append(r); ult = r["t"]
segs.append(cur)
A = [r for s in segs for r in s]
prep = [r for r in A if r["flags"] & F_PREP]; op = [r for r in A if not r["flags"] & F_PREP]
print(f"amostras {len(A)} | preparo {len(prep)} | operação {len(op)} | sessões {len(segs)} | duração {sum(s[-1]['t']-s[0]['t'] for s in segs)/60:.1f} min")
for k, s in enumerate(segs):
    print(f"  sessão {k}: {len(s)} amostras, {s[0]['t']}–{s[-1]['t']} s, operação {sum(1 for r in s if not r['flags']&F_PREP)}, base {base.get(k)}")
print("operação por modo:", dict(Counter(r["modo"] for r in op)))
print("falhas:", sum(1 for r in A if r["flags"] & F_FALHA), "| partidas:", sum(1 for r in A if r["flags"] & F_PART))

itu = lambda T, U: (1.8*T + 32) - (0.55 - 0.0055*U) * (1.8*T - 26)
ea = [abs(itu(r["Tf"], r["Uf"]) - r["ITU"]) for r in A]
mm = []
for s in segs:
    for i in range(3, len(s)):
        if s[i]["t"] - s[i-3]["t"] > 12: continue
        mm.append(max(abs(sum(x["Tb"] for x in s[i-3:i+1])/4 - s[i]["Tf"]), abs(sum(x["Ub"] for x in s[i-3:i+1])/4 - s[i]["Uf"])))
DUTY = {0: 0, 1: 50, 2: 75, 3: 100}
print(f"\nTABELA 3")
print(f"  ITU x Eq.2              {len(A)}/{len(A)} conformes? {sum(e<0.0125 for e in ea)}  máx {max(ea):.4f}")
print(f"  média móvel             {sum(m<0.006 for m in mm)}/{len(mm)}  máx {max(mm):.4f}")
print(f"  ventilador off preparo  {sum(1 for r in prep if r['nivel']==0 and r['duty']==0)}/{len(prep)}")
print(f"  duty x nível            {sum(1 for r in op if DUTY[r['nivel']]==r['duty'])}/{len(op)}")
alvo = lambda x, lim: sum(1 for l in lim if x >= l)
ok = n = 0
for r in op:
    m = r["modo"]
    if m == "CONTINUO": e = r["nivel"] == 3
    elif m == "LIGA-DESLIGA": e = r["nivel"] == (3 if (r["ITU"] >= 72 or r["IG"] >= 1.5) else 0)
    elif m == "ADAPTATIVO": e = r["eI"] >= alvo(r["ITU"], (72,75,79)) and r["nivel"] == max(r["eI"], r["eG"])
    else: continue
    n += 1; ok += e
print(f"  nível x regra E1/E2/E3  {ok}/{n}")
dm = [r for r in op if r["modo"] == "DEMO"]
b = base.get(dm[0]["sess"]) if dm else None
lim = (b+1, b+2, b+3)
print(f"  nível x regra demo      {sum(1 for r in dm if r['nivel']==alvo(r['ITU'], lim))}/{len(dm)}  (base {b}, ITU máx no modo {max(r['ITU'] for r in dm):.2f}, limiar {lim[0]})")
print(f"  falhas de sensor        0 ocorrências em {len(A)}" if not any(r['flags']&F_FALHA for r in A) else "  HÁ FALHAS")
print("\nFAIXAS")
for k, nm in (("Tb","T bruta"),("Ub","UR"),("Tf","T filtrada"),("ITU","ITU")):
    xs = [r[k] for r in A]; print(f"  {nm:<10} {min(xs):.2f}–{max(xs):.2f}")
mv = [r["mv"] for r in op]; ig = [r["IG"] for r in op if not math.isnan(r["IG"])]
print(f"  MQ-2 em operação {st.mean(mv):.1f} ± {st.pstdev(mv):.1f} mV | IG {min(ig):.2f}–{max(ig):.2f}")
mvall = [r["mv"] for r in A]; print(f"  MQ-2 todas {st.mean(mvall):.1f} ± {st.pstdev(mvall):.1f} mV")
dT = [abs(s[i]["Tb"]-s[i-1]["Tb"]) for s in segs for i in range(1,len(s))]
dTf = [abs(s[i]["Tf"]-s[i-1]["Tf"]) for s in segs for i in range(1,len(s))]
print(f"  variação consecutiva T: bruta {st.mean(dT):.3f} → filtrada {st.mean(dTf):.3f} ({1-st.mean(dTf)/st.mean(dT):.0%})")
ad = [r for r in op if r["modo"]=="ADAPTATIVO"]
print(f"  adaptativa: {len(ad)} amostras, níveis {dict(Counter(r['nivel'] for r in ad))}, ITU {min(r['ITU'] for r in ad):.2f}–{max(r['ITU'] for r in ad):.2f}")
ex = [r for r in op if r["modo"]!="DEMO"]
fr = {"E1": 1.0, "E2": st.mean([1.0 if (r['ITU']>=72 or r['IG']>=1.5) else 0.0 for r in op]), "E3": st.mean([DUTY[alvo(r['ITU'],(72,75,79))]/100 for r in op])}
print("  fração de plena carga sobre as mesmas leituras de operação:", {k: f"{v:.0%}" for k,v in fr.items()}, "| ITU operação", f"{min(r['ITU'] for r in op):.2f}–{max(r['ITU'] for r in op):.2f}")
