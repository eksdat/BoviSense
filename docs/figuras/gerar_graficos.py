#!/usr/bin/env python3
"""Figuras de dados do artigo: mapa de níveis por temperatura e umidade e
resposta simulada das estratégias. A simulação não é refeita aqui: a série vem
de simulacao.csv, gravado pelo próprio teste (testes/testes simulacao.csv),
para que a figura e os números do texto saiam da mesma execução.

Cores: faixas de ITU de verde (N0) a vermelho (N3), na mesma escala do
diagrama de estados; estratégia adaptativa em azul, referências em vermelho."""
import csv, pathlib
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

AQUI = pathlib.Path(__file__).resolve().parent
FAIXAS = ["#E3F2E1", "#FFF5C2", "#FFDDB0", "#F8C8CE"]       # N0 a N3
BORDAS = ["#2E7D32", "#B8920A", "#D9730D", "#C62828"]
AZUL, VERMELHO, LARANJA, GRAFITE = "#1565C0", "#C62828", "#E07B00", "#22384F"


def faixas(ax, topo=200):
    for (a, b), c in zip(((0, 72), (72, 75), (75, 79), (79, topo)), FAIXAS):
        ax.axhspan(a, b, color=c, lw=0, zorder=0)
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 8, "axes.linewidth": 0.8,
                     "savefig.bbox": "tight", "savefig.pad_inches": 0.05})


def itu(T, UR):
    return (1.8 * T + 32) - (0.55 - 0.0055 * UR) * (1.8 * T - 26)


def mapa():
    T = np.linspace(18, 38, 400); UR = np.linspace(20, 95, 400)
    TT, UU = np.meshgrid(T, UR); Z = itu(TT, UU)
    fig, ax = plt.subplots(figsize=(5.6, 3.9))
    ax.contourf(TT, UU, Z, levels=[0, 72, 75, 79, 200], colors=FAIXAS)
    cs = ax.contour(TT, UU, Z, levels=[72, 75, 79], colors=BORDAS[1:], linewidths=1.1)
    ax.clabel(cs, fmt="%d", fontsize=7.5, inline_spacing=6)
    for (x, y, r), c in zip(((21.0, 32, "N0"), (26.9, 32, "N1"), (30.0, 32, "N2"), (35.3, 32, "N3")), BORDAS):
        ax.text(x, y, r, ha="center", va="center", fontsize=8.5, fontweight="bold", color=c)
    ax.set_xlabel("Temperatura do ar (°C)"); ax.set_ylabel("Umidade relativa (%)")
    ax.tick_params(width=0.8, length=3)
    fig.savefig(AQUI / "fig4_mapa_itu.png", dpi=300); plt.close(fig)


def simulacao():
    t, y, ld, ad = [], [], [], []
    with open(AQUI / "simulacao.csv", newline="") as f:
        for r in csv.DictReader(f):
            t.append(float(r["minuto"])); y.append(float(r["itu"]))
            ld.append(int(r["duty_liga_desliga"])); ad.append(int(r["duty_adaptativa"]))
    fig, (a, b) = plt.subplots(2, 1, figsize=(6.2, 4.6), sharex=True,
                               gridspec_kw={"height_ratios": [1, 1], "hspace": 0.12})
    faixas(a); a.set_ylim(min(min(y), 70) - 0.6, max(y) + 0.5)
    a.plot(t, y, color=GRAFITE, lw=0.7)
    a.axhline(72, color=VERMELHO, lw=0.9, ls=(0, (5, 3)), label="limiar (72)")
    a.axhline(70, color=LARANJA, lw=0.9, ls=(0, (1, 2)), label="limiar − histerese (70)")
    a.set_ylabel("ITU"); a.legend(loc="upper left", frameon=True, facecolor="white", edgecolor="none", framealpha=0.85, fontsize=7)
    b.step(t, ld, where="post", color=VERMELHO, lw=0.6, label="liga-desliga")
    b.step(t, ad, where="post", color=AZUL, lw=1.6, label="adaptativa")
    b.set_ylabel("Razão cíclica (%)"); b.set_xlabel("Tempo (min)")
    b.set_yticks([0, 50, 75, 100]); b.set_xlim(0, 60)
    b.legend(loc="lower right", frameon=False, fontsize=7)
    for ax in (a, b):
        ax.tick_params(width=0.8, length=3)
    fig.savefig(AQUI / "fig9_simulacao.png", dpi=300); plt.close(fig)


DADOS = AQUI.parent.parent / "dados"
COLS = "t,modo,estado,Tb,Ub,Tf,Uf,ITU,mv,IG,eI,eG,nivel,duty,flags".split(",")


def operacao(nome):
    """Amostras de operação válidas de um registro de bancada (mesmos critérios
    de dados/analise_perturbacao.py)."""
    linhas = (DADOS / nome).read_bytes().decode("utf8", "replace").splitlines()
    out = []
    for l in linhas:
        p = l.strip().split(",")
        if len(p) != 15 or not p[0].isdigit():
            continue
        try:
            r = {k: (v if k == "modo" else float(v)) for k, v in zip(COLS, p)}
        except ValueError:
            continue
        if abs(itu(r["Tf"], r["Uf"]) - r["ITU"]) > 0.0125 or r["t"] < 1.7e9 or not int(r["flags"]) & 0x80:
            continue
        if {0: 0, 1: 50, 2: 75, 3: 100}.get(int(r["nivel"])) != int(r["duty"]) or int(r["flags"]) & 0x008:
            continue
        out.append(r)
    return out


def perturbacao():
    regs = [("registro_2026-11-16_perturbacao.txt", "(a)"), ("registro_2026-11-17_perturbacao.txt", "(b)")]
    fig, ax = plt.subplots(2, 2, figsize=(6.4, 4.4), sharex="col", sharey="row",
                           gridspec_kw={"height_ratios": [1.25, 1], "hspace": 0.12, "wspace": 0.08})
    todos = []
    for j, (nome, rot) in enumerate(regs):
        op = operacao(nome)
        todos += [r["ITU"] for r in op]
        t0 = op[0]["t"]
        t = [(r["t"] - t0) / 60 for r in op]
        a, b = ax[0][j], ax[1][j]
        faixas(a)
        a.plot(t, [r["ITU"] for r in op], color=GRAFITE, lw=0.8, zorder=3)
        for lim, c in zip((72, 75, 79), BORDAS[1:]):
            a.axhline(lim, color=c, lw=0.8, ls=(0, (5, 3)), zorder=2)
        for lim in (73, 77):
            a.axhline(lim, color="#777777", lw=0.7, ls=(0, (1, 2)), zorder=2)
        a.set_title(rot, loc="left", fontsize=8, pad=3)
        b.step(t, [100] * len(t), where="post", color=VERMELHO, lw=1.0, ls=(0, (4, 2)))
        b.step(t, [r["duty"] for r in op], where="post", color=AZUL, lw=1.6)
        b.set_xlabel("Tempo de operação (min)")
        b.set_yticks([0, 50, 75, 100]); b.set_ylim(-5, 108)
        for x in (a, b):
            x.tick_params(width=0.8, length=3)
    ax[0][0].set_ylabel("ITU"); ax[1][0].set_ylabel("Razão cíclica (%)")
    ax[0][0].set_ylim(min(min(todos), 72) - 1.5, max(todos) + 1.5)
    ax[0][1].set_yticks([72, 75, 79, 85, 90])
    from matplotlib.lines import Line2D
    ax[1][1].legend([Line2D([], [], color=AZUL, lw=1.6), Line2D([], [], color=VERMELHO, lw=1.0, ls=(0, (4, 2)))],
                    ["adaptativa", "contínua e liga-desliga"], loc="lower right", frameon=False, fontsize=7)
    ax[0][1].legend([Line2D([], [], color=BORDAS[3], lw=0.8, ls=(0, (5, 3))), Line2D([], [], color="#777777", lw=0.7, ls=(0, (1, 2)))],
                    ["limiares", "limiar − histerese"], loc="upper right", frameon=True, facecolor="white",
                    edgecolor="none", framealpha=0.85, fontsize=7)
    fig.savefig(AQUI / "fig10_perturbacao.png", dpi=300); plt.close(fig)


mapa(); simulacao(); perturbacao()
print("geradas: fig4_mapa_itu.png, fig9_simulacao.png, fig10_perturbacao.png")
