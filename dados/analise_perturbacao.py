"""Ensaios de 16 e 17/11/2026 com perturbação térmica e de umidade aplicada à
mão sobre o DHT11 (ferro de solda e ar expirado), estratégia adaptativa.

Uso: python3 analise_perturbacao.py registro_2026-11-16_perturbacao.txt
     python3 analise_perturbacao.py registro_2026-11-17_perturbacao.txt

Confere, amostra a amostra, a regra de estágios do firmware (seção 3.5):
  - subida imediata quando o ITU atinge um limiar (72, 75, 79);
  - descida de um estágio só depois de o ITU ficar abaixo de
    (limiar do estágio atual - 2) por 60 s seguidos.
Usa as mesmas regras de descarte de linhas corrompidas de analise_estrategias.py.
"""
import sys, math
from collections import Counter

COLS = "t,modo,estado,Tb,Ub,Tf,Uf,ITU,mv,IG,eI,eG,nivel,duty,flags".split(",")
INT = {"t", "estado", "mv", "eI", "eG", "nivel", "duty", "flags"}
DUTY = {0: 0, 1: 50, 2: 75, 3: 100}
F_DHT, F_FS, F_PREP, F_NTP = 0x001, 0x002, 0x008, 0x080
LIM, HIST, T_DESC = (72.0, 75.0, 79.0), 2.0, 60
itu_eq = lambda T, U: (1.8 * T + 32) - (0.55 - 0.0055 * U) * (1.8 * T - 26)

cand, desc = [], 0
for l in open(sys.argv[1], "rb").read().decode("utf8", "replace").splitlines():
    p = l.strip().split(",")
    if len(p) != 15 or not p[0].isdigit():
        desc += bool(l.strip()) and not l.startswith(("#", "t,", " ", "ets", "rst", "config", "clk", "mode", "load", "ho ", "entry", "assert", "Backtrace", "ELF", "Rebooting")) ; continue
    try:
        r = {k: (int(v) if k in INT else (v if k == "modo" else float(v))) for k, v in zip(COLS, p)}
    except ValueError:
        desc += 1; continue
    if abs(itu_eq(r["Tf"], r["Uf"]) - r["ITU"]) > 0.0125 or DUTY.get(r["nivel"]) != r["duty"]:
        desc += 1; continue
    if r["t"] > 1_700_000_000 and not r["flags"] & F_NTP:
        desc += 1; continue
    cand.append(r)

# só a sessão após o último reinício, com relógio de calendário
ses = [r for r in cand if r["t"] > 1_700_000_000]
op = [r for r in ses if not r["flags"] & F_PREP]
prep = [r for r in ses if r["flags"] & F_PREP]
print(f"amostras válidas na sessão: {len(ses)} (preparo {len(prep)}, operação {len(op)}) | linhas descartadas no arquivo: {desc}")
print(f"duração da operação: {(op[-1]['t'] - op[0]['t']) / 60:.1f} min | modos: {dict(Counter(r['modo'] for r in op))}")
I = [r["ITU"] for r in ses]; T = [r["Tb"] for r in ses]; U = [r["Ub"] for r in ses]
print(f"faixas na sessão: T bruta {min(T):.1f}–{max(T):.1f} °C | UR bruta {min(U):.0f}–{max(U):.0f} % | ITU {min(I):.2f}–{max(I):.2f}")
print(f"ITU no início da operação {op[0]['ITU']:.2f}, no fim {op[-1]['ITU']:.2f}")
print(f"falhas de leitura do DHT11: {sum(1 for r in ses if r['flags'] & F_DHT)} amostras (operação {sum(1 for r in op if r['flags'] & F_DHT)}); "
      f"modo seguro acionado: {sum(1 for r in ses if r['flags'] & F_FS)}")
G = [r["IG"] for r in op if not math.isnan(r["IG"])]
print(f"IG na operação: {min(G):.2f}–{max(G):.2f} (limiar de alerta 1,5)")

print("\nTROCAS DE NÍVEL NA OPERAÇÃO")
ok = n = 0
abaixo_desde, ult_troca = None, op[0]["t"]
est = op[0]["eI"]
print(f"  t0 {op[0]['t']}: início em nível {op[0]['nivel']} com ITU {op[0]['ITU']:.2f}")
for a, b in zip(op, op[1:]):
    # tempo contínuo abaixo de (limiar do estágio vigente - 2), como o firmware conta
    if a["eI"] > 0 and a["ITU"] < LIM[a["eI"] - 1] - HIST:
        abaixo_desde = abaixo_desde if abaixo_desde is not None else a["t"]
    else:
        abaixo_desde = None
    if b["eI"] != a["eI"]:
        n += 1
        if b["eI"] > a["eI"]:
            alvo = sum(1 for l in LIM if b["ITU"] >= l)
            e = alvo == b["eI"]
            print(f"  {b['t']}: SUBIU {a['eI']}→{b['eI']} com ITU {b['ITU']:.2f} {'ok' if e else 'NÃO CONFERE'}")
        else:
            lim = LIM[a["eI"] - 1] - HIST
            dur = b["t"] - abaixo_desde if abaixo_desde is not None else 0
            e = b["eI"] == a["eI"] - 1 and b["ITU"] < lim and dur >= T_DESC - 3
            print(f"  {b['t']}: DESCEU {a['eI']}→{b['eI']} com ITU {b['ITU']:.2f}; abaixo de {lim:.0f} havia {dur} s {'ok' if e else 'NÃO CONFERE'}")
        ok += e
print(f"  trocas conferidas: {ok}/{n}")

# instantes em que o ITU já estava abaixo do limiar de descida mas o nível ficou, e por quê
print("\nPERMANÊNCIA (ITU abaixo de 77 com nível 3, antes da descida)")
corr, ini = [], None
for r in op:
    if r["eI"] == 3 and r["ITU"] < LIM[2] - HIST:
        ini = ini or r["t"]
    elif ini is not None:
        corr.append((ini, r["t"])); ini = None
for a, b in corr:
    print(f"  {a}–{b}: {b - a} s abaixo de 77, interrompido por leitura ≥ 77" if b - a < T_DESC else f"  {a}–{b}: {b - a} s")
# replica a regra do firmware amostra a amostra, partindo do estagiador zerado
# na entrada em operação, e compara com o estágio gravado
est, ult, ab, abd, div = 0, op[0]["t"] - 2.5, False, 0, 0
for r in op:
    x, ag = r["ITU"], r["t"]
    alvo = sum(1 for l in LIM if x >= l)
    if alvo > est: est, ult, ab = alvo, ag, False
    elif est > 0 and x < LIM[est - 1] - HIST:
        if not ab: ab, abd = True, ag
        if ag - abd >= T_DESC and ag - ult >= T_DESC: est -= 1; ult = ag; ab = False
    else: ab = False
    div += est != r["eI"]
print(f"\nregra replicada em Python × estágio gravado pelo firmware: {len(op) - div}/{len(op)} amostras iguais")
regra = sum(1 for r in op if r["nivel"] == max(r["eI"], r["eG"]))
print(f"\nnível = max(estágio ITU, estágio gás) em {regra}/{len(op)} amostras de operação")
