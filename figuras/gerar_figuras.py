#!/usr/bin/env python3
# Figuras do BoviSense (sem OLED): esquema geral e detalhe do estágio de potência.
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle, Circle, Polygon, Wedge

AZUL   = "#28445f"
LARANJA= "#e8812a"
VERDE  = "#2f7d4f"
ROXO   = "#7b5ea7"
VERM   = "#c0392b"
PRETO  = "#1b1f1d"
CINZA  = "#9aa29d"
FUNDO  = "#ffffff"

def caixa(ax, x, y, w, h, texto, cor="#f4f6f4", borda=PRETO, fs=12, peso="normal", lw=1.6):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.012,rounding_size=0.03",
                                fc=cor, ec=borda, lw=lw, zorder=3))
    ax.text(x + w/2, y + h/2, texto, ha="center", va="center", fontsize=fs,
            fontweight=peso, color=PRETO, zorder=4, linespacing=1.45)

def fio(ax, pts, cor, lw=3.0, z=2):
    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=cor, lw=lw,
            solid_capstyle="round", solid_joinstyle="round", zorder=z)

def rotulo(ax, x, y, t, cor, fs=11, ha="center", va="center"):
    ax.text(x, y, t, ha=ha, va=va, fontsize=fs, color=cor, fontweight="bold", zorder=6,
            bbox=dict(fc=FUNDO, ec="none", pad=1.2))

def terminal(ax, x, y, t, cor):
    ax.add_patch(FancyBboxPatch((x, y-0.11), 0.42, 0.22, boxstyle="round,pad=0.01,rounding_size=0.04",
                                fc=cor, ec="none", zorder=5))
    ax.text(x+0.21, y, t, ha="center", va="center", fontsize=10.5, color="#fff",
            fontweight="bold", zorder=6)

# =====================================================================
# FIGURA 2 — esquema geral de ligação (sem tela)
# =====================================================================
fig, ax = plt.subplots(figsize=(15.2, 10.4))
ax.set_xlim(0, 15.2); ax.set_ylim(0, 10.4); ax.axis("off")
fig.patch.set_facecolor(FUNDO)

ax.text(7.6, 10.05, "Placa vista de cima, antena para cima e USB para baixo.",
        ha="center", fontsize=13.5, fontweight="bold", color=PRETO)
ax.text(7.6, 9.72, 'Na serigrafia, o número depois do "D" é o número do GPIO: D4 é o GPIO 4.',
        ha="center", fontsize=11.5, color="#555c57")

# ---- placa ESP32 ----
BX, BY, BW, BH = 6.05, 1.30, 3.10, 8.00
ax.add_patch(FancyBboxPatch((BX, BY), BW, BH, boxstyle="round,pad=0.02,rounding_size=0.06",
                            fc=AZUL, ec="#16293a", lw=2, zorder=3))
ax.add_patch(Rectangle((BX+1.00, BY+BH-0.62), 1.10, 0.42, fc="#b9c0bb", ec="#6f7a73", lw=1, zorder=4))
ax.text(BX+1.55, BY+BH-0.41, "antena", ha="center", va="center", fontsize=9.5, color="#2a2f2c", zorder=5)
ax.add_patch(Rectangle((BX+1.18, BY-0.30), 0.74, 0.30, fc="#b9c0bb", ec="#6f7a73", lw=1, zorder=4))
ax.text(BX+1.55, BY-0.52, "micro-USB → power bank", ha="center", fontsize=10.5, color=PRETO)
ax.text(BX+BW/2, BY+BH-1.55, "ESP32\nDevKit\n30 pinos", ha="center", va="center",
        fontsize=15, fontweight="bold", color="#fff", zorder=5, linespacing=1.35)

ESQ = ["EN","VP","VN","D34","D35","D32","D33","D25","D26","D27","D14","D12","D13","GND","VIN"]
DIR = ["D23","D22","TX0","RX0","D21","D19","D18","D5","TX2","RX2","D4","D2","D15","GND","3V3"]
USA = {"D34","D25","D26","D4","GND","3V3"}
PY = {}
p0, dp = BY + BH - 0.95, 0.483
for i, n in enumerate(ESQ):
    y = p0 - i*dp; ativo = n in USA
    ax.add_patch(Circle((BX+0.20, y), 0.085, fc="#f0d57a" if ativo else "#aab2ad",
                        ec="#6f7a73", lw=.8, zorder=5))
    ax.text(BX+0.40, y, n, ha="left", va="center", fontsize=11 if ativo else 10,
            color="#fff" if ativo else "#b6c0b9", fontweight="bold" if ativo else "normal", zorder=5)
    PY[("E", n)] = y
for i, n in enumerate(DIR):
    y = p0 - i*dp; ativo = n in USA
    ax.add_patch(Circle((BX+BW-0.20, y), 0.085, fc="#f0d57a" if ativo else "#aab2ad",
                        ec="#6f7a73", lw=.8, zorder=5))
    ax.text(BX+BW-0.40, y, n, ha="right", va="center", fontsize=11 if ativo else 10,
            color="#fff" if ativo else "#b6c0b9", fontweight="bold" if ativo else "normal", zorder=5)
    PY[("D", n)] = y

# ---- MQ-2 ----
caixa(ax, 0.75, 7.75, 3.30, 1.05, "MQ-2\nVCC   GND   DO   AO", cor="#fdf0e2", fs=12.5)
ax.text(2.40, 7.52, "DO não é usado: deixe solto", ha="center", fontsize=10, style="italic", color="#555c57")
terminal(ax, 0.12, 8.56, "3V3", VERM); ax.text(0.33, 8.84, "VCC", ha="center", fontsize=9.5, color="#555c57")
terminal(ax, 0.12, 8.02, "GND", "#5b6560"); ax.text(0.33, 7.74, "GND", ha="center", fontsize=9.5, color="#555c57")
fio(ax, [(0.54, 8.56), (0.75, 8.56)], VERM, lw=2.2)
fio(ax, [(0.54, 8.02), (0.75, 8.02)], "#5b6560", lw=2.2)
yA = PY[("E","D34")]
fio(ax, [(4.05,8.00),(5.20,8.00),(5.20,yA),(BX+0.20,yA)], LARANJA)
rotulo(ax, 5.20, (8.00+yA)/2, "AO", LARANJA)

# ---- buzzer ----
yB = PY[("E","D25")]
caixa(ax, 0.75, 5.55, 3.30, 0.95, "Buzzer piezo\n+            −", cor="#f2ecfa", fs=12.5)
terminal(ax, 0.12, 5.78, "GND", "#5b6560")
fio(ax, [(0.54, 5.78), (0.75, 5.78)], "#5b6560", lw=2.2)
fio(ax, [(4.05,6.26),(5.55,6.26),(5.55,yB),(BX+0.20,yB)], ROXO)
rotulo(ax, 5.55, (6.26+yB)/2, "+", ROXO, fs=13)

# ---- estágio de potência (bloco; detalhe na figura seguinte) ----
yF = PY[("E","D26")]
caixa(ax, 0.75, 2.35, 4.35, 2.45,
      "ESTÁGIO DE POTÊNCIA\n\nTIP122  ·  resistores de 1 kΩ e 10 kΩ\ndiodo 1N4007  ·  bateria 9 V  ·  cooler\n\nligações detalhadas na Figura 3",
      cor="#eaf3ed", borda=VERDE, fs=12, lw=2.4)
fio(ax, [(5.10,4.10),(5.72,4.10),(5.72,yF),(BX+0.20,yF)], VERDE)
rotulo(ax, 5.35, 4.10, "PWM", VERDE)
ax.text(2.92, 2.08, "fica na placa perfurada, fora da maquete", ha="center", fontsize=10,
        style="italic", color="#555c57")
terminal(ax, 0.12, 2.95, "GND", "#5b6560")
fio(ax, [(0.54, 2.95), (0.75, 2.95)], "#5b6560", lw=2.2)

# ---- DHT11 ----
yD = PY[("D","D4")]
caixa(ax, 11.15, yD-0.52, 2.80, 1.05, "DHT11\n+            −            OUT", cor="#e6f0f8", fs=12.5)
fio(ax, [(BX+BW-0.20,yD),(11.15,yD)], VERDE)
rotulo(ax, (BX+BW+11.15)/2 - 0.10, yD+0.21, "OUT", VERDE)
terminal(ax, 14.12, yD+0.30, "3V3", VERM); ax.text(14.33, yD+0.58, "+", ha="center", fontsize=12.5, color="#555c57")
terminal(ax, 14.12, yD-0.26, "GND", "#5b6560"); ax.text(14.33, yD-0.54, "−", ha="center", fontsize=12.5, color="#555c57")
fio(ax, [(13.95, yD+0.30), (14.12, yD+0.30)], VERM, lw=2.2)
fio(ax, [(13.95, yD-0.26), (14.12, yD-0.26)], "#5b6560", lw=2.2)

# ---- trilhos ----
terminal(ax, BX+BW+0.25, PY[("D","GND")], "GND", "#5b6560")
fio(ax, [(BX+BW-0.20,PY[("D","GND")]),(BX+BW+0.25,PY[("D","GND")])], "#5b6560", lw=2.4)
terminal(ax, BX+BW+0.25, PY[("D","3V3")], "3V3", VERM)
fio(ax, [(BX+BW-0.20,PY[("D","3V3")]),(BX+BW+0.25,PY[("D","3V3")])], VERM, lw=2.4)

ax.text(11.15, 8.40, "SEM TELA", fontsize=13, fontweight="bold", color=VERDE)
ax.text(11.15, 8.05,
        "A leitura local é o painel web:\nconecte o celular à rede BoviSense\ne abra 192.168.4.1\n\n"
        "O diagnóstico de inicialização sai\npelo Monitor Serial e por bipes.",
        fontsize=11, color="#30403a", va="top", linespacing=1.6)

ax.text(7.6, 0.42, "Pinos de mesmo rótulo (3V3 ou GND) são ligados entre si: faça uma fileira para cada um na placa perfurada.",
        ha="center", fontsize=11.5, color=PRETO)
ax.text(7.6, 0.12, "O MQ-2 vai no 3V3, nunca no VIN: em 5 V a saída AO ultrapassa o limite de 3,3 V da entrada D34.",
        ha="center", fontsize=11.5, color=VERM, fontweight="bold")

fig.savefig("/home/claude/bovisense/docs/figuras/fig2_esquema_ligacoes.png",
            dpi=165, bbox_inches="tight", facecolor=FUNDO)
plt.close(fig)
print("fig2 OK")


# =====================================================================
# FIGURA 3 — detalhe do estágio de potência (TIP122)
# =====================================================================
fig, ax = plt.subplots(figsize=(14.6, 10.2))
ax.set_xlim(0, 14.6); ax.set_ylim(0, 10.2); ax.axis("off")
fig.patch.set_facecolor(FUNDO)

ax.text(7.3, 9.85, "Estágio de potência: como ligar o TIP122",
        ha="center", fontsize=17, fontweight="bold", color=PRETO)
ax.text(7.3, 9.48, "O ESP32 apenas envia o sinal. Quem alimenta o cooler é a bateria de 9 V.",
        ha="center", fontsize=12.5, color="#555c57")

# ---- transistor ----
TX, TY, TW, TH = 5.60, 4.30, 2.55, 2.45
ax.add_patch(FancyBboxPatch((TX, TY+0.62), TW, TH-0.62, boxstyle="round,pad=0.01,rounding_size=0.05",
                            fc="#2e3634", ec=PRETO, lw=2, zorder=3))
ax.add_patch(Rectangle((TX+0.12, TY+TH-0.60), TW-0.24, 0.50, fc="#b9c0bb", ec="#6f7a73", lw=1.4, zorder=4))
ax.add_patch(Circle((TX+TW/2, TY+TH-0.35), 0.11, fc=FUNDO, ec="#6f7a73", lw=1.2, zorder=5))
ax.text(TX+TW/2, TY+1.66, "TIP122", ha="center", va="center", fontsize=17,
        fontweight="bold", color="#fff", zorder=5)
ax.text(TX+TW/2, TY+1.22, "face escrita voltada\npara você", ha="center", va="center",
        fontsize=10, color="#c9d2cc", zorder=5, linespacing=1.4)

PB, PC, PE = TX+0.52, TX+TW/2, TX+TW-0.52
for px, nm in ((PB,"B"), (PC,"C"), (PE,"E")):
    fio(ax, [(px, TY+0.62), (px, TY-0.02)], "#8d9792", lw=4.5)
ax.text(PB-0.26, TY+0.26, "B", ha="center", fontsize=14, fontweight="bold", color=PRETO)
ax.text(PC-0.26, TY+0.26, "C", ha="center", fontsize=14, fontweight="bold", color=PRETO)
ax.text(PE+0.26, TY+0.26, "E", ha="center", fontsize=14, fontweight="bold", color=PRETO)

# ---- base: 1 kΩ até o D26, 10 kΩ até o terra ----
YB = 3.20
fio(ax, [(PB, TY-0.02), (PB, YB), (3.40, YB)], VERDE)
ax.add_patch(Rectangle((2.60, YB-0.17), 0.80, 0.34, fc="#fff8e1", ec=PRETO, lw=1.6, zorder=4))
ax.text(3.00, YB+0.40, "1 kΩ", ha="center", fontsize=12.5, fontweight="bold", color=PRETO)
fio(ax, [(2.60, YB), (1.65, YB)], VERDE)
caixa(ax, 0.35, YB-0.32, 1.30, 0.64, "D26", cor="#28445f", borda="#16293a", fs=14.5, peso="bold")
ax.text(1.00, YB+0.56, "do ESP32", ha="center", fontsize=10.5, color="#555c57", style="italic")
ax.text(1.00, YB-0.58, "sinal PWM", ha="center", fontsize=10.5, color="#555c57", style="italic")

Y10 = 2.10
fio(ax, [(5.05, YB), (5.05, Y10), (5.95, Y10)], VERDE)
ax.add_patch(Circle((5.05, YB), 0.085, fc=VERDE, ec="none", zorder=5))
ax.add_patch(Rectangle((5.95, Y10-0.17), 0.80, 0.34, fc="#fff8e1", ec=PRETO, lw=1.6, zorder=4))
ax.text(6.35, Y10+0.40, "10 kΩ", ha="center", fontsize=12.5, fontweight="bold", color=PRETO)
fio(ax, [(6.75, Y10), (7.55, Y10)], VERDE)

# ---- terra comum ----
YG = 1.05
fio(ax, [(7.55, Y10), (7.55, YG)], VERDE)
fio(ax, [(PE, TY-0.02), (PE, 2.60), (9.10, 2.60), (9.10, YG)], PRETO)
rotulo(ax, 8.35, 2.82, "emissor → terra", PRETO, fs=11)
fio(ax, [(1.20, YG), (13.90, YG)], PRETO, lw=6)
for xn in (7.55, 9.10, 13.75):
    ax.add_patch(Circle((xn, YG), 0.10, fc=PRETO, ec="none", zorder=5))
ax.text(1.20, YG-0.40, "TERRA COMUM", fontsize=12.5, fontweight="bold", color=PRETO, ha="left")
ax.text(1.20, YG-0.72, "o GND do ESP32, o emissor do TIP122 e o negativo da bateria precisam estar "
                       "no mesmo ponto, senão o transistor não chaveia",
        fontsize=11, color=VERM, ha="left")

# ---- coletor até o cooler ----
XC, YF = 9.75, 6.60
CXc, CYc, R = 11.55, YF, 0.95
fio(ax, [(PC, TY-0.02), (PC, 3.70), (XC, 3.70), (XC, YF), (CXc-R, YF)], VERM)
rotulo(ax, 8.60, 3.92, "coletor → fio negativo do cooler", VERM, fs=11)

ax.add_patch(Circle((CXc, CYc), R, fc="#eef1ee", ec=PRETO, lw=2, zorder=3))
for a in (90, 210, 330):
    ax.add_patch(Wedge((CXc, CYc), 0.82, a-26, a+26, fc=CINZA, ec="none", zorder=4))
ax.add_patch(Circle((CXc, CYc), 0.24, fc="#6f7a73", ec=PRETO, lw=1.2, zorder=5))
ax.text(CXc, CYc-R-0.36, "Cooler", ha="center", fontsize=13.5, fontweight="bold", color=PRETO)
ax.text(10.05, 4.92, "terceiro fio = tacômetro:", ha="left", fontsize=10.5, color="#555c57")
ax.text(10.05, 4.58, "não é usado, isole a ponta", ha="left", fontsize=10.5, color="#555c57")

# ---- positivo do cooler até a bateria ----
XP = 12.75
fio(ax, [(CXc+R, YF), (XP, YF), (XP, 3.40)], VERM)
caixa(ax, 12.35, 2.30, 1.80, 1.10, "Bateria 9 V\n\n+                    −", cor="#fdf6e3", fs=12.5)
fio(ax, [(13.75, 2.30), (13.75, YG)], PRETO)
ax.text(13.00, 4.95, "positivo do cooler\n→ positivo da bateria", ha="left", fontsize=10.5,
        color=VERM, fontweight="bold", linespacing=1.45)

# ---- diodo 1N4007, em paralelo com o cooler ----
YD = 8.00
fio(ax, [(XC, YF), (XC, YD), (XP, YD), (XP, YF)], "#30403a", lw=2.4)
ax.add_patch(Polygon([[10.95, YD+0.20], [10.95, YD-0.20], [11.38, YD]], fc="#30403a", ec="none", zorder=5))
fio(ax, [(11.38, YD+0.23), (11.38, YD-0.23)], "#30403a", lw=5, z=5)
ax.text(11.25, YD+0.44, "diodo 1N4007", ha="center", fontsize=12.5, fontweight="bold", color=PRETO)
ax.text(11.25, YD+0.76, "a faixa impressa fica do lado do positivo",
        ha="center", fontsize=10.5, color="#555c57")
ax.text(11.25, YD+1.06, "absorve o pico de tensão do motor ao desligar",
        ha="center", fontsize=10.5, color="#555c57")

# ---- avisos ----
ax.add_patch(FancyBboxPatch((0.35, 6.30), 4.95, 2.78, boxstyle="round,pad=0.02,rounding_size=0.05",
                            fc="#fdeceb", ec=VERM, lw=1.8, zorder=3))
ax.text(0.62, 8.86, "Confira antes de energizar", fontsize=12.5, fontweight="bold",
        color=VERM, va="top", zorder=4)
ax.text(0.62, 8.46,
        "•  As três pernas são B, C e E, da esquerda para a\n"
        "    direita, com a face escrita voltada para você.\n"
        "•  A aba de metal é ligada ao coletor: não deixe\n"
        "    encostar em nada.\n"
        "•  O cooler e a bateria nunca vão num pino D.",
        fontsize=10.5, color="#3a2320", va="top", zorder=4, linespacing=1.68)

ax.text(0.35, 5.95, "Se o cooler não girar nos 50 %, é atrito de partida do motor:",
        fontsize=11, color="#555c57", va="top")
ax.text(0.35, 5.62, "troque DUTY_NIVEL para {0, 65, 80, 100} no firmware.",
        fontsize=11, color="#555c57", va="top")

fig.savefig("/home/claude/bovisense/docs/figuras/fig3_estagio_potencia.png",
            dpi=165, bbox_inches="tight", facecolor=FUNDO)
plt.close(fig)
print("fig3 OK")

# =====================================================================
# FIGURA 1 — diagrama de blocos (sem tela)
# =====================================================================
fig, ax = plt.subplots(figsize=(14.2, 8.4))
ax.set_xlim(0, 14.2); ax.set_ylim(0, 8.4); ax.axis("off")
fig.patch.set_facecolor(FUNDO)

for x, t in ((2.25, "ENTRADAS"), (7.10, "PROCESSAMENTO"), (11.95, "SAÍDAS")):
    ax.text(x, 8.05, t, ha="center", fontsize=14, fontweight="bold", color=PRETO)

ENT = [("DHT11\ntemperatura e umidade (1-Wire)", 6.15),
       ("MQ-2\nresposta a gases (analógica, 3V3)", 4.40),
       ("Botão BOOT\ntroca de modo e simulação", 2.65),
       ("Telegram e painel web\ncomandos do operador", 0.90)]
for t, y in ENT:
    caixa(ax, 0.30, y, 3.90, 1.25, t, cor="#e6f0f8", fs=11.5)
    fio(ax, [(4.20, y+0.62), (4.92, y+0.62)], "#5b6560", lw=2)
    ax.add_patch(Polygon([[4.92, y+0.62], [4.70, y+0.74], [4.70, y+0.50]], fc="#5b6560", ec="none", zorder=5))

ax.add_patch(FancyBboxPatch((4.95, 0.75), 4.30, 6.65, boxstyle="round,pad=0.02,rounding_size=0.06",
                            fc="#fdf8ec", ec="#b08b3e", lw=2, zorder=2))
ax.text(7.10, 7.08, "ESP32", ha="center", fontsize=14, fontweight="bold", color=PRETO, zorder=4)
caixa(ax, 5.20, 4.55, 3.80, 2.20,
      "Núcleo 1 — controle crítico\n\nmédia aparada · ITU · IG\nhisterese · níveis · modo seguro\ncalibração · CSV · métricas",
      cor="#fdecd8", fs=11.5)
caixa(ax, 5.20, 2.05, 3.80, 2.10,
      "Núcleo 0 — comunicação\n\nWi-Fi AP+STA · HTTP local\nHTTPS (TLS) · NTP\nfila de mensagens",
      cor="#e4f1e8", fs=11.5)
caixa(ax, 5.20, 1.00, 3.80, 0.68, "filas e semáforo entre os núcleos", cor="#eef0ee", fs=11)

SAI = [("Cooler (bateria 9 V)\nPWM 0 / 50 / 75 / 100 %\npelo transistor TIP122", 5.95, "#fdeceb"),
       ("Buzzer piezo\nalarme local e códigos\nde diagnóstico", 4.10, "#fdeceb"),
       ("Painel web local\noperação e modo técnico\n192.168.4.1 · CSV", 2.25, "#e9eefb"),
       ("Telegram\nao ligar, de hora em hora,\nalertas e ligação de voz", 0.40, "#e9eefb")]
for t, y, c in SAI:
    caixa(ax, 10.00, y, 3.90, 1.45, t, cor=c, fs=11.5)
    fio(ax, [(9.25, y+0.72), (9.78, y+0.72)], "#5b6560", lw=2)
    ax.add_patch(Polygon([[9.78, y+0.72], [9.56, y+0.84], [9.56, y+0.60]], fc="#5b6560", ec="none", zorder=5))

fig.savefig("/home/claude/bovisense/docs/figuras/fig1_diagrama_blocos.png",
            dpi=165, bbox_inches="tight", facecolor=FUNDO)
plt.close(fig)
print("fig1 OK")
