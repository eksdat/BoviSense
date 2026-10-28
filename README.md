# BoviSense

Protótipo de monitoramento ambiental e controle automático de ventilação para instalações de bovinos leiteiros, construído com ESP32.

O sistema mede temperatura, umidade e resposta a gases combustíveis, calcula o Índice de Temperatura e Umidade (ITU) e ajusta a rotação do ventilador em quatro níveis, no lugar de deixá-lo ligado o tempo todo. Tudo foi montado em escala reduzida, numa maquete, como prova de conceito.

Trabalho A3 da unidade curricular Sistemas de Comunicação e Redes, Engenharia de Software, Centro Universitário de Belo Horizonte (UniBH), 2026. Professor: Dr. Rodrigo Romero.

Autores: Bernardo Luiz Silva Edmundo, Pedro Henrique Esperidião Aureliano, Pedro Lucas dos Santos Kaizer, Rafael Araujo Pace, Stanley Borges Papa.

## O problema

A ventilação mecânica melhora as condições ambientais do galpão, mas um ventilador que fica ligado em potência máxima consome energia também nas horas em que isso não é necessário. Reduzir a ventilação sem medir nada, por outro lado, prejudica o conforto térmico dos animais e a qualidade do ar.

A pergunta do trabalho é se um controle adaptativo baseado em medição local reduz o tempo de operação e o consumo da ventilação sem aumentar de forma significativa o período em condições ambientais inadequadas.

## Como funciona

O ESP32 amostra os sensores a cada 2,5 s. Temperatura e umidade passam por média móvel de 4 amostras antes de entrar no cálculo do ITU:

```
ITU = (1,8·T + 32) − (0,55 − 0,0055·UR)·(1,8·T − 26)
```

com T em °C e UR em %. A equação é a de Kelly e Bond (1971), na forma apresentada por Azevedo et al. (2005). Os limiares de 72, 75 e 79 vêm de Armstrong (1994), também apud Azevedo et al. (2005), e separam quatro níveis de ventilação:

| ITU | Nível | Razão cíclica |
|---|---|---|
| abaixo de 72 | 0 | desligado |
| 72 a 75 | 1 | 50 % |
| 75 a 79 | 2 | 75 % |
| acima de 79 | 3 | 100 % |

As transições são assimétricas de propósito. O nível sobe assim que o ITU cruza o limiar, mas só desce depois de 60 s abaixo de (limiar − 2,0) e 60 s de permanência no nível atual. Errar para cima custa energia; errar para baixo custa bem-estar animal. Toda partida do motor começa com um pulso de 1,5 s em 100 % para vencer a inércia.

O MQ-2 entra como índice relativo, IG = R₀/Rs, calculado contra a resistência medida em ar limpo durante os primeiros 60 s depois do aquecimento. Os limiares de gás saem do coeficiente de variação dessa linha de base, e não de valores fixos.

O firmware roda nos dois núcleos: controle e sensores no núcleo 1, Wi-Fi e Telegram no núcleo 0. Uma queda de rede não atrasa o acionamento do ventilador.

## Ligações

| Componente | Terminais | Onde liga |
|---|---|---|
| DHT11 | + / − / OUT | 3V3 / GND / D4 |
| MQ-2 | VCC / GND / AO / DO | 3V3 / GND / D34 / não ligar |
| Buzzer piezo | + / − | D25 / GND |
| TIP122 | B / C / E | resistor de 1 kΩ para D26 (e 10 kΩ da base ao GND) / negativo do cooler / GND |
| Cooler | + / − / tacômetro | positivo da bateria 9 V / coletor do TIP122 / isolar |
| 1N4007 | faixa / outra perna | positivo do cooler / negativo do cooler |

O MQ-2 vai no 3V3, não no VIN. A saída AO acompanha a tensão de alimentação, e com 5 V ela passa do limite de 3,3 V do conversor A/D do D34. Alimentar o sensor com 3,3 V deixa o aquecedor com cerca de 43 % da potência de catálogo, o que está declarado como limitação no artigo.

O cooler e a bateria de 9 V não encostam em nenhum pino do ESP32. A placa entrega 3,3 V e algumas dezenas de miliampères; o motor precisa de muito mais, e é para isso que existe o TIP122. O GND da bateria e o GND do ESP32 precisam estar no mesmo ponto, ou o transistor não chaveia.

## Gravando o firmware

Na IDE Arduino:

1. Preferências, em URL adicional de placas: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Gerenciador de Placas: esp32 by Espressif Systems (testado na versão 3.3.12).
3. Gerenciar Bibliotecas: DHT sensor library, Adafruit Unified Sensor, ArduinoJson 7.x.
4. Copie `firmware/segredos.exemplo.h` para `firmware/segredos.h`. Ele pode ficar em branco: a rede Wi-Fi e as credenciais do Telegram são configuradas pelo painel e ficam gravadas na memória não volátil do ESP32.
5. Abra `firmware/firmware.ino`, selecione ESP32 Dev Module e grave.

O programa fica em `bovisense.cpp`, não no `.ino`. A IDE pré-processa apenas arquivos `.ino`, e esse pré-processamento quebra a página HTML embutida no código. O `.ino` precisa ter o mesmo nome da pasta que o contém, e é por isso que ele se chama `firmware.ino`: é uma exigência da IDE, não uma escolha de estilo.

Se aparecer "Sketch too big", vá em Ferramentas, Partition Scheme, e escolha Minimal SPIFFS (1.9MB APP).

Nunca publique o `segredos.h` preenchido. Se algum token de bot do Telegram já apareceu em print, slide, grupo ou commit, revogue no @BotFather: `/mybots`, o bot, API Token, Revoke.

## Painel e Telegram

Depois de gravar, abra o Monitor Serial a 115200 e aperte EN para ver o autoteste. O ESP32 cria a rede Wi-Fi `BoviSense`; conecte o celular nela e abra `http://192.168.4.1`.

O painel tem duas vistas. A de operação abre por padrão e mostra ITU, temperatura, umidade, gases, nível de ventilação e o gráfico dos últimos 10 minutos. A vista técnica, a um clique, traz as três estratégias, as métricas do ensaio, a calibração dos sensores e a configuração de rede.

Os endereços que mudam estado (`/modo`, `/novo`, `/simular`, `/parar`, `/wifi`, `/tg`, `/cal/*`) só aceitam requisição vinda de 192.168.4.x, ou seja, de quem está na rede do próprio aparelho. Leitura é livre.

Comandos do bot: `/status`, `/relatorio`, `/simular`, `/parar`, `/modo 0..3`, `/novoensaio`, `/calibrar_gas`, `/ligar`.

## Conteúdo

```
firmware/               programa do ESP32 (abra esta pasta na IDE Arduino)
  firmware.ino            aba de instruções
  bovisense.cpp           programa completo, 1334 linhas
  segredos.exemplo.h      modelo em branco, copie para segredos.h
figuras/                figuras do artigo
modelos/                modelagem 3D da case, do galpão e dos suportes (.3mf)
referências/            levantamento bibliográfico, 22 obras em ABNT
```

## Situação

O firmware compila sem avisos no núcleo Arduino-ESP32 3.3.12, ocupando 86 % da área de programa no esquema de partição padrão (57 % em Minimal SPIFFS) e 27 % da memória RAM. A lógica de controle tem testes que rodam no computador, fora do ESP32.

Sensores, painel, Telegram e o estágio de potência com TIP122 estão montados. Os ensaios comparativos entre as três estratégias de ventilação ainda não foram executados, e é deles que sai a tabela de resultados do artigo.

Para gravar sem o transistor montado, use `#define SEM_ATUADOR 1` no início do `bovisense.cpp`. O sistema mede, decide, registra e alerta, mas apenas recomenda o nível em vez de acionar o motor. Serve para validar sensores, painel e Telegram, e não produz dados de ensaio.

## Limitações

O MQ-2 não mede ppm. É um sensor resistivo de baixa seletividade, sem calibração com gás padrão. O que o sistema informa é um índice relativo ao ar limpo medido na inicialização, e é assim que o artigo o trata.

O DHT11 tem exatidão de ±2 °C e ±5 % de umidade relativa, o que propaga para o ITU uma incerteza da ordem de ±2 unidades. Trocar por DHT22 ou SHT31 reduz isso em cerca de quatro vezes.

O consumo de energia é estimado a partir da razão cíclica e da potência nominal do ventilador, não medido. Medir exigiria um sensor de corrente como o INA219.

Os resultados vêm de uma maquete, não de um galpão. Economia de energia medida aqui não se extrapola para uma instalação comercial.

O cliente HTTPS usa `setInsecure()`, sem validação de certificado. Para um protótipo em rede local é aceitável, e está registrado como limitação no artigo.

## Licença

MIT, veja [LICENSE](LICENSE). As referências bibliográficas pertencem a seus respectivos autores e editoras.
