#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <SD.h>

char ssid[64]     = "wifi";
char password[64] = "password";

const char* AI_URL     = "https://text.pollinations.ai/openai/chat/completions";
const char* AI_MODEL   = "openai";
const char* WIKI_API   = "https://ru.wikipedia.org/w/api.php";
const char* FANDOM_API = "https://community.fandom.com/ru/api.php";

String systemPrompt =
  "Отвечай на русском языке, живо и по сути, без markdown. "
  "Опирайся только на раздел с данными из источников, если он есть. "
  "Если данных нет или они не отвечают на вопрос, прямо скажи, что не знаешь. Не выдумывай факты. "
  "Ответ не длиннее 80 слов. "
  "Если пользователь просит изменить настройки ESP32, добавь в самый конец: "
  "[PATCH]{\"title\":\"Имя\",\"invert\":true,\"ir_pulses\":3,\"banner\":\"Текст\"}[/PATCH] (только нужные поля).";

#define BTN_UP    32
#define BTN_DOWN  33
#define BTN_LEFT  25
#define BTN_RIGHT 26
#define BTN_OK    27

#define SD_CS     5
#define SD_SCK    18
#define SD_MISO   19
#define SD_MOSI   23

#define IR_LED    4
#define OLED_SDA  21
#define OLED_SCL  22

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const unsigned char boot_logo_bits[] U8X8_PROGMEM = {
  0x00, 0x7e, 0x7e, 0x00, 0x00, 0x42, 0x42, 0x00, 0x00, 0xff, 0xff, 0x00,
  0x38, 0xff, 0xff, 0x1c, 0x20, 0x01, 0x80, 0x04, 0xff, 0x01, 0x80, 0xff,
  0x20, 0x3d, 0xbc, 0x04, 0x20, 0x25, 0xa4, 0x04, 0xff, 0x3d, 0xbc, 0xff,
  0x20, 0x01, 0x80, 0x04, 0x20, 0x01, 0x80, 0x04, 0xff, 0x7d, 0xbe, 0xff,
  0x20, 0x01, 0x80, 0x04, 0x38, 0xff, 0xff, 0x1c, 0x00, 0xff, 0xff, 0x00,
  0x00, 0x42, 0x42, 0x00, 0x00, 0x7e, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x7e, 0x18, 0x18, 0x7e, 0x42, 0x18, 0x18, 0x42, 0x7e, 0x18, 0x18, 0x7e,
  0x42, 0x18, 0x18, 0x42, 0x42, 0x7e, 0x7e, 0x42, 0x00, 0x00, 0x00, 0x00
};

String uiTitle          = "=== AI OS ===";
bool   uiInvert         = false;
bool   irBlinkEnabled   = true;
int    irPulses         = 1;
String customBanner     = "Поиск: Википедия";
String pendingPatchJSON = "";
bool   sdAvailable      = false;

#define MAX_CHATS 5
struct ChatItem {
  String title;
  String history;
  bool   active;
};
ChatItem chats[MAX_CHATS];
int activeChatIdx  = -1;
int chatListCursor = 0;

#define MAX_LINES      120
#define LINES_PER_PAGE 4
String pageLines[MAX_LINES];
int totalLines = 0;
int pageIdx    = 0;
int lastQLine  = 0;   // номер строки с которой начинается последний вопрос

enum AppState {
  STATE_MAIN_MENU,
  STATE_CHAT_LIST,
  STATE_CHAT_VIEW,
  STATE_SETTINGS,
  STATE_CONFIRM_OTA
};
AppState currentState = STATE_MAIN_MENU;
int menuCursor = 0;

int kbLayout = 0; // 0=RU, 1=EN, 2=123

const char* kbRU[3][11] = {
  {"й","ц","у","к","е","н","г","ш","щ","з","х"},
  {"ф","ы","в","а","п","р","о","л","д","ж","э"},
  {"я","ч","с","м","и","т","ь","б","ю","ъ","ё"}
};
const char* kbEN[3][11] = {
  {"q","w","e","r","t","y","u","i","o","p","-"},
  {"a","s","d","f","g","h","j","k","l","@","_"},
  {"z","x","c","v","b","n","m",",",".","?","!"}
};
const char* kbSYM[3][11] = {
  {"1","2","3","4","5","6","7","8","9","0","-"},
  {"!","?","@","#","$","%","&","*","(",")","+"},
  {"=","/","\\",":",";","\"","'",",",".","_","~"}
};

const char* getKey(int r, int c) {
  if (kbLayout == 0) return kbRU[r][c];
  if (kbLayout == 1) return kbEN[r][c];
  return kbSYM[r][c];
}

int utf8Len(unsigned char c) {
  if (c < 0x80)         return 1;
  if ((c >> 5) == 0x6)  return 2;
  if ((c >> 4) == 0xE)  return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

void removeLastUTF8Char(String &s) {
  int i = (int)s.length() - 1;
  if (i < 0) return;
  while (i > 0 && (((unsigned char)s[i]) & 0xC0) == 0x80) i--;
  s.remove(i);
}

void dropFirstUTF8Char(String &s) {
  if (s.length() == 0) return;
  s = s.substring(utf8Len((unsigned char)s[0]));
}

String utf8Prefix(const String &s, int maxChars) {
  int count = 0, i = 0;
  while (i < (int)s.length() && count < maxChars) {
    i += utf8Len((unsigned char)s[i]);
    count++;
  }
  if (i > (int)s.length()) i = s.length();
  return s.substring(0, i);
}

String cutAtLine(const String &s, int maxLen) {
  if ((int)s.length() <= maxLen) return s;
  int cut = s.lastIndexOf('\n', maxLen);
  if (cut > 0) return s.substring(0, cut + 1);
  int e = maxLen;
  while (e > 0 && (((unsigned char)s[e]) & 0xC0) == 0x80) e--;
  return s.substring(0, e);
}

String urlEncode(const String &s) {
  const char *hex = "0123456789ABCDEF";
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    unsigned char c = s[i];
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

bool btnPressed(int pin) {
  if (digitalRead(pin) == LOW) {
    delay(40);
    if (digitalRead(pin) == LOW) {
      unsigned long t = millis();
      while (digitalRead(pin) == LOW && (millis() - t < 350)) delay(10);
      return true;
    }
  }
  return false;
}

void saveChatsToSD() {
  if (!sdAvailable) return;
  SD.remove("/chats.json");
  File f = SD.open("/chats.json", FILE_WRITE);
  if (!f) return;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < MAX_CHATS; i++) {
    JsonObject obj = arr.add<JsonObject>();
    obj["a"] = chats[i].active;
    obj["t"] = chats[i].title;
    obj["h"] = chats[i].history;
  }
  serializeJson(doc, f);
  f.close();
}

void loadChatsFromSD() {
  if (!sdAvailable || !SD.exists("/chats.json")) return;
  File f = SD.open("/chats.json", FILE_READ);
  if (!f) return;
  JsonDocument doc;
  if (!deserializeJson(doc, f)) {
    JsonArray arr = doc.as<JsonArray>();
    for (int i = 0; i < MAX_CHATS && i < (int)arr.size(); i++) {
      JsonObject obj = arr[i];
      chats[i].active  = obj["a"] | false;
      chats[i].title   = obj["t"].as<String>();
      chats[i].history = obj["h"].as<String>();
    }
  }
  f.close();
}

void logToSD(const String &q, const String &a) {
  if (!sdAvailable) return;
  File f = SD.open("/ai_chat.txt", FILE_APPEND);
  if (f) {
    f.println("Я: " + q);
    f.println("ИИ: " + a);
    f.println("------------------------");
    f.close();
  }
}

void applyPatch(const String &jsonStr, bool saveToSD) {
  JsonDocument doc;
  if (deserializeJson(doc, jsonStr)) return;

  if (!doc["title"].isNull())  uiTitle = doc["title"].as<String>();
  if (!doc["invert"].isNull()) {
    uiInvert = doc["invert"].as<bool>();
    u8g2.sendF("c", uiInvert ? 0xA7 : 0xA6);
  }
  if (!doc["ir_pulses"].isNull()) irPulses = doc["ir_pulses"].as<int>();
  if (!doc["banner"].isNull())    customBanner = doc["banner"].as<String>();

  if (saveToSD && sdAvailable) {
    SD.remove("/patch.json");
    File f = SD.open("/patch.json", FILE_WRITE);
    if (f) { f.print(jsonStr); f.close(); }
  }
}

void showStatus(const String &l1, const String &l2 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.drawUTF8(4, 24, l1.c_str());
  if (l2.length()) u8g2.drawUTF8(4, 40, l2.c_str());
  u8g2.sendBuffer();
}

void drawBootScreen() {
  for (int p = 0; p <= 100; p += 25) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    u8g2.drawXBMP(48, 2, 32, 24, boot_logo_bits);
    u8g2.drawUTF8(22, 38, "ЗАГРУЗКА AI OS");
    u8g2.drawFrame(14, 44, 100, 10);
    u8g2.drawBox(16, 46, (p * 96) / 100, 6);
    u8g2.sendBuffer();
    delay(150);
  }
}

void addLine(const String &s) {
  if (totalLines < MAX_LINES) pageLines[totalLines++] = s;
}

void wrapText(const String &text) {
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  totalLines = 0;
  lastQLine  = 0;

  int pStart = 0;
  while (pStart <= (int)text.length() && totalLines < MAX_LINES) {
    int nl = text.indexOf('\n', pStart);
    if (nl < 0) nl = text.length();
    String para = text.substring(pStart, nl);

    if (para.startsWith("Я:")) lastQLine = totalLines;

    String line = "";
    int ws = 0;
    while (ws <= (int)para.length()) {
      int sp = para.indexOf(' ', ws);
      if (sp < 0) sp = para.length();
      String word = para.substring(ws, sp);
      String test = line.length() ? line + " " + word : word;
      if (u8g2.getUTF8Width(test.c_str()) > 124 && line.length()) {
        addLine(line);
        line = word;
      } else {
        line = test;
      }
      ws = sp + 1;
    }
    addLine(line);
    pStart = nl + 1;
  }
}

int totalPages() {
  return max(1, (totalLines + LINES_PER_PAGE - 1) / LINES_PER_PAGE);
}

void renderMainMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.drawUTF8(4, 11, uiTitle.c_str());
  u8g2.drawHLine(0, 14, 128);

  String items[3] = {"+ Новый чат", "Мои чаты", "Настройки"};
  for (int i = 0; i < 3; i++) {
    int y = 28 + i * 13;
    if (menuCursor == i) u8g2.drawUTF8(2, y, ("> " + items[i]).c_str());
    else                 u8g2.drawUTF8(12, y, items[i].c_str());
  }
  u8g2.sendBuffer();
}

int firstActiveChat() {
  for (int i = 0; i < MAX_CHATS; i++) if (chats[i].active) return i;
  return -1;
}

int nextActiveChat(int from, int dir) {
  for (int s = 1; s <= MAX_CHATS; s++) {
    int n = ((from + dir * s) % MAX_CHATS + MAX_CHATS) % MAX_CHATS;
    if (chats[n].active) return n;
  }
  return from;
}

void renderChatList() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.drawUTF8(2, 11, "ЧАТЫ OK:Открыть");
  u8g2.drawHLine(0, 14, 128);

  int count = 0, cursorRow = 0;
  for (int i = 0; i < MAX_CHATS; i++) {
    if (chats[i].active) {
      if (i == chatListCursor) cursorRow = count;
      count++;
    }
  }

  if (count == 0) {
    u8g2.drawUTF8(8, 36, "Чатов пока нет");
  } else {
    int first = max(0, cursorRow - 2);
    int row = 0;
    for (int i = 0; i < MAX_CHATS; i++) {
      if (!chats[i].active) continue;
      if (row >= first && row < first + 3) {
        int y = 26 + (row - first) * 11;
        String p = (chatListCursor == i) ? "> " : "  ";
        u8g2.drawUTF8(2, y, (p + chats[i].title).c_str());
      }
      row++;
    }
  }

  u8g2.drawHLine(0, 52, 128);
  u8g2.drawUTF8(2, 62, "ЛЕВ:Удалить ПРАВ:Меню");
  u8g2.sendBuffer();
}

void renderChatView() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  for (int i = 0; i < LINES_PER_PAGE; i++) {
    int idx = pageIdx * LINES_PER_PAGE + i;
    if (idx < totalLines) u8g2.drawUTF8(0, 11 + i * 12, pageLines[idx].c_str());
  }
  u8g2.drawHLine(0, 51, 128);
  u8g2.drawUTF8(0, 62, "OK:Ввод ЛЕВ:Меню");
  String pg = String(pageIdx + 1) + "/" + String(totalPages());
  u8g2.drawUTF8(128 - u8g2.getUTF8Width(pg.c_str()) - 2, 62, pg.c_str());
  u8g2.sendBuffer();
}

void renderSettings() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.drawUTF8(2, 11, "НАСТРОЙКИ");
  u8g2.drawHLine(0, 14, 128);
  u8g2.drawUTF8(2, 26, ("Статус: " + customBanner).c_str());
  u8g2.drawUTF8(2, 38, ("Инверсия: " + String(uiInvert ? "ВКЛ" : "ВЫКЛ")).c_str());
  u8g2.drawUTF8(2, 50, ("SD: " + String(sdAvailable ? "ОК" : "НЕТ")).c_str());
  u8g2.drawUTF8(2, 62, "[OK/ЛЕВ] Назад");
  u8g2.sendBuffer();
}

void renderConfirmOTA() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawUTF8(4, 13, "ИИ ПРИСЛАЛ ОБНОВЛЕНИЕ");
  u8g2.drawHLine(0, 16, 128);
  u8g2.drawUTF8(6, 29, "Добавить этот код");
  u8g2.drawUTF8(6, 41, "в систему?");
  u8g2.drawHLine(0, 48, 128);
  u8g2.drawUTF8(6, 60, "[ЛЕВ] Нет  [ПРАВ] Да");
  u8g2.sendBuffer();
}

void renderKeyboard(const String &hint, const String &buf, int x, int y) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);

  String disp = hint.length() ? (hint + ": ") : String("");
  disp += buf + "_";
  while (u8g2.getUTF8Width(disp.c_str()) > 124 && disp.length() > 1) dropFirstUTF8Char(disp);
  u8g2.drawUTF8(2, 11, disp.c_str());
  u8g2.drawHLine(0, 14, 128);

  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 11; c++) {
      int kx = 2 + c * 11;
      int ky = 25 + r * 12;
      if (x == c && y == r) u8g2.drawFrame(kx - 2, ky - 10, 11, 12);
      u8g2.drawUTF8(kx, ky, getKey(r, c));
    }
  }

  int yBot = 62;
  u8g2.drawUTF8(4,  yBot, "Spc");
  u8g2.drawUTF8(36, yBot, "<");
  u8g2.drawUTF8(56, yBot, kbLayout == 0 ? "RU" : (kbLayout == 1 ? "EN" : "123"));
  u8g2.drawUTF8(92, yBot, "ОТПР");

  if (y == 3) {
    if (x == 0) u8g2.drawFrame(1,  yBot - 10, 24, 12);
    if (x == 1) u8g2.drawFrame(33, yBot - 10, 14, 12);
    if (x == 2) u8g2.drawFrame(53, yBot - 10, 24, 12);
    if (x == 3) u8g2.drawFrame(89, yBot - 10, 36, 12);
  }
  u8g2.sendBuffer();
}

int moveX(int x, int fromY, int toY) {
  if (fromY != 3 && toY == 3) {
    if (x <= 2) return 0;
    if (x <= 5) return 1;
    if (x <= 8) return 2;
    return 3;
  }
  if (fromY == 3 && toY != 3) {
    const int up[4] = {0, 2, 6, 9};
    return up[x];
  }
  return x;
}

String readInput(const String &hint, int layout) {
  String buf = "";
  kbLayout = layout;
  int x = 0, y = 0;
  renderKeyboard(hint, buf, x, y);

  while (true) {
    int maxCol = (y == 3) ? 3 : 10;
    bool redraw = false;

    if (btnPressed(BTN_LEFT))  { x = (x == 0) ? maxCol : x - 1; redraw = true; }
    if (btnPressed(BTN_RIGHT)) { x = (x >= maxCol) ? 0 : x + 1; redraw = true; }
    if (btnPressed(BTN_UP))    { int ny = (y == 0) ? 3 : y - 1; x = moveX(x, y, ny); y = ny; redraw = true; }
    if (btnPressed(BTN_DOWN))  { int ny = (y + 1) % 4;          x = moveX(x, y, ny); y = ny; redraw = true; }

    if (btnPressed(BTN_OK)) {
      if (y < 3)       buf += getKey(y, x);
      else if (x == 0) buf += " ";
      else if (x == 1) removeLastUTF8Char(buf);
      else if (x == 2) kbLayout = (kbLayout + 1) % 3;
      else {
        kbLayout = 0;
        return buf;
      }
      redraw = true;
    }

    if (redraw) renderKeyboard(hint, buf, x, y);
    delay(20);
  }
}

bool connectWiFi(unsigned long timeoutMs) {
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid, password);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t < timeoutMs)) delay(300);
  return WiFi.status() == WL_CONNECTED;
}

void connectWiFiWithPrompt() {
  while (true) {
    showStatus("Подключаюсь к сети:", ssid);
    if (connectWiFi(10000)) return;

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    u8g2.drawFrame(0, 0, 128, 64);
    u8g2.drawUTF8(6, 13, "СЕТЬ НЕ НАЙДЕНА!");
    u8g2.drawHLine(0, 16, 128);
    u8g2.drawUTF8(6, 29, "Сменить сеть?");
    u8g2.drawHLine(0, 48, 128);
    u8g2.drawUTF8(6, 60, "[ЛЕВ] Нет  [ПРАВ] Да");
    u8g2.sendBuffer();

    bool wantNew = false;
    while (true) {
      if (btnPressed(BTN_RIGHT)) { wantNew = true;  break; }
      if (btnPressed(BTN_LEFT))  { wantNew = false; break; }
      delay(20);
    }
    if (!wantNew) return;

    String newSsid = readInput("Имя WiFi", 1);
    String newPass = readInput("Пароль", 1);
    if (newSsid.length() > 0) {
      newSsid.toCharArray(ssid, sizeof(ssid));
      newPass.toCharArray(password, sizeof(password));
    }
  }
}

bool httpGet(const String &url, String &body) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "AIOS-ESP32/1.0 (educational project)");
  http.setTimeout(15000);
  int code = http.GET();
  if (code == HTTP_CODE_OK) body = http.getString();
  http.end();
  return code == HTTP_CODE_OK;
}

String searchMediaWiki(const char* apiBase, const String &query, int limit) {
  String url = String(apiBase) +
    "?action=query&generator=search&gsrlimit=" + String(limit) +
    "&gsrsearch=" + urlEncode(query) +
    "&prop=extracts&exintro=1&explaintext=1&exchars=300&redirects=1&format=json";

  String body;
  if (!httpGet(url, body)) return "";

  JsonDocument doc;
  if (deserializeJson(doc, body)) return "";

  String out = "";
  JsonObject pages = doc["query"]["pages"];
  for (JsonPair kv : pages) {
    String extract = kv.value()["extract"].as<String>();
    if (extract.length() == 0) continue;
    out += "- " + kv.value()["title"].as<String>() + ": " + extract + "\n";
  }
  return out;
}

String searchWeb(const String &query) {
  String wiki   = cutAtLine(searchMediaWiki(WIKI_API,   query, 2), 600);
  String fandom = cutAtLine(searchMediaWiki(FANDOM_API, query, 2), 600);

  String ctx = "";
  if (wiki.length())   ctx += "Википедия:\n" + wiki;
  if (fandom.length()) ctx += "Fandom:\n" + fandom;
  return ctx;
}

String askModel(const String &userText) {
  JsonDocument doc;
  doc["model"] = AI_MODEL;
  JsonArray messages = doc["messages"].to<JsonArray>();
  JsonObject sysMsg = messages.add<JsonObject>();
  sysMsg["role"]    = "system";
  sysMsg["content"] = systemPrompt;
  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"]    = "user";
  userMsg["content"] = userText;

  String body;
  serializeJson(doc, body);

  String result = "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, AI_URL)) return "Ошибка подключения";

  http.addHeader("Content-Type", "application/json");
  http.setTimeout(30000);
  int code = http.POST(body);
  if (code == HTTP_CODE_OK) {
    JsonDocument res;
    if (!deserializeJson(res, http.getString())) {
      result = res["choices"][0]["message"]["content"].as<String>();
    } else {
      result = "Ошибка разбора ответа";
    }
  } else {
    result = "Ошибка HTTP " + String(code);
  }
  http.end();

  result.trim();
  if (result.length() == 0) result = "Пустой ответ";
  return result;
}

void openChat(int idx) {
  activeChatIdx = idx;
  wrapText(chats[idx].history);
  pageIdx = lastQLine / LINES_PER_PAGE;
  if (pageIdx >= totalPages()) pageIdx = totalPages() - 1;
  if (pageIdx < 0) pageIdx = 0;
  currentState = STATE_CHAT_VIEW;
  renderChatView();
}

bool createNewChat() {
  for (int i = 0; i < MAX_CHATS; i++) {
    if (!chats[i].active) {
      chats[i].active  = true;
      chats[i].title   = "Чат " + String(i + 1);
      chats[i].history = "Задай вопрос, нажми OK.";
      saveChatsToSD();
      openChat(i);
      return true;
    }
  }
  return false;
}

void sendToAI(const String &prompt) {
  if (activeChatIdx < 0) return;
  ChatItem &chat = chats[activeChatIdx];

  if (WiFi.status() != WL_CONNECTED) {
    chat.history += "\nОшибка: нет Wi-Fi";
    saveChatsToSD();
    openChat(activeChatIdx);
    return;
  }

  showStatus("Ищу подождите это быстро");
  String ctx = searchWeb(prompt);

  showStatus("Думаю...");
  String userText;
  if (ctx.length() > 0) {
    userText = "Данные из источников:\n" + ctx + "\nВопрос: " + prompt;
  } else {
    userText = "Вопрос: " + prompt +
               "\n(Поиск ничего не нашёл. Ответь, только если уверен, и предупреди о возможной неточности.)";
  }

  String result = askModel(userText);

  int pStart = result.indexOf("[PATCH]");
  int pEnd   = result.indexOf("[/PATCH]");
  bool hasPatch = (pStart != -1 && pEnd > pStart);
  if (hasPatch) {
    pendingPatchJSON = result.substring(pStart + 7, pEnd);
    result = result.substring(0, pStart);
    result.trim();
  }

  if (irBlinkEnabled) {
    for (int b = 0; b < irPulses; b++) {
      digitalWrite(IR_LED, HIGH); delay(80);
      digitalWrite(IR_LED, LOW);  delay(80);
    }
  }

  if (chat.history.indexOf("Я:") < 0) {
    chat.title = utf8Prefix(prompt, 12);
  }

  chat.history += "\nЯ: " + prompt + "\nИИ: " + result;

  while (chat.history.length() > 1200) {
    int cut = chat.history.indexOf("\nЯ:", 1);
    if (cut < 0) break;
    chat.history = chat.history.substring(cut);
  }

  logToSD(prompt, result);
  saveChatsToSD();

  if (hasPatch) {
    wrapText(chat.history);
    currentState = STATE_CONFIRM_OTA;
    renderConfirmOTA();
  } else {
    openChat(activeChatIdx);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_DOWN,  INPUT_PULLUP);
  pinMode(BTN_LEFT,  INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_OK,    INPUT_PULLUP);

  pinMode(IR_LED, OUTPUT);
  digitalWrite(IR_LED, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.enableUTF8Print();

  drawBootScreen();

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, SPI, 4000000)) {
    sdAvailable = true;
    loadChatsFromSD();

    if (SD.exists("/patch.json")) {
      File f = SD.open("/patch.json", FILE_READ);
      if (f) {
        String saved = f.readString();
        f.close();
        if (saved.length() > 0) applyPatch(saved, false);
      }
    }
  }

  connectWiFiWithPrompt();

  currentState = STATE_MAIN_MENU;
  renderMainMenu();
}

void loop() {
  if (currentState == STATE_MAIN_MENU) {
    if (btnPressed(BTN_UP)) {
      menuCursor = (menuCursor + 2) % 3;
      renderMainMenu();
    }
    if (btnPressed(BTN_DOWN)) {
      menuCursor = (menuCursor + 1) % 3;
      renderMainMenu();
    }
    if (btnPressed(BTN_OK)) {
      if (menuCursor == 0) {
        if (!createNewChat()) {
          showStatus("Все слоты заняты");
          delay(1000);
          renderMainMenu();
        }
      } else if (menuCursor == 1) {
        int first = firstActiveChat();
        if (first < 0) {
          showStatus("Чатов пока нет");
          delay(1000);
          renderMainMenu();
        } else {
          chatListCursor = first;
          currentState = STATE_CHAT_LIST;
          renderChatList();
        }
      } else {
        currentState = STATE_SETTINGS;
        renderSettings();
      }
    }
  }

  else if (currentState == STATE_CHAT_LIST) {
    if (btnPressed(BTN_UP)) {
      chatListCursor = nextActiveChat(chatListCursor, -1);
      renderChatList();
    }
    if (btnPressed(BTN_DOWN)) {
      chatListCursor = nextActiveChat(chatListCursor, +1);
      renderChatList();
    }
    if (btnPressed(BTN_OK) && chats[chatListCursor].active) {
      openChat(chatListCursor);
    }
    if (btnPressed(BTN_LEFT) && chats[chatListCursor].active) {
      chats[chatListCursor].active  = false;
      chats[chatListCursor].title   = "";
      chats[chatListCursor].history = "";
      if (activeChatIdx == chatListCursor) activeChatIdx = -1;
      saveChatsToSD();
      int first = firstActiveChat();
      if (first < 0) {
        currentState = STATE_MAIN_MENU;
        renderMainMenu();
      } else {
        chatListCursor = first;
        renderChatList();
      }
    }
    if (btnPressed(BTN_RIGHT)) {
      currentState = STATE_MAIN_MENU;
      renderMainMenu();
    }
  }

  else if (currentState == STATE_CHAT_VIEW) {
    if (btnPressed(BTN_UP) && pageIdx > 0) {
      pageIdx--;
      renderChatView();
    }
    if (btnPressed(BTN_DOWN) && pageIdx < totalPages() - 1) {
      pageIdx++;
      renderChatView();
    }
    if (btnPressed(BTN_OK)) {
      String q = readInput("Вопрос", 0);
      if (q.length() > 0) {
        sendToAI(q);
      } else {
        renderChatView();
      }
    }
    if (btnPressed(BTN_LEFT)) {
      currentState = STATE_MAIN_MENU;
      renderMainMenu();
    }
  }

  else if (currentState == STATE_SETTINGS) {
    if (btnPressed(BTN_OK) || btnPressed(BTN_LEFT)) {
      currentState = STATE_MAIN_MENU;
      renderMainMenu();
    }
  }

  else if (currentState == STATE_CONFIRM_OTA) {
    if (btnPressed(BTN_RIGHT)) {
      applyPatch(pendingPatchJSON, true);
      pendingPatchJSON = "";
      chats[activeChatIdx].history += "\n[Система: код применён]";
      saveChatsToSD();
      openChat(activeChatIdx);
    }
    if (btnPressed(BTN_LEFT)) {
      pendingPatchJSON = "";
      chats[activeChatIdx].history += "\n[Система: отклонено]";
      saveChatsToSD();
      openChat(activeChatIdx);
    }
  }
}
