/*
 * Ventilação adaptativa para instalações de bovinos leiteiros
 *
 * Arquivos:
 *   ventilacao_adaptativa.ino  -> esta aba (apenas instruções)
 *   ventilacao.cpp             -> TODO o programa (configurações no início)
 *   segredos.h                 -> Wi-Fi, token do Telegram e usuário do CallMeBot
 *
 * O código fica no .cpp de propósito: a IDE não pré-processa arquivos .cpp,
 * o que evita erros de geração automática de protótipos com a página HTML
 * embutida. Basta abrir esta pasta na IDE e clicar em "Carregar".
 *
 * Placa: "ESP32 Dev Module"
 * Bibliotecas: Adafruit SSD1306, Adafruit GFX Library, DHT sensor library,
 *              Adafruit Unified Sensor, ArduinoJson (v7)
 * Monitor Serial: 115200 baud (saída em CSV para análise)
 *
 * Configuração padrão deste arquivo: TIP122 acionando o cooler (SEM_ATUADOR 0)
 * e MQ-2 alimentado em 3V3 com AO direto no GPIO34 (MQ2_EM_3V3 1).
 */
