/*
 * ESP8266 + SIM800L — SMS ↔ Telegram Bridge (v2.2)
 */

#define BOT_TOKEN "12345:ABC"
#define CHAT_ID   12345

#include <SoftwareSerial.h>
#include <FastBot.h>
#include <pdulib.h>
#include <WiFiManager.h>

// ─── Пины ────────────────────────────────────────────────────────────────────
#define GSM_RX D2
#define GSM_TX D1

// ─── Буферы ──────────────────────────────────────────────────────────────────
#define PDU_BUFFER_SIZE  250
#define LINE_BUF_SIZE    420

// ─── Настройки ───────────────────────────────────────────────────────────────
#define WIFI_SSID "SSID"
#define WIFI_PASS "Pass"
#define AUTO_HANGUP true
#define SMS_SEND_TIMEOUT 8000

// ─── Объекты ─────────────────────────────────────────────────────────────────
FastBot bot(BOT_TOKEN);
WiFiManager wifiManager;
SoftwareSerial GSM(GSM_RX, GSM_TX);
PDU mypdu(PDU_BUFFER_SIZE);

// ─── Глобальные переменные ───────────────────────────────────────────────────
static char linebuf[LINE_BUF_SIZE];
static int inCount = 0;
static bool nextLinePDU = false;
static bool doRestart = false;
static bool inCall = false;
static bool gsmReady = false;

static bool smsPending = false;
static char smsNumber[32];
static char smsText[182];

// ─── AT-команды ──────────────────────────────────────────────────────────────
void sendAT(const char* cmd, uint16_t waitMs = 300) {
  GSM.println(cmd);
  delay(waitMs);
  while (GSM.available()) GSM.read();
}

// ─── Уровень сигнала GSM ─────────────────────────────────────────────────────
int getSignalQuality() {
  GSM.println("AT+CSQ");
  delay(800);
  String resp = "";
  while (GSM.available()) resp += (char)GSM.read();
  
  int idx = resp.indexOf("+CSQ:");
  if (idx != -1) return resp.substring(idx + 5).toInt();
  return -1;
}

String getSignalText(int csq) {
  if (csq == -1) return "Нет данных";
  if (csq <= 5)  return "Очень слабый";
  if (csq <= 10) return "Слабый";
  if (csq <= 15) return "Средний";
  if (csq <= 20) return "Хороший";
  return "Отличный";
}

// ─── Отправка SMS (кириллица) ────────────────────────────────────────────────
void sendSMS(const char* number, const char* text) {
  sendAT("AT+CMGF=0");
  sendAT("AT+CSCS=\"UCS2\"");

  mypdu.setSCAnumber();
  int len = mypdu.encodePDU(number, text);

  if (len < 0) {
    bot.sendMessage("❌ Ошибка encodePDU");
    return;
  }

  GSM.print("AT+CMGS=");
  GSM.println(len);
  delay(400);

  GSM.print(mypdu.getSMS());
  GSM.write(0x1A);

  delay(SMS_SEND_TIMEOUT);
  while (GSM.available()) GSM.read();

  sendAT("AT+CSCS=\"GSM\"");
  sendAT("AT+CMGF=0");
}

// ─── Инициализация SIM800L ───────────────────────────────────────────────────
void initGSM() {

  sendAT("AT", 1000);
  sendAT("ATE0", 400);
  
  // Ждём готовность SIM
  for (int i = 0; i < 25; i++) {   // до ~40-50 секунд
    GSM.println("AT+CPIN?");
    delay(800);
    
    String resp = "";
    while (GSM.available()) resp += (char)GSM.read();
    
    if (resp.indexOf("READY") != -1) {
      gsmReady = true;
      break;
    }
    if (resp.indexOf("SIM PIN") != -1) {
      bot.sendMessage("🔐 SIM-карта требует PIN-код!");
      return;
    }
    delay(1000);
  }

  if (!gsmReady) {
    bot.tickManual();
    bot.sendMessage("⚠️ SIM не готова после 40 сек. Перезагрузка...");
    delay(1000);
    ESP.restart();
  }

  sendAT("AT+CMGF=0", 300);
  sendAT("AT+CNMI=1,2,0,0,0", 300);
  sendAT("AT+CLIP=1", 300);
  sendAT("AT+CSMP=17,167,0,0", 300);
}

// ─── Обработка строки от SIM800L ─────────────────────────────────────────────
void processLine() {
  int len = inCount;
  while (len > 0 && (linebuf[len-1] == '\r' || linebuf[len-1] == '\n'))
    linebuf[--len] = '\0';
  
  linebuf[len] = '\0';        // гарантируем нуль-терминатор

  if (len == 0) {
    inCount = 0;
    return;
  }

  // Входящее SMS
  if (nextLinePDU) {
    if (mypdu.decodePDU(linebuf)) {
      char msgBuf[400];
      snprintf(msgBuf, sizeof(msgBuf),
               "📩 *SMS от:* `%s`\n💬 %s",
               mypdu.getSender(), mypdu.getText());
      bot.sendMessage(msgBuf);
    }
    nextLinePDU = false;
  }
  else if (strncmp(linebuf, "+CMT:", 5) == 0) {
    nextLinePDU = true;
  }

  // Входящий звонок
  else if (strcmp(linebuf, "RING") == 0) {
    inCall = true;
  }
  else if (strncmp(linebuf, "+CLIP:", 6) == 0) {
    char caller[40] = "скрытый номер";
    
    char* p = strchr(linebuf, '"');
    if (p) {
      char* q = strchr(p + 1, '"');
      if (q) {
        size_t numLen = q - p - 1;
        if (numLen > 0 && numLen < sizeof(caller) - 1) {
          strncpy(caller, p + 1, numLen);
          caller[numLen] = '\0';
          
          // Очистка от мусора (на случай битых символов)
          for (size_t i = 0; i < numLen; i++) {
            if (caller[i] < 32 || caller[i] > 126) {
              caller[i] = '\0';
              break;
            }
          }
        }
      }
    }

    char msg[200];
    snprintf(msg, sizeof(msg),
             "📞 *Входящий звонок*\nНомер: `%s`%s",
             caller,
             AUTO_HANGUP ? "\n_(автоотбой)_" : "");
    
    bot.sendMessage(msg);

    if (AUTO_HANGUP) {
      sendAT("ATH", 300);
      inCall = false;
    }
  }
  else if (strcmp(linebuf, "NO CARRIER") == 0 || strcmp(linebuf, "BUSY") == 0) {
    if (inCall) {
      bot.sendMessage("📴 Вызов завершён");
      inCall = false;
    }
  }

  inCount = 0;
}

// ─── Обработчик Telegram ─────────────────────────────────────────────────────
void onTelegramMessage(FB_msg& msg) {
  const String& text = msg.text;

  if (text.startsWith("/sms ")) {
    String body = text.substring(5);
    body.trim();
    int sp = body.indexOf(' ');
    if (sp < 3) {
      bot.sendMessage("❌ Формат: `/sms +79001234567 Привет, как дела?`");
      return;
    }

    String num = body.substring(0, sp);
    String sms = body.substring(sp + 1);
    sms.trim();

    num.toCharArray(smsNumber, sizeof(smsNumber));
    sms.toCharArray(smsText, sizeof(smsText));
    smsPending = true;
    bot.sendMessage("📤 Отправляю SMS...");
  }
  else if (text == "/status") {
    int csq = getSignalQuality();
    int percent = (csq == -1) ? 0 : map(csq, 0, 31, 0, 100);

    char buf[450];
    snprintf(buf, sizeof(buf),
             "📊 *Статус устройства*\n\n"
             "🌐 WiFi: `%s`\n"
             "IP: `%s`\n"
             "📶 GSM: %d (%d%%) — %s\n"
             "Автоотбой: %s\n"
             "Звонок: %s",
             WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(),
             csq, percent, getSignalText(csq).c_str(),
             AUTO_HANGUP ? "Вкл" : "Выкл",
             inCall ? "Активен" : "Нет");

    bot.sendMessage(buf);
  }
  else if (text == "/restart") {
    doRestart = true;
  }
  else if (text == "/help" || text == "/start") {
    bot.sendMessage(
      "📋 *Команды:*\n\n"
      "/sms +номер текст — отправить SMS\n"
      "/status — статус\n"
      "/restart — перезагрузка"
    );
  }
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  GSM.begin(9600);
  delay(500);

  wifiManager.setConnectTimeout(25);
  if (!wifiManager.autoConnect(WIFI_SSID, WIFI_PASS)) {
    ESP.restart();
  }

  bot.setChatID(CHAT_ID);
  bot.sendMessage("✅ ESP8266 + SIM800L запущен\n/help — команды");

  bot.attach(onTelegramMessage);
  delay(1000);
  initGSM();
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  bot.tick();

  if (smsPending) {
    smsPending = false;
    sendSMS(smsNumber, smsText);
    char buf[90];
    snprintf(buf, sizeof(buf), "✅ SMS отправлен на `%s`", smsNumber);
    bot.sendMessage(buf);
    smsNumber[0] = '\0';
    smsText[0] = '\0';
  }

  if (doRestart) {
    doRestart = false;
    bot.tickManual();
    bot.sendMessage("🔄 Перезагрузка...");
    delay(1000);
    ESP.restart();
  }

  while (GSM.available()) {
    char b = GSM.read();
    if (inCount < LINE_BUF_SIZE - 1) {
      linebuf[inCount++] = b;
    }
    if (b == '\n') {
      processLine();
    }
  }
}
