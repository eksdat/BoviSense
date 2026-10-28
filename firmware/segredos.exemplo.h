// ============================================================================
//  segredos.h — credenciais do BoviSense
//  NÃO publique este arquivo preenchido (nem no artigo, banner, grupo ou GitHub).
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
// TG_CHAT_ID: NÚMERO do chat (ex.: "123456789"), NÃO o telefone.
//   Grave o firmware com o token preenchido, envie /start ao seu bot e ele
//   responde com o seu chat_id. Copie aqui e grave de novo.
#define TG_TOKEN         ""
#define TG_CHAT_ID       ""

// Ligação de voz de alerta (serviço gratuito de terceiros CallMeBot)
// 1) No Telegram, envie /start para @CallMeBot_txtbot (autoriza as ligações);
// 2) Você precisa ter um @usuario no Telegram (Configurações > Nome de usuário);
// 3) Preencha abaixo, ex.: "@seu_usuario". Deixe "" para desativar.
#define CALLMEBOT_USER   ""
// Voz da ligação. Se a voz em português falhar, use "en-US-Standard-B".
#define CALLMEBOT_LANG   "pt-BR-Standard-A"
