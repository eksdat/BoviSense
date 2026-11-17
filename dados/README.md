# Dados de bancada

Registros seriais dos ensaios do protótipo e scripts que geram, a partir deles, os números apresentados no artigo.

> Nenhum valor é corrigido à mão. Linhas corrompidas pelo Monitor Serial são descartadas por critérios objetivos, descritos no início de cada script, e contadas na saída.

---

## Registros

| Arquivo | Conteúdo |
|---|---|
| `registro_bancada.txt` | sessão inicial de bancada, com as quatro estratégias (seção 4.2 do artigo) |
| `registro_2026-11-16_estrategias.txt` | uma sessão por estratégia, em ar ambiente (seção 4.3) |
| `registro_2026-11-16_perturbacao.txt` | primeiro ensaio com perturbação aplicada ao DHT11 (seção 4.4) |
| `registro_2026-11-17_perturbacao.txt` | segundo ensaio com perturbação aplicada ao DHT11 (seção 4.4) |
| `registro_2026-11-16_telegram.txt` | depuração da integração com o Telegram |
| `registro_2026-11-16_ligacao.txt` | teste da ligação de voz pelo CallMeBot, recurso removido do firmware por não completar as ligações |

Nos ensaios com perturbação, a temperatura foi elevada com o ferro de solda aproximado do DHT11 e a umidade, com ar expirado sobre o sensor. Os números de chat do Telegram foram substituídos por `<chat_id>`.

---

## Formato

Cada amostra é uma linha CSV, gravada a cada 2,5 s:

```
t,modo,estado,Tb,Ub,Tf,Uf,ITU,mv,IG,eI,eG,nivel,duty,flags
```

- `t`: carimbo de tempo, em segundos (hora de calendário quando sincronizada por NTP);
- `modo`: estratégia ativa;
- `estado`: fase do firmware (preparo ou operação);
- `Tb` e `Ub`: temperatura e umidade brutas do DHT11;
- `Tf` e `Uf`: temperatura e umidade após a média móvel;
- `ITU`: índice calculado com `Tf` e `Uf`;
- `mv`: saída do MQ-2, em mV;
- `IG`: índice de gases, R₀/Rs;
- `eI` e `eG`: estágio pedido pelo ITU e pelo índice de gases;
- `nivel` e `duty`: nível aplicado e razão cíclica, em %;
- `flags`: indicadores em bits (falha do DHT11, modo seguro, preparo, partida, hora NTP, entre outros).

As linhas iniciadas por `#` são mensagens do firmware (autoteste, rede, Telegram) e não entram nas análises.

---

## Scripts

```bash
python3 analise_bancada.py registro_bancada.txt
python3 analise_estrategias.py registro_2026-11-16_estrategias.txt
python3 analise_perturbacao.py registro_2026-11-16_perturbacao.txt
python3 analise_perturbacao.py registro_2026-11-17_perturbacao.txt
```

- `analise_bancada.py`: amostras por fase e por estratégia e verificações de conformidade da sessão inicial;
- `analise_estrategias.py`: resumo por estratégia (Tabela 4 do artigo) e conformidade do nível com a regra da estratégia ativa;
- `analise_perturbacao.py`: trocas de nível, descidas adiadas pela histerese e reimplementação independente da regra de estágios, comparada amostra a amostra com o estágio gravado pelo firmware (Tabela 5 do artigo).

Os scripts usam apenas a biblioteca padrão do Python 3.
