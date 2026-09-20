// ================================================================================
// credenciais do protótipo (NUNCA publique este arquivo preenchido)
// ================================================================================
#pragma once

// Rede COM internet (ex.: roteador/hotspot do celular). Deixe "" para operar só com o painel local (Access Point), sem Telegram.
#define WIFI_SSID        ""
#define WIFI_SENHA       ""

// Rede própria do ESP32 (painel local em http://192.168.4.1)
#define AP_SSID          "Ventilacao-ESP32"
#define AP_SENHA         "bovinos2026"

// Telegram: token do bot criado no @BotFather e o seu chat_id (número). Deixe "" para desativar o Telegram.
#define TG_TOKEN         ""
#define TG_CHAT_ID       ""

// Chamada de voz de alerta pelo serviço gratuito CallMeBot.
// 1) No Telegram, envie /start para @CallMeBot_txtbot para autorizar;
// 2) Preencha com o seu usuário, ex.: "@seu_usuario". Deixe "" para desativar.
#define CALLMEBOT_USER   ""

// Voz da síntese
#define CALLMEBOT_LANG   "pt-BR-Standard-A"