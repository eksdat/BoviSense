# BoviSense

Sistema embarcado de monitoramento ambiental e controle adaptativo de ventilação para instalações de bovinos leiteiros.

> Projeto acadêmico e experimental desenvolvido com ESP32, sensores ambientais e controle automático de ventilação, com foco em ambiência animal, eficiência energética e possibilidade de futura aplicação em instalações pecuárias reais.

Trabalho A3 da unidade curricular Sistemas de Comunicação e Redes, Engenharia de Software, Centro Universitário de Belo Horizonte (UniBH), 2026. Professor: Dr. Rodrigo Romero.

---

## Sobre o projeto

A qualidade ambiental em instalações destinadas a bovinos leiteiros depende de diferentes fatores, incluindo temperatura, umidade relativa, ventilação, concentração de gases e características construtivas do galpão.

Sistemas de ventilação mecânica são utilizados para melhorar as condições ambientais, porém o funcionamento contínuo dos ventiladores pode resultar em consumo energético desnecessário em períodos nos quais a capacidade máxima de ventilação não é necessária.

Este projeto propõe o desenvolvimento de um sistema embarcado capaz de monitorar variáveis ambientais e controlar automaticamente a ventilação de acordo com as condições observadas.

A primeira versão foi desenvolvida como uma prova de conceito em escala reduzida, montada e ensaiada em bancada. O ESP32 mede temperatura, umidade relativa e a resposta de um sensor de gases, calcula o Índice de Temperatura e Umidade (ITU) e ajusta a rotação do ventilador em quatro níveis, em vez de mantê-lo ligado o tempo todo.

A maquete do galpão que acompanha o trabalho é expositiva: serve para explicar a aplicação pretendida na apresentação e não abriga os sensores nem o ventilador.

---

## Problema de pesquisa

A ventilação contínua pode proporcionar condições ambientais adequadas, porém pode consumir energia mesmo quando sua capacidade máxima não é necessária.

Por outro lado, reduzir a ventilação sem considerar as condições ambientais pode prejudicar o conforto térmico dos animais e a qualidade do ar.

A pergunta central do projeto é:

> Um sistema embarcado de controle adaptativo baseado em medições ambientais locais pode reduzir o tempo de operação e o consumo energético da ventilação sem aumentar significativamente o período em condições ambientais inadequadas?

---

## Objetivo geral

Desenvolver e avaliar um sistema embarcado de baixo custo para monitoramento ambiental e controle adaptativo da ventilação em instalações destinadas a bovinos leiteiros.

---

## Objetivos específicos

- monitorar temperatura e umidade relativa do ambiente;
- monitorar a resposta de sensores relacionados à qualidade do ar;
- desenvolver um controlador baseado em ESP32;
- implementar diferentes níveis de ventilação;
- desenvolver sinalização local por painel web e alertas remotos pelo Telegram;
- registrar dados ambientais e operacionais;
- implementar estados de segurança em caso de falha;
- comparar diferentes estratégias de ventilação;
- avaliar o tempo de operação e o esforço de acionamento da ventilação, base para a estimativa do consumo energético;
- desenvolver uma arquitetura que possa futuramente ser escalada para instalações reais.

---

## Funcionamento

![Diagrama de blocos do sistema](docs/figuras/fig1_diagrama_blocos.png)

O ESP32 amostra os sensores a cada 2,5 s. A temperatura e a umidade passam por média móvel de quatro amostras antes do cálculo do ITU:

```
ITU = (1,8·T + 32) − (0,55 − 0,0055·UR)·(1,8·T − 26)
```

em que T é a temperatura do ar, em °C, e UR é a umidade relativa, em %. A equação é a de Kelly e Bond (1971), na forma apresentada por Azevedo et al. (2005). Os limiares de 72, 75 e 79, de Armstrong (1994), também apud Azevedo et al. (2005), separam quatro níveis de ventilação:

| ITU | Nível | Razão cíclica |
|---|---|---|
| abaixo de 72 | 0 | desligado |
| de 72 a 75 | 1 | 50 % |
| de 75 a 79 | 2 | 75 % |
| a partir de 79 | 3 | 100 % |

A subida de nível é imediata quando o ITU atinge o limiar. A descida, de um nível por vez, ocorre somente depois de 60 s com o ITU abaixo do limiar menos a histerese de 2,0 unidades e de 60 s de permanência no nível atual. A assimetria é proposital: errar para cima custa energia, enquanto errar para baixo prejudica o conforto térmico dos animais. Toda partida do motor começa com um pulso de 1,5 s a 100 % para vencer a inércia.

O MQ-2 é tratado como índice relativo, IG = R₀/Rs, calculado em relação à resistência medida em ar limpo durante 60 s, logo após o aquecimento do sensor. Os limiares de gás são definidos a partir do coeficiente de variação dessa linha de base, e não por valores fixos. O nível aplicado ao ventilador é o maior entre o nível pedido pelo ITU e o pedido pelo índice de gases.

O firmware usa os dois núcleos do ESP32: amostragem, decisão e acionamento no núcleo 1; Wi-Fi, painel web e Telegram no núcleo 0, com troca de mensagens por fila. Dessa forma, uma falha de rede não atrasa o acionamento do ventilador. Três falhas seguidas de leitura do DHT11 levam ao modo seguro, com o ventilador a 100 %.

O sistema oferece quatro modos, escolhidos pelo painel ou pelo Telegram:

- 0, contínua: ventilador sempre a 100 %, como referência;
- 1, liga-desliga: 100 % quando o ITU atinge 72 ou o índice de gases atinge o primeiro limiar, e desligado abaixo disso, sem histerese;
- 2, adaptativa: os quatro níveis, com histerese e tempo de permanência;
- 3, verificação funcional: a mesma lógica da adaptativa, com limiares no ITU medido na inicialização acrescido de 1, 2 e 3 unidades, para demonstrar as trocas de nível sem aquecer o ambiente.

---

## Componentes e ligações

![Esquema de ligação](docs/figuras/fig2_esquema_ligacoes.png)

| Componente | Terminais | Ligação |
|---|---|---|
| DHT11 | + / − / OUT | 3V3 / GND / D4 |
| MQ-2 | VCC / GND / AO / DO | 3V3 / GND / D34 / sem ligação |
| Buzzer piezoelétrico | + / − | D25 / GND |
| TIP122 | B / C / E | resistor de 1 kΩ para D26 (e 10 kΩ da base ao GND) / negativo do cooler / GND |
| Cooler | + / − / tacômetro | positivo da bateria de 9 V / coletor do TIP122 / isolado |
| Diodo 1N4007 | faixa / outro terminal | positivo do cooler / negativo do cooler |

O MQ-2 é alimentado pelo 3V3, e não pelo VIN. A saída AO acompanha a tensão de alimentação e, com 5 V, ultrapassaria o limite de 3,3 V do conversor A/D do D34. Com 3,3 V, o aquecedor do sensor opera com cerca de 44 % da potência de catálogo, o que está declarado como limitação no artigo.

O cooler e a bateria de 9 V não são ligados a nenhum pino do ESP32. A placa fornece 3,3 V e algumas dezenas de miliampères, enquanto o motor exige corrente muito maior; por isso o acionamento é feito pelo TIP122. O GND da bateria e o GND do ESP32 devem estar no mesmo ponto, caso contrário o transistor não chaveia.

---

## Gravação do firmware

Na IDE Arduino:

1. em Preferências, no campo de URLs adicionais de placas, informar `https://espressif.github.io/arduino-esp32/package_esp32_index.json`;
2. no Gerenciador de Placas, instalar esp32 by Espressif Systems (testado na versão 3.3.12);
3. em Gerenciar Bibliotecas, instalar DHT sensor library, Adafruit Unified Sensor e ArduinoJson 7.x;
4. copiar `firmware/segredos.exemplo.h` para `firmware/segredos.h`;
5. abrir `firmware/firmware.ino`, selecionar a placa ESP32 Dev Module e gravar.

O `segredos.h` pode ficar em branco: a rede Wi-Fi e as credenciais do Telegram podem ser configuradas pelo painel e ficam gravadas na memória não volátil do ESP32. Quando o `segredos.h` é editado e o firmware é gravado de novo, o valor novo substitui o que estava salvo pelo painel; sem edição, vale o que foi salvo pelo painel.

O programa fica em `bovisense.cpp`, e não no `.ino`. A IDE pré-processa apenas arquivos `.ino`, e esse pré-processamento quebra a página HTML embutida no código. O `.ino` precisa ter o mesmo nome da pasta que o contém, por isso se chama `firmware.ino`.

Caso apareça a mensagem "Sketch too big", selecionar em Ferramentas, Partition Scheme, a opção Minimal SPIFFS (1.9MB APP).

O `segredos.h` preenchido nunca deve ser publicado. Se um token de bot do Telegram já apareceu em captura de tela, slide, grupo ou commit, ele deve ser revogado no @BotFather: `/mybots`, o bot, API Token, Revoke.

Para gravar sem o transistor montado, basta usar `#define SEM_ATUADOR 1` no início do `bovisense.cpp`. O sistema mede, decide, registra e alerta, mas apenas recomenda o nível, sem acionar o motor. Serve para validar sensores, painel e Telegram, e não produz dados de ensaio.

---

## Painel local e Telegram

Depois da gravação, o Monitor Serial a 115200 mostra o autoteste (pressionar EN para reiniciar). O ESP32 cria a rede Wi-Fi `BoviSense`; com o celular conectado a ela, o painel fica em `http://192.168.4.1`.

O painel tem duas vistas. A de operação abre por padrão e mostra ITU, temperatura, umidade, gases, nível de ventilação e o gráfico dos últimos 10 minutos. A vista técnica traz as estratégias, as métricas do ensaio, a calibração dos sensores e a configuração de rede, além do download do registro em CSV.

Os endereços que alteram o estado do sistema (`/modo`, `/novo`, `/simular`, `/parar`, `/wifi`, `/tg`, `/cal/*`) só aceitam requisições vindas da rede do próprio aparelho (192.168.4.x). A leitura é livre.

Para receber os alertas, basta informar o token do bot no `segredos.h` ou no painel. Com o ESP32 ligado e conectado à internet, qualquer mensagem enviada ao bot pelo Telegram vincula aquele chat: o bot responde "Pronto! Este chat foi vinculado" e grava o número do chat na memória do ESP32, que se mantém após uma nova gravação do firmware. A vinculação automática só ocorre enquanto não há chat válido configurado; depois disso, quem escrever ao bot recebe apenas o próprio número. O `TG_CHAT_ID` pode ficar vazio. O número antes dos dois-pontos do token é o identificador do próprio bot, não do usuário, e o firmware o ignora.

Comandos do bot:

- `/status`: leituras atuais;
- `/relatorio`: métricas do ensaio;
- `/modo 0..3`: troca a estratégia;
- `/simular` e `/parar`: emergência de teste de 60 s;
- `/novoensaio`: zera as métricas;
- `/calibrar_gas`: refaz a linha de base do MQ-2.

---

## Ensaios e resultados

Os resultados abaixo estão detalhados no artigo e podem ser reproduzidos a partir dos registros e scripts da pasta `dados/`.

- **Verificação em computador:** as 14 verificações da pasta `testes/` reproduziram o comportamento especificado. Em uma hora de sinal sintético de ITU, a estratégia liga-desliga comutou 185 vezes e a adaptativa, 2 vezes; o tempo equivalente de plena carga foi de 32,8 min na adaptativa, 43,3 min no liga-desliga e 60 min na contínua.
- **Sessão inicial de bancada:** 226 amostras, com temperatura entre 28,0 e 28,9 °C e ITU entre 76,0 e 77,2. Todas as verificações de conformidade foram atendidas.
- **Ensaio por estratégia em ar ambiente (16/11/2026):** 958 amostras de operação, com ITU entre 75,5 e 76,6. O nível aplicado seguiu a regra da estratégia ativa em todas as amostras; sobre as mesmas leituras, a adaptativa operou a 75 %, contra 100 % da contínua e da liga-desliga.
- **Ensaios com perturbação aplicada ao DHT11 (16 e 17/11/2026):** a temperatura foi elevada com o ferro de solda e a umidade, com ar expirado. As quatro trocas de nível seguiram a regra, dez descidas foram corretamente adiadas pela histerese e uma reimplementação independente da regra reproduziu o estágio gravado em todas as 734 amostras. A razão cíclica média da adaptativa foi de 87,5 % e 94,8 %, contra 100 % das estratégias de referência.

O consumo de energia não foi medido. O tempo equivalente de plena carga e a razão cíclica média indicam o esforço de acionamento; convertê-los em energia exige medir a potência do ventilador em cada nível.

---

## Recuperação após reinício

Se o ESP32 reiniciar sozinho (pânico de software, watchdog ou queda de tensão), o firmware retoma o controle em poucos segundos, na mesma estratégia e com a linha de base do MQ-2 guardada na memória do RTC, sem repetir o aquecimento do sensor: nesses reinícios o 3V3 não é desligado e o aquecedor do MQ-2 permanece quente. Ao ligar a placa ou pressionar EN, o aquecimento completo é feito normalmente. O motivo do reinício aparece no Monitor Serial e no Telegram.

O único reinício observado nos ensaios (16/11/2026) teve origem no núcleo Arduino-ESP32 3.3.12: na primeira consulta de DNS, `NetworkManager::hostByName` limpa o cache do lwIP sem a trava do núcleo TCP/IP e, se o SNTP estiver resolvendo o servidor de hora nesse instante, o ESP32 aborta (`assert failed: udp_new_ip_type`). O firmware passou a fazer essa primeira consulta antes de iniciar o SNTP.

---

## Estrutura do repositório

```
dados/                  registros de bancada e scripts de análise
docs/figuras/           figuras do artigo e scripts que as geram
firmware/               programa do ESP32 (abrir esta pasta na IDE Arduino)
  firmware.ino            instruções de uso
  bovisense.cpp           programa completo, 1649 linhas
  segredos.exemplo.h      modelo em branco, copiar para segredos.h
modelos/                modelagem 3D das cases, do galpão e dos suportes (.3mf)
referências/            levantamento bibliográfico em ABNT
testes/                 testes da lógica de controle, executados no computador
```

O firmware compila sem avisos no núcleo Arduino-ESP32 3.3.12, ocupando 87 % da área de programa no esquema de partição padrão (58 % em Minimal SPIFFS) e 27 % da memória RAM em variáveis globais.

---

## Limitações

- o MQ-2 não mede ppm: é um sensor resistivo de baixa seletividade, sem calibração com gás padrão, e o sistema informa apenas um índice relativo ao ar limpo medido na inicialização;
- o DHT11 tem exatidão de ±2 °C e ±5 % de umidade relativa, o que propaga para o ITU uma incerteza da ordem de ±2 unidades; um DHT22 ou SHT31 reduziria essa incerteza;
- o consumo de energia não foi medido, o que exigiria um sensor de corrente como o INA219;
- os ensaios são de bancada e em malha aberta: o ventilador não altera o ar em que os sensores estão, e nenhum resultado é extrapolado para galpões reais, onde o escoamento de ar, o volume e a carga térmica animal são de outra ordem;
- o cliente HTTPS usa `setInsecure()`, sem validação de certificado, o que é aceitável para um protótipo em rede local, mas não para uma instalação real.

---

## Licença

MIT, conforme o arquivo [LICENSE](LICENSE). As referências bibliográficas pertencem a seus respectivos autores e editoras.
