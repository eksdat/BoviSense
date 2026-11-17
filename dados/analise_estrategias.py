"""Ensaio de 16/11/2026: uma sessão por estratégia (contínua, liga-desliga,
adaptativa e demonstração), em sequência, com o protótipo na bancada.

Uso: python3 analise_estrategias.py registro_2026-11-16_estrategias.txt

Linhas que o monitor serial corrompeu (caractere perdido ou trocado) são
descartadas por critérios objetivos e contadas, nunca corrigidas à mão:
  - número de campos diferente de 15 ou campo que não converte;
  - carimbo de tempo isolado: difere em mais de 10 s da linha anterior e da
    seguinte, que concordam entre si (um dígito perdido); intervalos entre
    sessões, em que as linhas seguintes continuam do novo valor, são mantidos;
  - ITU gravado diferente do ITU recalculado de T e UR filtradas (> 0,0125);
  - razão cíclica que não corresponde ao nível, ou nível fora de 0..3;
  - indicador de hora NTP (0x080) ausente com carimbo de tempo de calendário;
  - IG fora de 0..10.
"""
import sys, math, re, statistics as st
from collections import Counter

COLS = "t,modo,estado,Tb,Ub,Tf,Uf,ITU,mv,IG,eI,eG,nivel,duty,flags".split(",")
INT = {"t", "estado", "mv", "eI", "eG", "nivel", "duty", "flags"}
DUTY = {0: 0, 1: 50, 2: 75, 3: 100}
F_PREP, F_FALHA, F_PART, F_NTP, F_SIM = 0x008, 0x001 | 0x002 | 0x004, 0x020, 0x080, 0x100
MODOS = ("CONTINUO", "LIGA-DESLIGA", "ADAPTATIVO", "DEMO")
itu_eq = lambda T, U: (1.8 * T + 32) - (0.55 - 0.0055 * U) * (1.8 * T - 26)
alvo = lambda x, lim: sum(1 for l in lim if x >= l)

linhas = open(sys.argv[1], "rb").read().decode("utf8", "replace").splitlines()
cand, motivos = [], Counter()
for l in linhas:
    if not l or l.startswith("#") or l.startswith("t,"):
        continue
    p = l.strip().split(",")
    if len(p) != 15:
        motivos["campos"] += 1; continue
    try:
        r = {k: (int(v) if k in INT else (v if k == "modo" else float(v))) for k, v in zip(COLS, p)}
    except ValueError:
        motivos["conversão"] += 1; continue
    if r["modo"] not in MODOS: motivos["modo"] += 1; continue
    if abs(itu_eq(r["Tf"], r["Uf"]) - r["ITU"]) > 0.0125: motivos["ITU"] += 1; continue
    if r["nivel"] not in DUTY or DUTY[r["nivel"]] != r["duty"]: motivos["nível/razão"] += 1; continue
    if r["t"] > 1_700_000_000 and not r["flags"] & F_NTP: motivos["indicador"] += 1; continue
    if not math.isnan(r["IG"]) and not 0 <= r["IG"] <= 10: motivos["IG"] += 1; continue
    cand.append(r)
validas = []
for i, r in enumerate(cand):
    if 0 < i < len(cand) - 1:
        a_, c_ = cand[i - 1]["t"], cand[i + 1]["t"]
        if abs(r["t"] - a_) > 10 and abs(c_ - r["t"]) > 10 and 0 < c_ - a_ <= 20:
            motivos["tempo"] += 1; continue
    validas.append(r)

op = [r for r in validas if not r["flags"] & F_PREP and not r["flags"] & F_SIM]
sim = [r for r in validas if r["flags"] & F_SIM]
desc = sum(motivos.values())
print(f"linhas de dados: {len(validas) + desc} | válidas {len(validas)} | descartadas {desc} {dict(motivos)}")
print(f"falhas de sensor: {sum(1 for r in validas if r['flags'] & F_FALHA)} | simulação: {len(sim)} amostras")

print("\nPOR SESSÃO (estratégia ativa)")
print(f"{'modo':<13}{'n':>4}{'min':>6}  {'ITU médio ± dp':<16}{'faixa ITU':<14}{'T °C':<12}{'UR %':<8}{'IG':<12}{'níveis':<12}{'razão':>6}{'trocas':>7}")
for m in MODOS:
    s = [r for r in op if r["modo"] == m]
    if not s: continue
    dur = (s[-1]["t"] - s[0]["t"]) / 60
    I = [r["ITU"] for r in s]; T = [r["Tf"] for r in s]; U = [r["Uf"] for r in s]
    G = [r["IG"] for r in s if not math.isnan(r["IG"])]
    trocas = sum(1 for a, b in zip(s, s[1:]) if a["duty"] != b["duty"])
    print(f"{m:<13}{len(s):>4}{dur:>6.1f}  {st.mean(I):.2f} ± {st.pstdev(I):.2f}    {min(I):.2f}–{max(I):.2f}  "
          f"{min(T):.1f}–{max(T):.1f}  {min(U):.0f}–{max(U):.0f}   {min(G):.2f}–{max(G):.2f}  "
          f"{str(dict(Counter(r['nivel'] for r in s))):<12}{st.mean(r['duty'] for r in s):>5.0f}%{trocas:>7}")

print("\nCONFORMIDADE DO NÍVEL COM A REGRA DA ESTRATÉGIA ATIVA")
ok = n = 0
for r in op:
    m = r["modo"]
    if m == "CONTINUO": e = r["nivel"] == 3
    elif m == "LIGA-DESLIGA": e = r["nivel"] == (3 if (r["ITU"] >= 72 or r["IG"] >= 1.5) else 0)
    elif m == "ADAPTATIVO": e = r["eI"] == alvo(r["ITU"], (72, 75, 79)) and r["nivel"] == max(r["eI"], r["eG"])
    else: continue
    n += 1; ok += e
print(f"  contínua, liga-desliga e adaptativa: {ok}/{n}")
dm = [r for r in op if r["modo"] == "DEMO"]
if dm:
    print(f"  demonstração: nível 0 em {sum(1 for r in dm if r['nivel'] == 0)}/{len(dm)}; ITU máx {max(r['ITU'] for r in dm):.2f}. "
          "A linha ITU_base desta sessão não está no trecho; o resultado só é compatível com limiar de demonstração acima desse máximo.")
print(f"  simulação: nível 3 e 100 % em {sum(1 for r in sim if r['nivel'] == 3 and r['duty'] == 100)}/{len(sim)}")

print("\nAS TRÊS REGRAS SOBRE TODAS AS LEITURAS DE OPERAÇÃO (cálculo pareado)")
ex = [r for r in op]
e1 = [100] * len(ex)
e2 = [100 if (r["ITU"] >= 72 or (not math.isnan(r["IG"]) and r["IG"] >= 1.5)) else 0 for r in ex]
e3 = [DUTY[alvo(r["ITU"], (72, 75, 79))] for r in ex]
for nome, v in (("E1 contínua", e1), ("E2 liga-desliga", e2), ("E3 adaptativa", e3)):
    print(f"  {nome:<16} razão média {st.mean(v):5.1f} % | trocas {sum(1 for a, b in zip(v, v[1:]) if a != b)}")
I = [r["ITU"] for r in ex]
print(f"  leituras: {len(ex)} | ITU {min(I):.2f}–{max(I):.2f} (média {st.mean(I):.2f}) | "
      f"UR {min(r['Uf'] for r in ex):.0f}–{max(r['Uf'] for r in ex):.0f} % | T {min(r['Tf'] for r in ex):.2f}–{max(r['Tf'] for r in ex):.2f} °C")
