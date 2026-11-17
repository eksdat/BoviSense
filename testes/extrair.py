#!/usr/bin/env python3
"""Extrai as funções puras do firmware para um cabeçalho testável.
Não reimplementa nada: o texto sai do bovisense.cpp como está, para que o
teste exercite exatamente o código que vai para o ESP32."""
import re, sys, pathlib

FW = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "bovisense.cpp"
s = FW.read_text(encoding="utf8")

def bloco(assinatura):
    """captura a função inteira, equilibrando chaves a partir da assinatura.
    Funciona tanto para funções de uma linha quanto para as de várias."""
    i = s.index(assinatura)
    j = s.index("{", i)
    nivel = 0
    for k in range(j, len(s)):
        if s[k] == "{":
            nivel += 1
        elif s[k] == "}":
            nivel -= 1
            if nivel == 0:
                return s[i:k + 1]
    sys.exit(f"chaves desequilibradas em {assinatura!r}")

def define(nome):
    m = re.search(rf"^#define\s+{nome}\s+(.+?)\s*(?://.*)?$", s, re.M)
    if not m:
        sys.exit(f"não achei #define {nome}")
    return f"#define {nome} {m.group(1)}"

partes = [
    "// GERADO AUTOMATICAMENTE a partir de bovisense.cpp — não editar.",
    "#pragma once",
    "#include <cstdint>",
    "#include <cmath>",
    define("ADC_N"),
    define("N_MEDIA"),
    define("T_DESCIDA_MS"),
    define("T_PERMANENCIA_MS"),
    define("ITU_HISTERESE"),
    "#define PIN_MQ2 34",
    "uint32_t analogReadMilliVolts(int);   // substituído pelo teste",
    "inline void delayMicroseconds(int) {}",
    re.search(r"^struct Estagiador \{.*?\};$", s, re.M).group(0),
    re.search(r"^static float ITU_LIMIAR\[3\] = \{.*?\};$", s, re.M).group(0),
    bloco("static float calcITU("),
    bloco("static float media("),
    bloco("static uint8_t estagiar("),
    bloco("static uint32_t lerMQ2mV("),
]

destino = pathlib.Path(__file__).parent / "logica_extraida.h"
destino.write_text("\n".join(partes) + "\n", encoding="utf8")
print(f"extraído de {FW.name}: calcITU, estagiar, lerMQ2mV, Estagiador, limiares")
