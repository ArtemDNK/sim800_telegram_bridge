/*
 * ESP8266 + SIM800L — SMS ↔ Telegram Bridge (v2.2)
 */

#define BOT_TOKEN "12345:ABC"
#define CHAT_ID   12345

#include <SoftwareSerial.h>
#include <FastBot.h>
#include <pdulib.h>
#include <WiFiManager.h>
#include <LittleFS.h>

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

// ─── Спам-лист ───────────────────────────────────────────────────────────────
#define BLOCKLIST_FILE  "/blocklist.txt"  // файл во flash (LittleFS)
#define BLOCKLIST_MAX   50                // максимум записей
#define BLOCK_ENTRY_LEN 32                // макс. длина записи в байтах + 1
#define BLOCK_KEY_LEN   64

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

static char blocklist[BLOCKLIST_MAX][BLOCK_ENTRY_LEN];
static int blockCount = 0;
static uint32_t blockedSinceBoot = 0;   // сколько SMS/звонков отсеяно с запуска
static bool fsOk = false;

enum BlockResult { BLOCK_ADDED, BLOCK_EXISTS, BLOCK_FULL, BLOCK_BAD };

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
  if (csq == 99) return "Нет сигнала";
  if (csq <= 5)  return "Очень слабый";
  if (csq <= 10) return "Слабый";
  if (csq <= 15) return "Средний";
  if (csq <= 20) return "Хороший";
  return "Отличный";
}

// ─── Спам-лист ───────────────────────────────────────────────────────────────
// Ключ для сравнения записей:
//  • номер (+, цифры, пробелы, дефисы, скобки) → только цифры, последние 10:
//    +7 900 123-45-67, 89001234567 и 9001234567 считаются одним номером,
//    короткие номера (900, 0611) сравниваются как есть;
//  • имя сервиса (MTS, Tinkoff, Gosuslugi) → без пробелов/дефисов и без учёта
//    регистра (латиница и кириллица).
void makeBlockKey(const char* in, char* out, size_t outSize) {
  char tmp[BLOCK_KEY_LEN];
  size_t t = 0;
  for (const char* p = in; *p && t < sizeof(tmp) - 1; p++) {
    if (*p == ' ' || *p == '-' || *p == '(' || *p == ')') continue;
    tmp[t++] = *p;
  }
  tmp[t] = '\0';

  const char* digits = (tmp[0] == '+') ? tmp + 1 : tmp;
  bool phone = digits[0] != '\0';
  for (const char* p = digits; *p; p++) {
    if (*p < '0' || *p > '9') { phone = false; break; }
  }
  if (phone) {
    size_t len = strlen(digits);
    if (len > 10) digits += len - 10;
    strlcpy(out, digits, outSize);
    return;
  }

  size_t o = 0;
  for (size_t i = 0; tmp[i] && o + 2 < outSize; i++) {
    uint8_t c = tmp[i];
    if (c >= 'A' && c <= 'Z') { out[o++] = c + 32; continue; }
    if (c == 0xD0 && tmp[i + 1]) {                 // кириллица UTF-8: А-Я, Ё → а-я, ё
      uint8_t c2 = tmp[i + 1];
      if (c2 >= 0x90 && c2 <= 0x9F) { out[o++] = 0xD0; out[o++] = c2 + 0x20; i++; continue; }
      if (c2 >= 0xA0 && c2 <= 0xAF) { out[o++] = 0xD1; out[o++] = c2 - 0x20; i++; continue; }
      if (c2 == 0x81)               { out[o++] = 0xD1; out[o++] = 0x91;      i++; continue; }
    }
    out[o++] = c;
  }
  out[o] = '\0';
}

int findBlocked(const char* who) {
  char key[BLOCK_KEY_LEN], other[BLOCK_KEY_LEN];
  makeBlockKey(who, key, sizeof(key));
  if (!key[0]) return -1;
  for (int i = 0; i < blockCount; i++) {
    makeBlockKey(blocklist[i], other, sizeof(other));
    if (strcmp(key, other) == 0) return i;
  }
  return -1;
}

bool isBlocked(const char* who) {
  return findBlocked(who) >= 0;
}

void loadBlocklist() {
  blockCount = 0;
  if (!fsOk) return;
  File f = LittleFS.open(BLOCKLIST_FILE, "r");
  if (!f) return;
  while (f.available() && blockCount < BLOCKLIST_MAX) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.length() >= BLOCK_ENTRY_LEN) continue;
    line.toCharArray(blocklist[blockCount++], BLOCK_ENTRY_LEN);
  }
  f.close();
}

void saveBlocklist() {
  if (!fsOk) return;
  File f = LittleFS.open(BLOCKLIST_FILE, "w");
  if (!f) return;
  for (int i = 0; i < blockCount; i++) {
    f.print(blocklist[i]);
    f.print('\n');
  }
  f.close();
}

BlockResult addBlocked(const char* who) {
  size_t len = strlen(who);
  if (len == 0 || len >= BLOCK_ENTRY_LEN) return BLOCK_BAD;
  char key[BLOCK_KEY_LEN];
  makeBlockKey(who, key, sizeof(key));
  if (!key[0]) return BLOCK_BAD;
  if (findBlocked(who) >= 0) return BLOCK_EXISTS;
  if (blockCount >= BLOCKLIST_MAX) return BLOCK_FULL;
  strlcpy(blocklist[blockCount++], who, BLOCK_ENTRY_LEN);
  saveBlocklist();
  return BLOCK_ADDED;
}

void removeBlocked(int idx) {
  if (idx < 0 || idx >= blockCount) return;
  for (int i = idx; i < blockCount - 1; i++) strcpy(blocklist[i], blocklist[i + 1]);
  blockCount--;
  saveBlocklist();
}

// Уведомление с кнопкой «В спам-лист». Отправитель кладётся в callback_data
// в hex (лимит Telegram — 64 байта), поэтому кнопка не зависит от памяти ESP
// и работает даже после перезагрузки.
void notifyWithBlockButton(const char* text, const char* who) {
  size_t len = strlen(who);
  if (len == 0 || len >= BLOCK_ENTRY_LEN) {
    bot.sendMessage(text);
    return;
  }
  char cb[3 + 2 * BLOCK_ENTRY_LEN] = "b:";
  for (size_t i = 0; i < len; i++) {
    snprintf(cb + 2 + 2 * i, 3, "%02X", (uint8_t)who[i]);
  }
  bot.inlineMenuCallback(text, "🚫 В спам-лист", cb);
}

bool decodeHexSender(const String& hex, char* out, size_t outSize) {
  size_t n = hex.length();
  if (n == 0 || n % 2 != 0 || n / 2 >= outSize) return false;
  for (size_t i = 0; i < n / 2; i++) {
    char h[3] = { hex[2 * i], hex[2 * i + 1], '\0' };
    if (!isxdigit((uint8_t)h[0]) || !isxdigit((uint8_t)h[1])) return false;
    out[i] = (char)strtol(h, nullptr, 16);
  }
  out[n / 2] = '\0';
  return true;
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
      const char* sender = mypdu.getSender();
      if (isBlocked(sender)) {
        blockedSinceBoot++;              // спам — молча пропускаем
      } else {
        char msgBuf[400];
        snprintf(msgBuf, sizeof(msgBuf),
                 "📩 SMS от: %s\n💬 %s",
                 sender, mypdu.getText());
        notifyWithBlockButton(msgBuf, sender);
      }
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

    if (isBlocked(caller)) {
      // Номер в спам-листе — сбрасываем без уведомления (даже если автоотбой выключен)
      blockedSinceBoot++;
      sendAT("ATH", 300);
      inCall = false;
    } else {
      char msg[200];
      snprintf(msg, sizeof(msg),
               "📞 Входящий звонок\nНомер: %s%s",
               caller,
               AUTO_HANGUP ? "\n(автоотбой)" : "");

      notifyWithBlockButton(msg, caller);

      if (AUTO_HANGUP) {
        sendAT("ATH", 300);
        inCall = false;
      }
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

  // Нажатие кнопки «В спам-лист» под уведомлением
  if (msg.query) {
    if (msg.data.startsWith("b:")) {
      char who[BLOCK_ENTRY_LEN];
      if (!decodeHexSender(msg.data.substring(2), who, sizeof(who))) {
        bot.answer("Некорректные данные кнопки", true);
        return;
      }
      BlockResult r = addBlocked(who);
      if (r == BLOCK_FULL) {
        bot.answer("Спам-лист заполнен. Удалите лишнее через /unblock", true);
        return;
      }
      if (r == BLOCK_BAD) {
        bot.answer("Этого отправителя нельзя добавить", true);
        return;
      }
      bot.answer(r == BLOCK_ADDED ? "🚫 Добавлено в спам-лист" : "Уже в спам-листе");
      bot.editMessage(msg.messageID, text + "\n\n🚫 " + who + " — в спам-листе");
    }
    return;
  }

  if (text.startsWith("/sms ")) {
    String body = text.substring(5);
    body.trim();
    int sp = body.indexOf(' ');
    if (sp < 3) {
      bot.sendMessage("❌ Формат: /sms +79001234567 Привет, как дела?");
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
    int percent = (csq < 0 || csq > 31) ? 0 : map(csq, 0, 31, 0, 100);

    char buf[600];
    snprintf(buf, sizeof(buf),
             "📊 Статус устройства\n\n"
             "🌐 WiFi: %s\n"
             "IP: %s\n"
             "📶 GSM: %d (%d%%) — %s\n"
             "Автоотбой: %s\n"
             "Звонок: %s\n"
             "🚫 Спам-лист: %d/%d%s\n"
             "Отсеяно с запуска: %lu",
             WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(),
             csq, percent, getSignalText(csq).c_str(),
             AUTO_HANGUP ? "Вкл" : "Выкл",
             inCall ? "Активен" : "Нет",
             blockCount, BLOCKLIST_MAX, fsOk ? "" : " (flash недоступна!)",
             (unsigned long)blockedSinceBoot);

    bot.sendMessage(buf);
  }
  else if (text == "/blocklist") {
    if (blockCount == 0) {
      bot.sendMessage("Спам-лист пуст.\nДобавить: /block +79001234567 или /block Tinkoff");
      return;
    }
    String out = "🚫 Спам-лист (" + String(blockCount) + "/" + String(BLOCKLIST_MAX) + "):\n\n";
    for (int i = 0; i < blockCount; i++) {
      out += String(i + 1) + ". " + blocklist[i] + "\n";
    }
    out += "\nУдалить: /unblock <номер в списке | телефон | имя>";
    bot.sendMessage(out);
  }
  else if (text == "/block" || text.startsWith("/block ")) {
    String who = text.substring(6);
    int nl = who.indexOf('\n');
    if (nl >= 0) who = who.substring(0, nl);
    who.trim();
    if (who.length() == 0) {
      bot.sendMessage("Формат: /block +79001234567 или /block Tinkoff\n"
                      "Для сервисных SMS укажите имя отправителя так, как оно приходит в уведомлении.");
      return;
    }
    switch (addBlocked(who.c_str())) {
      case BLOCK_ADDED:
        bot.sendMessage("🚫 Добавлено в спам-лист: " + who +
                        (fsOk ? "" : "\n⚠️ Flash недоступна — список сбросится после перезагрузки"));
        break;
      case BLOCK_EXISTS: bot.sendMessage("Уже в спам-листе: " + who); break;
      case BLOCK_FULL:   bot.sendMessage("❌ Спам-лист заполнен (" + String(BLOCKLIST_MAX) + "). Удалите лишнее через /unblock"); break;
      default:           bot.sendMessage("❌ Слишком длинное или пустое значение (максимум " + String(BLOCK_ENTRY_LEN - 1) + " байт)"); break;
    }
  }
  else if (text == "/unblock" || text.startsWith("/unblock ")) {
    String arg = text.substring(8);
    arg.trim();
    if (arg.length() == 0) {
      bot.sendMessage("Формат: /unblock 3 (номер в /blocklist), /unblock +79001234567 или /unblock Tinkoff");
      return;
    }
    int idx = findBlocked(arg.c_str());
    // Если такой записи нет — пробуем как порядковый номер в /blocklist
    if (idx < 0 && arg.length() <= 2 && isDigit(arg[0]) && (arg.length() == 1 || isDigit(arg[1]))) {
      int n = arg.toInt();
      if (n >= 1 && n <= blockCount) idx = n - 1;
    }
    if (idx < 0) {
      bot.sendMessage("Не найдено в спам-листе: " + arg + "\nСписок: /blocklist");
      return;
    }
    String removed = blocklist[idx];
    removeBlocked(idx);
    bot.sendMessage("✅ Удалено из спам-листа: " + removed);
  }
  else if (text == "/restart") {
    doRestart = true;
  }
  else if (text == "/help" || text == "/start") {
    bot.sendMessage(
      "📋 Команды:\n\n"
      "/sms +номер текст — отправить SMS\n"
      "/status — статус\n"
      "/block номер|имя — в спам-лист\n"
      "/unblock номер|имя|N — убрать из спам-листа\n"
      "/blocklist — спам-лист\n"
      "/restart — перезагрузка"
    );
  }
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  GSM.begin(9600);
  delay(500);

  fsOk = LittleFS.begin();   // при первом запуске ФС форматируется автоматически
  loadBlocklist();

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
    snprintf(buf, sizeof(buf), "✅ SMS отправлен на %s", smsNumber);
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
