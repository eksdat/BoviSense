#!/usr/bin/env python3
"""Figuras desenhadas do artigo: blocos, ligações, esquema do estágio de
potência, fluxograma do controle, diagrama de estados e fluxograma da rede.

Regra de desenho: nenhuma frase dentro da imagem. Entram só os rótulos que um
esquema precisa (nome do bloco, pino, valor do componente, condição de uma
transição). Toda explicação fica no texto do artigo e na legenda. Símbolos
elétricos convencionais, com cor de engenharia: blocos preenchidos por função
(entrada, processamento, saída), fios pela grandeza (3,3 V, terra, sinal
analógico, dado digital, PWM, 9 V) e níveis de ventilação na mesma escala de
verde a vermelho em todas as figuras.

Todo valor aqui tem de bater com o firmware (bovisense.cpp): pinos, resistores,
limiares, níveis e tempos.

Coordenadas em 0..100 (ou até xmax) com aspecto livre: a área de dados ocupa a
figura inteira e o texto em pontos não depende da proporção. Círculos são
desenhados como elipses compensadas pela proporção da figura.
"""
import pathlib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, Polygon, Ellipse, FancyBboxPatch, FancyArrowPatch

SAIDA = pathlib.Path(__file__).resolve().parent
DPI = 300
PRETO, CINZA = "#1A1A1A", "#444444"
LW = 1.0
# blocos por função: preenchimento, borda
ENT = ("#E3EEF9", "#2B5C8A")      # entradas
PROC = ("#FFF6E0", "#B7862B")     # ESP32
N1 = ("#FDE8D3", "#C9692C")       # núcleo de controle
N0 = ("#E4F2E6", "#2E7D32")       # núcleo de rede
SAI = ("#FBE4E7", "#B03A48")      # saídas
NEU = ("#EEEEEE", "#555555")      # infraestrutura
TG = ("#DCEBFA", "#2B7BB9")       # Telegram
# fios pela grandeza
C3V3, CGND, CAO, CDATA, CBUZ, CPWM, C9V, CMOT = ("#C62828", "#1A1A1A", "#E07B00", "#2E7D32",
                                                 "#7B3F9E", "#1565C0", "#C62828", "#E07B00")
# níveis 0 a 3: preenchimento e borda (mesma escala do mapa de ITU)
NIV_F = ("#E3F2E1", "#FFF5C2", "#FFDDB0", "#F8C8CE")
NIV_B = ("#2E7D32", "#B8920A", "#D9730D", "#C62828")
# fluxograma
F_TERM, F_PROC, F_DEC, F_SEG, F_FIM, BORDA = "#E0E0E0", "#E3EEF9", "#FFF4CC", "#F9D3D8", "#E4F2E6", "#333333"

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 8,
                     "mathtext.fontset": "dejavusans",
                     "savefig.bbox": "tight", "savefig.pad_inches": 0.05})


def nova(w, h, xmax=100, ymin=0, ymax=100):
    fig, ax = plt.subplots(figsize=(w, h))
    ax.set_xlim(0, xmax); ax.set_ylim(ymin, ymax)
    ax.set_aspect("auto"); ax.axis("off")
    ax.k = (w / xmax) / (h / (ymax - ymin))    # quantas unidades de y valem uma de x
    return fig, ax


def ret(ax, x, y, w, h, lw=LW, fc=None, ec=PRETO):
    ax.add_patch(Rectangle((x, y), w, h, fill=fc is not None, fc=fc or "none", ec=ec, lw=lw, zorder=3))


def circ(ax, x, y, r, lw=LW, fc=None, ec=PRETO):
    ax.add_patch(Ellipse((x, y), 2 * r, 2 * r * ax.k, fill=fc is not None, fc=fc or "none", ec=ec, lw=lw, zorder=3))


def txt(ax, x, y, s, tam=7.4, neg=False, ha="center", va="center", cor=PRETO):
    ax.text(x, y, s, ha=ha, va=va, fontsize=tam, color=cor, zorder=5,
            fontweight="bold" if neg else "normal", linespacing=1.4)


def bloco(ax, x, y, w, h, s, tam=7.6, neg=True, lw=LW, cores=None, cor_txt=PRETO):
    fc, ec = cores if cores else (None, PRETO)
    ret(ax, x, y, w, h, lw, fc, ec); txt(ax, x + w / 2, y + h / 2, s, tam, neg, cor=cor_txt)


def fio(ax, pts, lw=LW, cor=PRETO):
    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=cor, lw=lw,
            solid_capstyle="round", solid_joinstyle="miter", zorder=2)


def no(ax, x, y, cor=PRETO):
    ax.plot(x, y, marker="o", ms=3.0, color=cor, zorder=6)


def borne(ax, x, y):
    ax.plot(x, y, marker="o", ms=4.2, mfc="white", mec=PRETO, mew=LW, zorder=6)


def seta(ax, x1, y1, x2, y2, lw=0.9, cor=PRETO):
    ax.annotate("", xy=(x2, y2), xytext=(x1, y1), zorder=2,
                arrowprops=dict(arrowstyle="-|>", color=cor, lw=lw,
                                shrinkA=0, shrinkB=0, mutation_scale=8))


def terra(ax, x, y, larg=5.0):
    """Símbolo de terra: haste e três traços decrescentes, abaixo de (x, y)."""
    d = 1.5 * ax.k
    fio(ax, [(x, y), (x, y - d)])
    for i, f in enumerate((1.0, 0.62, 0.26)):
        fio(ax, [(x - larg * f / 2, y - d * (1 + i * 0.75)), (x + larg * f / 2, y - d * (1 + i * 0.75))])


def bateria(ax, x, y_cima, y_baixo, ym):
    """Bateria vertical: placa longa (positivo) em cima, curta e grossa embaixo."""
    d = 1.5 * ax.k
    fio(ax, [(x, y_cima), (x, ym + d / 2)], cor=C9V)
    fio(ax, [(x - 3.6, ym + d / 2), (x + 3.6, ym + d / 2)])
    fio(ax, [(x - 2.0, ym - d / 2), (x + 2.0, ym - d / 2)], lw=2.2)
    fio(ax, [(x, ym - d / 2), (x, y_baixo)])
    txt(ax, x - 5.2, ym + d / 2 + 1.2 * ax.k, "+", 7.5)


def diodo_v(ax, x, y0, y1, ym):
    """Diodo vertical com catodo para cima, entre y0 (anodo) e y1 (catodo)."""
    a = 2.4; h = 2.6 * ax.k
    fio(ax, [(x, y0), (x, ym - h / 2)], cor=CMOT); fio(ax, [(x, ym + h / 2), (x, y1)], cor=C9V)
    ax.add_patch(Polygon([[x - a, ym - h / 2], [x + a, ym - h / 2], [x, ym + h / 2]],
                         closed=True, fill=True, fc="#9E9E9E", ec=PRETO, lw=LW, zorder=4))
    fio(ax, [(x - a, ym + h / 2), (x + a, ym + h / 2)], lw=1.8)


# ---------------------------------------------------------------- Figura 1
def fig_blocos():
    fig, ax = nova(7.4, 3.5, xmax=120)
    for nome, y, rot in (("DHT11", 72, "1-Wire"), ("MQ-2", 43, "ADC"), ("Botão", 14, "GPIO")):
        bloco(ax, 1, y, 19, 15, nome, cores=ENT)
        seta(ax, 20, y + 7.5, 33, y + 7.5)
        txt(ax, 26.5, y + 10.6, rot, 6.4, cor=CINZA)

    ret(ax, 33, 6, 30, 89, lw=1.6, fc=PROC[0], ec=PROC[1])
    txt(ax, 48, 89.5, "ESP32", 9, neg=True)
    bloco(ax, 37, 51, 22, 30, "Núcleo 1\ncontrole", 7.4, cores=N1)
    bloco(ax, 37, 12, 22, 30, "Núcleo 0\nrede", 7.4, cores=N0)
    ax.annotate("", xy=(48, 51), xytext=(48, 42), zorder=2,
                arrowprops=dict(arrowstyle="<|-|>", color=PRETO, lw=0.9, mutation_scale=7))
    txt(ax, 49.5, 46.5, "fila", 6.2, ha="left", cor=CINZA)

    bloco(ax, 74, 72, 17, 15, "TIP122", cores=SAI)
    seta(ax, 59, 79.5, 74, 79.5); txt(ax, 66.5, 82.6, "PWM", 6.4, cor=CINZA)
    bloco(ax, 101, 72, 18, 15, "Cooler", cores=SAI)
    seta(ax, 91, 79.5, 101, 79.5); txt(ax, 96, 82.6, "9 V", 6.4, cor=CINZA)
    bloco(ax, 74, 51, 17, 15, "Buzzer", cores=SAI)
    seta(ax, 59, 58.5, 74, 58.5); txt(ax, 66.5, 61.6, "PWM", 6.4, cor=CINZA)
    bloco(ax, 74, 30, 17, 15, "Painel\nweb", cores=SAI)
    seta(ax, 59, 37.5, 74, 37.5); txt(ax, 66.5, 40.6, "HTTP", 6.4, cor=CINZA)
    bloco(ax, 74, 9, 17, 15, "Telegram", cores=SAI)
    seta(ax, 59, 16.5, 74, 16.5); txt(ax, 66.5, 19.6, "HTTPS", 6.4, cor=CINZA)
    fig.savefig(SAIDA / "fig1_diagrama_blocos.png", dpi=DPI); plt.close(fig)


# ---------------------------------------------------------------- Figura 2
def fig_ligacoes():
    fig, ax = nova(7.4, 4.4)
    ret(ax, 46, 12, 18, 80, lw=1.5, fc="#22384F", ec="#22384F")
    txt(ax, 55, 86.5, "ESP32", 8.6, neg=True, cor="white")
    cor_pino = {"3V3": C3V3, "GND": CGND, "D34": CAO, "D4": CDATA, "D25": CBUZ, "D26": CPWM}
    for nome, y in (("3V3", 78), ("GND", 62), ("D34", 52), ("D4", 40), ("D25", 30)):
        fio(ax, [(43, y), (46, y)], cor=cor_pino[nome]); txt(ax, 47.2, y, nome, 7, ha="left", cor="white")
    for nome, y in (("D26", 52), ("GND", 30)):
        fio(ax, [(64, y), (67, y)], cor=cor_pino[nome]); txt(ax, 62.8, y, nome, 7, ha="right", cor="white")

    mods = (("MQ-2", 62, 26, (("VCC", 82), ("GND", 74), ("AO", 68)), ("#FDE8D3", CAO)),
            ("DHT11", 34, 22, (("VCC", 50), ("GND", 45), ("DATA", 40)), ("#E4F2E6", CDATA)),
            ("Buzzer", 12, 16, (("+", 24), ("−", 18)), ("#EFE3F6", CBUZ)))
    for nome, y, h, pinos, (fc, ec) in mods:
        ret(ax, 2, y, 21, h, fc=fc, ec=ec)
        txt(ax, 8.5, y + h / 2, nome, 7.8, neg=True)
        for pn, py in pinos:
            txt(ax, 21.8, py, pn, 6.6, ha="right")

    X3, XG, XS = 33, 37, 40.5        # barra de 3V3, barra de terra, coluna de sinal
    fio(ax, [(23, 82), (X3, 82), (X3, 50), (23, 50)], cor=C3V3)        # barra de 3V3
    fio(ax, [(X3, 78), (43, 78)], cor=C3V3); no(ax, X3, 78, C3V3)
    txt(ax, X3, 86.5, "3V3", 6.6, cor=C3V3)
    fio(ax, [(23, 74), (XG, 74), (XG, 18), (23, 18)])        # barra de terra
    fio(ax, [(23, 45), (XG, 45)]); no(ax, XG, 45)
    fio(ax, [(XG, 62), (43, 62)]); no(ax, XG, 62)
    no(ax, XG, 18); terra(ax, XG, 18, larg=4.2)
    fio(ax, [(23, 68), (XS, 68), (XS, 52), (43, 52)], cor=CAO)        # AO -> D34
    fio(ax, [(23, 40), (43, 40)], cor=CDATA)                          # DATA -> D4
    fio(ax, [(23, 24), (XS, 24), (XS, 30), (43, 30)], cor=CBUZ)       # buzzer -> D25

    # potência: +9 V -> motor -> TIP122 -> terra; negativo da bateria no terra
    bloco(ax, 74, 45, 13, 14, "TIP122", 7.4, cores=("#E0E0E0", "#424242"))
    fio(ax, [(67, 52), (74, 52)], cor=CPWM)
    rm = 3.8
    circ(ax, 80.5, 72, rm, fc="#F2F2F2"); txt(ax, 80.5, 72, "M", 8, neg=True)
    fio(ax, [(80.5, 59), (80.5, 72 - rm * ax.k)], cor=CMOT)
    fio(ax, [(80.5, 72 + rm * ax.k), (80.5, 88), (95, 88)], cor=C9V)
    bateria(ax, 95, 88, 30, 60); txt(ax, 99.6, 60, "9 V", 7, ha="left")
    fio(ax, [(67, 30), (95, 30)])
    fio(ax, [(80.5, 45), (80.5, 30)]); no(ax, 80.5, 30)
    terra(ax, 88, 30, larg=4.2); no(ax, 88, 30)
    ax.set_xlim(0, 104)
    fig.savefig(SAIDA / "fig2_esquema_ligacoes.png", dpi=DPI); plt.close(fig)


# ---------------------------------------------------------------- Figura 3
def fig_potencia():
    fig, ax = nova(6.6, 4.0, xmax=104)
    YT, YG = 92, 14
    # entrada e resistor de base
    borne(ax, 6, 50); txt(ax, 6, 56.5, "D26", 7.4, cor=CPWM)
    fio(ax, [(6.9, 50), (17, 50)], cor=CPWM)
    ret(ax, 17, 46.6, 13, 6.8, fc="#F6EBD0"); txt(ax, 23.5, 59, r"$R_B$ = 1 kΩ", 7.4)
    fio(ax, [(30, 50), (52, 50)], cor=CPWM)
    # resistor da base ao terra
    no(ax, 39, 50, CPWM); fio(ax, [(39, 50), (39, 40)], cor=CPWM)
    ret(ax, 36.6, 25, 4.8, 15, fc="#F6EBD0"); txt(ax, 34.6, 32.5, "10 kΩ", 7.4, ha="right")
    fio(ax, [(39, 25), (39, YG)]); no(ax, 39, YG)
    # transistor NPN (par Darlington representado pelo símbolo simples)
    bx, by = 52, 50
    circ(ax, bx + 4.5, by, 10.5, fc="#EEF2F6")
    fio(ax, [(bx, by - 5 * ax.k), (bx, by + 5 * ax.k)], lw=2.2)
    fio(ax, [(bx, by + 2.3 * ax.k), (bx + 9, by + 7.0 * ax.k)])
    fio(ax, [(bx, by - 2.3 * ax.k), (bx + 9, by - 7.0 * ax.k)])
    ex, ey = bx + 9, by - 7.0 * ax.k
    ax.annotate("", xy=(ex - 1.4, ey + 0.73 * ax.k), xytext=(bx + 3.2, by - 3.97 * ax.k), zorder=4,
                arrowprops=dict(arrowstyle="-|>", color=PRETO, lw=0, mutation_scale=11,
                                shrinkA=0, shrinkB=0))
    txt(ax, bx - 2.2, by + 4.2 * ax.k, "B", 6.8); txt(ax, bx + 11.6, 67.4, "C", 6.8)
    txt(ax, bx + 11.6, 32.6, "E", 6.8)
    txt(ax, bx + 17, by, "TIP122", 7.6, neg=True, ha="left")
    fio(ax, [(ex, ey), (ex, YG)]); no(ax, ex, YG)
    # coletor, motor e diodo de roda livre
    cx, cy = bx + 9, by + 7.0 * ax.k
    yn = 70
    fio(ax, [(cx, cy), (cx, yn)], cor=CMOT); no(ax, cx, yn, CMOT)
    r = 4.4
    circ(ax, cx, 81, r, fc="#F2F2F2"); txt(ax, cx, 81, "M", 8.4, neg=True)
    fio(ax, [(cx, yn), (cx, 81 - r * ax.k)], cor=CMOT); fio(ax, [(cx, 81 + r * ax.k), (cx, YT)], cor=C9V); no(ax, cx, YT, C9V)
    fio(ax, [(cx, yn), (76, yn)], cor=CMOT)
    diodo_v(ax, 76, yn, YT, 81); no(ax, 76, YT, C9V)
    txt(ax, 80, 81, "1N4007", 7.4, ha="left")
    # alimentação do motor
    fio(ax, [(cx, YT), (95, YT)], cor=C9V)
    bateria(ax, 95, YT, YG, 53); txt(ax, 100, 53, "9 V", 7.4, ha="left")
    # terra comum
    borne(ax, 6, YG); txt(ax, 6, YG + 6.5, "GND", 7.4)
    fio(ax, [(6.9, YG), (95, YG)])
    terra(ax, 72, YG)
    fig.savefig(SAIDA / "fig3_estagio_potencia.png", dpi=DPI); plt.close(fig)


# ------------------------------------------------------- peças de fluxograma
def processo(ax, cx, y, w, h, s, tam=7.0, fc=F_PROC):
    ret(ax, cx - w / 2, y, w, h, fc=fc, ec=BORDA); txt(ax, cx, y + h / 2, s, tam)


def terminal(ax, cx, y, w, h, s, tam=7.0):
    # mutation_aspect = k faz o arredondamento sair circular com aspecto livre
    ax.add_patch(FancyBboxPatch((cx - w / 2, y), w, h,
                                boxstyle=f"round,pad=0,rounding_size={h / ax.k * 0.49}",
                                fill=True, fc=F_TERM, ec=BORDA, lw=LW, zorder=3, mutation_aspect=ax.k))
    txt(ax, cx, y + h / 2, s, tam)


def decisao(ax, cx, y, w, h, s, tam=7.0):
    cy = y + h / 2
    ax.add_patch(Polygon([[cx - w / 2, cy], [cx, y + h], [cx + w / 2, cy], [cx, y]],
                         closed=True, fill=True, fc=F_DEC, ec=BORDA, lw=LW, zorder=3))
    txt(ax, cx, cy, s, tam)


# ---------------------------------------------------------------- Figura 5
def fig_fluxo_controle():
    fig, ax = nova(5.6, 8.2, ymin=-6, ymax=100)
    C = 50
    terminal(ax, C, 95, 30, 4.5, "início")
    seta(ax, C, 95, C, 92)
    decisao(ax, C, 81, 40, 11, "reinício\npor falha?")
    processo(ax, 87, 83.5, 24, 6, "restaura estratégia\ne linha de base", 6.4, fc=F_FIM)
    seta(ax, 70, 86.5, 75, 86.5); txt(ax, 72.5, 88.3, "sim", 6.2, cor=CINZA)
    seta(ax, C, 81, C, 77.5); txt(ax, C + 1.6, 79.3, "não", 6.2, ha="left", cor=CINZA)
    processo(ax, C, 71.5, 50, 6, "autoteste e aquecimento (180 s)")
    seta(ax, C, 71.5, C, 68.5)
    processo(ax, C, 62.5, 50, 6, "linha de base do MQ-2 (60 s)")
    seta(ax, C, 62.5, C, 59.5)
    processo(ax, C, 53.5, 50, 6, "lê DHT11 e MQ-2")
    # a retomada entra direto na leitura
    fio(ax, [(87, 83.5), (87, 56.5)]); seta(ax, 87, 56.5, 75, 56.5)
    seta(ax, C, 53.5, C, 50.5)
    decisao(ax, C, 40.5, 40, 10, "3 falhas\ndo DHT11?")
    processo(ax, 87, 42.5, 20, 6, "modo seguro\n100 %", 6.6, fc=F_SEG)
    seta(ax, 70, 45.5, 77, 45.5); txt(ax, 73.5, 47.3, "sim", 6.2, cor=CINZA)
    seta(ax, C, 40.5, C, 37.5); txt(ax, C + 1.6, 39.1, "não", 6.2, ha="left", cor=CINZA)
    processo(ax, C, 31.5, 50, 6, "calcula ITU e IG")
    seta(ax, C, 31.5, C, 28.5)
    decisao(ax, C, 18.5, 40, 10, "estratégia\nativa?")
    processo(ax, 16, 7.5, 21, 7, "contínua ou\nliga-desliga", 6.6)
    processo(ax, 84, 7.5, 21, 7, "adaptativa\ncom histerese", 6.6)
    fio(ax, [(30, 23.5), (16, 23.5)]); seta(ax, 16, 23.5, 16, 14.5)
    fio(ax, [(70, 23.5), (84, 23.5)]); seta(ax, 84, 23.5, 84, 14.5)
    processo(ax, C, 7.5, 38, 7, "nível = máx(ITU, gás)\naplica PWM", 6.8)
    seta(ax, 26.5, 11, 31, 11); seta(ax, 73.5, 11, 69, 11)
    seta(ax, C, 7.5, C, 4.5)
    processo(ax, C, -1.5, 38, 6, "registra e alerta", fc=F_FIM)
    # o modo seguro segue direto para o registro
    fio(ax, [(87, 42.5), (87, 36), (98.5, 36), (98.5, 1.5)]); seta(ax, 98.5, 1.5, 69, 1.5)
    # laço de amostragem
    fio(ax, [(31, 1.5), (1.5, 1.5), (1.5, 56.5)]); seta(ax, 1.5, 56.5, 25, 56.5)
    txt(ax, 3.2, 30, "2,5 s", 6.4, ha="left", cor=CINZA)
    fig.savefig(SAIDA / "fig5_fluxograma.png", dpi=DPI); plt.close(fig)


# ------------------------------------------------------ diagrama de estados
def fig_estados():
    fig, ax = nova(7.2, 3.4)
    xs = (10, 36.5, 63.5, 90); y = 34; r = 6.0; rv = r * ax.k
    for k, x in enumerate(xs):
        circ(ax, x, y, r, lw=1.6, fc=NIV_F[k], ec=NIV_B[k])
        txt(ax, x, y + 3.2, f"N{k}", 8.4, neg=True)
        txt(ax, x, y - 4.6, ("0 %", "50 %", "75 %", "100 %")[k], 6.8, cor=CINZA)

    d = 4.2                                   # afastamento das setas retas do eixo
    dx = r * (1 - (d / rv) ** 2) ** 0.5 + 0.6
    for a, lim in ((0, 72), (1, 75), (2, 79)):          # subida, imediata
        seta(ax, xs[a] + dx, y + d, xs[a + 1] - dx, y + d, cor="#B03A48")
        txt(ax, (xs[a] + xs[a + 1]) / 2, y + d + 4.6, f"ITU ≥ {lim}", 6.8)
    for a, lim in ((1, 70), (2, 73), (3, 77)):          # descida, um nível por vez
        seta(ax, xs[a] - dx, y - d, xs[a - 1] + dx, y - d, cor=CPWM)
        txt(ax, (xs[a] + xs[a - 1]) / 2, y - d - 7.4, f"ITU < {lim}\npor 60 s", 6.8)

    def salto(a, b, rad, rot, yrot):                    # subida que pula níveis
        ax.add_patch(FancyArrowPatch((xs[a] + 1.5, y + rv), (xs[b] - 1.5, y + rv),
                                     connectionstyle=f"arc3,rad={rad}", arrowstyle="-|>",
                                     mutation_scale=8, color="#B03A48", lw=0.9, ls=(0, (4, 2)),
                                     shrinkA=1, shrinkB=1, zorder=2))
        txt(ax, (xs[a] + xs[b]) / 2, yrot, rot, 6.6, cor=CINZA)
    salto(0, 2, -0.42, "ITU ≥ 75", 73.5)
    salto(1, 3, -0.42, "ITU ≥ 79", 73.5)
    salto(0, 3, -0.46, "ITU ≥ 79", 97)
    fig.savefig(SAIDA / "fig_estados.png", dpi=DPI); plt.close(fig)


# ------------------------------------------------- fluxograma da tarefa de rede
def fig_fluxo_rede():
    fig, ax = nova(5.6, 7.8, xmax=106, ymin=-5, ymax=100)
    C = 46
    terminal(ax, C, 94, 34, 5, "tarefa de rede")
    seta(ax, C, 94, C, 90.5)
    processo(ax, C, 84.5, 48, 6, "copia as credenciais (mutex)")
    seta(ax, C, 84.5, C, 81)
    decisao(ax, C, 70, 46, 11, "Wi-Fi\nconectado?")
    processo(ax, 88, 72.5, 22, 6, "reconecta\n(30 s)", 6.6, fc=F_SEG)
    seta(ax, 69, 75.5, 77, 75.5); txt(ax, 73, 77.3, "não", 6.2, cor=CINZA)
    seta(ax, C, 70, C, 66.5); txt(ax, C + 1.6, 68.3, "sim", 6.2, ha="left", cor=CINZA)
    processo(ax, C, 60.5, 48, 6, "registra comandos e saúda (1 vez)")
    seta(ax, C, 60.5, C, 57)
    decisao(ax, C, 46, 46, 11, "mensagem\nna fila?")
    processo(ax, 88, 48.5, 22, 6, "envia a\nmensagem", 6.6, fc=F_FIM)
    seta(ax, 69, 51.5, 77, 51.5); txt(ax, 73, 53.3, "sim", 6.2, cor=CINZA)
    seta(ax, C, 46, C, 42.5); txt(ax, C + 1.6, 44.3, "não", 6.2, ha="left", cor=CINZA)
    decisao(ax, C, 31.5, 46, 11, "4 s desde a\núltima consulta?")
    seta(ax, C, 31.5, C, 28); txt(ax, C + 1.6, 29.8, "sim", 6.2, ha="left", cor=CINZA)
    processo(ax, C, 22, 48, 6, "getUpdates")
    seta(ax, C, 22, C, 18.5)
    decisao(ax, C, 7.5, 46, 11, "chat\nautorizado?")
    processo(ax, 88, 10, 22, 6, "executa o\ncomando", 6.6, fc=F_FIM)
    seta(ax, 69, 13, 77, 13); txt(ax, 73, 14.8, "sim", 6.2, cor=CINZA)
    processo(ax, C, -3.5, 48, 6, "vincula o chat ou informa o chat_id", 6.6)
    seta(ax, C, 7.5, C, 2.5); txt(ax, C + 1.6, 5, "não", 6.2, ha="left", cor=CINZA)
    # retornos ao início do laço pela direita
    for yb in (72.5, 48.5, 10):
        fio(ax, [(99, yb + 3), (103, yb + 3)])
    fio(ax, [(103, -0.5), (103, 87.5)]); fio(ax, [(70, -0.5), (103, -0.5)])
    seta(ax, 103, 87.5, 70, 87.5)
    # "não" da consulta: volta pela esquerda
    fio(ax, [(23, 37), (4, 37), (4, 87.5)]); seta(ax, 4, 87.5, 22, 87.5)
    txt(ax, 13, 38.8, "não", 6.2, cor=CINZA)
    fig.savefig(SAIDA / "fig_fluxo_rede.png", dpi=DPI); plt.close(fig)


# ------------------------------------------------ arquitetura de comunicação
def fig_comunicacao():
    fig, ax = nova(7.4, 3.3, xmax=122)

    def dupla(x1, y1, x2, y2, rot=None, dx=0, dy=3.4):
        ax.annotate("", xy=(x2, y2), xytext=(x1, y1), zorder=2,
                    arrowprops=dict(arrowstyle="<|-|>", color=PRETO, lw=0.9,
                                    shrinkA=0, shrinkB=0, mutation_scale=8))
        if rot:
            txt(ax, (x1 + x2) / 2 + dx, (y1 + y2) / 2 + dy, rot, 6.4, cor=CINZA)

    bloco(ax, 1, 34, 22, 32, "ESP32", 8.6, lw=1.6, cores=PROC)
    bloco(ax, 42, 68, 24, 22, "Celular\npainel web", cores=ENT)
    bloco(ax, 42, 10, 24, 22, "Roteador", cores=NEU)
    bloco(ax, 76, 10, 18, 22, "Internet", cores=NEU)
    bloco(ax, 102, 56, 19, 18, "Telegram", 7.2, cores=TG)
    bloco(ax, 102, 12, 19, 18, "NTP", 7.2, cores=N0)
    dupla(23, 58, 42, 79, "Wi-Fi AP\nHTTP", dx=-7, dy=6)
    dupla(23, 42, 42, 21, "Wi-Fi STA", dx=-7, dy=-7)
    dupla(66, 21, 76, 21)
    dupla(94, 25, 102, 63, "HTTPS", dx=-5.5, dy=4)
    dupla(94, 19, 102, 21, "UDP", dx=0, dy=-5.5)
    fig.savefig(SAIDA / "fig_comunicacao.png", dpi=DPI); plt.close(fig)


for f in (fig_blocos, fig_ligacoes, fig_potencia, fig_fluxo_controle, fig_estados, fig_fluxo_rede, fig_comunicacao):
    f(); print("gerada:", f.__name__)
