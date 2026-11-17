/*
 * Monitoramento ambiental e ventilação adaptativa para bovinos leiteiros
 *
 * Abas: firmware.ino (esta, só instruções) | bovisense.cpp (programa) | segredos.h (credenciais)
 * Placa: "ESP32 Dev Module". Monitor Serial: 115200.
 * Bibliotecas: DHT sensor library, Adafruit Unified Sensor, ArduinoJson (7.x)
 *
 * O protótipo não usa display: a interface local é o painel web, em
 * http://192.168.4.1, na rede Wi-Fi criada pelo próprio ESP32.
 *
 * O programa fica no .cpp de propósito: a IDE Arduino pré-processa apenas
 * arquivos .ino, e esse pré-processamento se perde com a página HTML
 * embutida. Arquivos .cpp vão direto para o compilador.
 *
<<<<<<< HEAD
 * Ligações, gravação e limitações: README.md, na pasta principal do projeto.
=======
 * Ligações, gravação e limitações: README.md na raiz do repositório.
>>>>>>> e7a837c01fd09e0d8a28afe6bacf92cf0e35c1f8
 */
