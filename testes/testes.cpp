// Testes da lógica pura do firmware, rodando no computador.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>
#include <algorithm>
#include "logica_extraida.h"

static std::vector<uint32_t> fila_adc;
static size_t pos_adc = 0;
uint32_t analogReadMilliVolts(int) {
    return pos_adc < fila_adc.size() ? fila_adc[pos_adc++] : 0;
}

static int falhas = 0, total = 0;
static void checa(bool ok, const char *nome, const char *detalhe = "") {
    total++;
    if (!ok) { falhas++; printf("  FALHOU  %s %s\n", nome, detalhe); }
    else      printf("  ok      %s %s\n", nome, detalhe);
}

// ITU na forma original, em Fahrenheit (Kelly e Bond, 1971)
static double ituFahrenheit(double tC, double urPct) {
    double tF = 1.8 * tC + 32.0, ur = urPct / 100.0;
    return tF - 0.55 * (1.0 - ur) * (tF - 58.0);
}

// Com um caminho como argumento, grava a série do grupo 4 em CSV, de onde
// sai a figura da simulação no artigo.
int main(int argc, char **argv) {
    FILE *csv = argc > 1 ? fopen(argv[1], "w") : nullptr;
    if (csv) fprintf(csv, "minuto,itu,duty_liga_desliga,duty_adaptativa\n");
    printf("\n=== 1. Equacao do ITU: forma em Celsius x forma original em Fahrenheit ===\n");
    double pior = 0; double piorT = 0, piorU = 0;
    for (double t = 15; t <= 40.001; t += 0.1)
        for (double u = 20; u <= 95.001; u += 0.5) {
            double d = std::fabs(calcITU((float)t, (float)u) - ituFahrenheit(t, u));
            if (d > pior) { pior = d; piorT = t; piorU = u; }
        }
    char buf[120];
    snprintf(buf, sizeof(buf), "(erro maximo %.2e a %.1f C e %.0f %% UR)", pior, piorT, piorU);
    checa(pior < 1e-4, "as duas formas coincidem", buf);

    printf("\n=== 2. Maquina de estagios com histerese ===\n");
    Estagiador e = {ITU_LIMIAR, 3, ITU_HISTERESE, 0, 0, false, 0};
    uint32_t t = 0;
    checa(estagiar(e, 70.0f, t) == 0, "abaixo de 72 fica no nivel 0");
    checa(estagiar(e, 72.0f, t) == 1, "ao cruzar 72 sobe NA HORA para o nivel 1");
    t += 1000; checa(estagiar(e, 79.5f, t) == 3, "salto grande sobe direto ao nivel 3");

    t += 1000;
    checa(estagiar(e, 77.5f, t) == 3, "77,5 esta acima de 79-2: nao desce");
    t += 1000;
    checa(estagiar(e, 76.5f, t) == 3, "76,5 ja esta abaixo, mas ainda sem os 60 s");
    t += 59000;
    checa(estagiar(e, 76.5f, t) == 3, "faltando 1 s ainda nao desce");
    t += 2000;
    checa(estagiar(e, 76.5f, t) == 2, "passados 60 s desce UM nivel");
    checa(estagiar(e, 60.0f, t) == 2, "mesmo com ITU 60 nao pula dois niveis de uma vez");
    t += 61000;
    checa(estagiar(e, 60.0f, t) == 1, "so desce o proximo apos outros 60 s");

    printf("\n=== 3. Media aparada do conversor A/D contra picos do radio ===\n");
    std::mt19937 g(12345);
    std::normal_distribution<double> ruido(0.0, 15.0);
    std::uniform_real_distribution<double> sorte(0.0, 1.0);
    double somaApar = 0, somaSimp = 0, som2Apar = 0, som2Simp = 0;
    const int N = 2000;
    for (int k = 0; k < N; k++) {
        fila_adc.clear(); pos_adc = 0;
        double simples = 0;
        for (int i = 0; i < ADC_N; i++) {
            double v = 800.0 + ruido(g) + (sorte(g) < 0.10 ? 600.0 : 0.0);
            uint32_t mv = (uint32_t)std::max(0.0, v);
            fila_adc.push_back(mv); simples += mv;
        }
        simples /= ADC_N;
        pos_adc = 0;
        double aparada = lerMQ2mV();
        somaApar += aparada; som2Apar += aparada * aparada;
        somaSimp += simples; som2Simp += simples * simples;
    }
    double mA = somaApar / N, dA = std::sqrt(som2Apar / N - mA * mA);
    double mS = somaSimp / N, dS = std::sqrt(som2Simp / N - mS * mS);
    printf("  media simples : %.1f +- %.1f mV  (vies %+.1f)\n", mS, dS, mS - 800.0);
    printf("  media aparada : %.1f +- %.1f mV  (vies %+.1f)\n", mA, dA, mA - 800.0);
    checa(std::fabs(mA - 800.0) < 5.0, "a aparada fica a menos de 5 mV do valor real");
    checa(dA < dS / 4.0, "a aparada dispersa pelo menos 4x menos que a simples");

    printf("\n=== 4. Comutacoes em uma hora (como o firmware decide: sobre a media movel) ===\n");
    std::mt19937 g2(2026);
    std::normal_distribution<double> rn(0.0, 0.4);
    Estagiador ad = {ITU_LIMIAR, 3, ITU_HISTERESE, 0, 0, false, 0};
    int comutaLD = 0, comutaAD = 0, antLD = 0, antAD = 0;
    float bufT[N_MEDIA]; uint8_t idx = 0, n = 0;
    double eqLD = 0, eqAD = 0;
    const double dt = 2.5;
    const uint8_t DUTY[4] = {0, 50, 75, 100};
    for (int k = 0; k * dt < 3600; k++) {
        uint32_t ms = (uint32_t)(k * dt * 1000);
        double minutos = k * dt / 60.0;
        double tReal = 22.6 + rn(g2) + (minutos > 30 ? (minutos - 30) * 2.5 / 30.0 : 0.0);
        double tLida = std::round(tReal);                 // DHT11: resolucao de 1 C
        // o firmware nao decide sobre a leitura crua: filtra por media movel
        // de N_MEDIA amostras e so entao calcula o ITU
        bufT[idx] = (float)tLida; idx = (idx + 1) % N_MEDIA; if (n < N_MEDIA) n++;
        double itu = calcITU(media(bufT, n), 90.0f);
        int ld = (itu >= ITU_LIMIAR[0]) ? 3 : 0;          // liga-desliga, limiar unico
        int nv = estagiar(ad, (float)itu, ms);            // adaptativa, com histerese
        if (ld != antLD) { comutaLD++; antLD = ld; }
        if (nv != antAD) { comutaAD++; antAD = nv; }
        if (csv) fprintf(csv, "%.4f,%.3f,%d,%d\n", minutos, itu, DUTY[ld], DUTY[nv]);
        eqLD += DUTY[ld] / 100.0 * dt;
        eqAD += DUTY[nv] / 100.0 * dt;
    }
    printf("  liga-desliga : %3d comutacoes | plena carga equivalente %.1f min\n", comutaLD, eqLD / 60.0);
    printf("  adaptativa   : %3d comutacoes | plena carga equivalente %.1f min\n", comutaAD, eqAD / 60.0);
    checa(comutaAD < comutaLD / 5, "a adaptativa comuta ao menos 5x menos");
    checa(eqAD < eqLD, "e acumula menos plena carga equivalente");

    if (csv) fclose(csv);
    printf("\n%s  %d de %d verificacoes passaram\n\n",
           falhas ? "RESULTADO: HOUVE FALHA" : "RESULTADO: TUDO PASSOU", total - falhas, total);
    return falhas ? 1 : 0;
}
