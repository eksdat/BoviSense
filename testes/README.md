# Testes da lógica de controle

Testes executados no computador, sem o ESP32, sobre as mesmas funções gravadas na placa.

> O `extrair.py` copia do `firmware/bovisense.cpp` as funções de cálculo do ITU, da média móvel, da máquina de estágios e da média aparada, sem alterar o texto, e o `testes.cpp` as exercita. Assim, o que é testado é o código do firmware, e não uma reescrita dele.

---

## Execução

```bash
python3 extrair.py
g++ -O2 -std=c++17 -o testes testes.cpp
./testes
```

Para gravar a série da simulação usada na figura de simulação do artigo:

```bash
./testes ../docs/figuras/simulacao.csv
```

---

## Verificações

São 14 verificações em quatro grupos:

- equação do ITU contra a forma original, em Fahrenheit;
- máquina de estágios com histerese: subida imediata, salto de níveis, espera de 60 s e descida de um nível por vez;
- média aparada do ADC contra picos de interferência do rádio;
- contagem de comutações em uma hora de sinal sintético, nas estratégias liga-desliga e adaptativa.
