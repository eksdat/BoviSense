// ============================================================================
//  segredos.h — credenciais do protótipo
//  NÃO publique este arquivo preenchido!
// ============================================================================
#pragma once

// Wi-Fi COM internet, obrigatoriamente 2,4 GHz (ex.: roteador do celular).
// Deixe "" para usar só o painel local, sem Telegram.
#define WIFI_SSID        ""
#define WIFI_SENHA       ""

// Rede própria do BoviSense (painel em http://192.168.4.1). Senha >= 8 caracteres.
#define AP_SSID          "BoviSense"
#define AP_SENHA         "bovinos2026"

// Telegram
// TG_TOKEN: token do bot (@BotFather). Se ele vazar, revogue e gere outro.
// TG_CHAT_ID: pode ficar "". Com o token preenchido, mande qualquer mensagem
//   ao bot pelo seu Telegram: ele se vincula ao seu chat e grava o número na
//   memória do ESP32. Não use o número do início do token (é o id do bot) nem
//   o telefone.
#define TG_TOKEN         ""
#define TG_CHAT_ID       ""
