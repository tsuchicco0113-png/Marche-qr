/*
  MarcheDisplay — Marché QR Maker の画面を M5Stack PaperDIY に表示するスケッチ

  ・microSD（なければ本体のフラッシュ）の /marche/*.png を、名前順に表示します
    本体のフラッシュは「ツール → Partition Scheme」に SPIFFS か FATFS の領域があるときに使えます
  ・ボタンB：次の画面　ボタンA：前の画面　ボタンA長押し：最初の画面
  ・電源ボタンをダブルクリックすると電源が切れます。表示はそのまま残ります
  ・次に電源を入れると、最後に表示していた画面から始まります
  ・操作しないまま AUTO_OFF_SEC 秒たつと、自動で電源が切れます（表示は残るので電池をほぼ使いません）
    電源ボタンを1回押すと起動します。USBで書き込むときは、先に電源を入れておいてください
  ・Marché QR Maker の「PaperDIYに書き込む（USB）」から、USBケーブルで画像を受け取れます

  ビルド（Arduino IDE）
    ボード：M5Stack → M5PaperDIY（M5Stack ボードパッケージ 3.3.10 以降）
    ライブラリ：M5Unified 0.2.26 以降 / M5GFX 0.2.33 以降
    ツール → USB CDC On Boot：Enabled（USB書き込みを使うとき）

  USBの受け取り手順（1行ずつ、改行で区切る）
    HELLO              → MARCHE-PAPER 2 <SD|FLASH>
    CLR                → /marche の画像をすべて消す → OK
    PUT <名前> <バイト数> → READY → （1024バイトずつ送り、そのつど ACK を待つ）→ 最後に OK
    SHOW <番号>         → その画面を表示 → OK
    LS                 → ファイル名を1行ずつ → END
*/
// SD / LittleFS は M5Unified より「先に」読み込む（M5GFX がファイルから PNG を描けるようになる）
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <FFat.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>

// microSD（PaperDIYのピン配置）
static constexpr int PIN_SD_SCK = 39, PIN_SD_MISO = 40, PIN_SD_MOSI = 38, PIN_SD_CS = 47;
static const char *DIR = "/marche";
// 操作がないとき自動で電源を切るまでの秒数（0 にすると切らない＝電池は約3時間）
static constexpr uint32_t AUTO_OFF_SEC = 45;
static constexpr size_t CHUNK = 1024;   // USBで受け取る1回分のバイト数（アプリ側と同じ）
uint32_t lastActive = 0;

fs::FS *store = nullptr;
const char *storeName = "NONE";
std::vector<String> pages;
int cur = 0;
Preferences prefs;
String rx;

static void mountStorage() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (SD.begin(PIN_SD_CS, SPI, 20000000)) { store = &SD; storeName = "SD"; }
  else if (LittleFS.begin(true)) { store = &LittleFS; storeName = "FLASH"; }            // 名前に SPIFFS が付く区画
  else if (FFat.begin(true)) { store = &FFat; storeName = "FLASH"; }                      // 名前に FATFS が付く区画
  if (store && !store->exists(DIR)) store->mkdir(DIR);
}

static bool isPng(String n) {
  if (n.startsWith(".")) return false;   // Macが作る ._ファイル などは無視
  n.toLowerCase();
  return n.endsWith(".png");
}

static void scanPages() {
  pages.clear();
  if (!store) return;
  File d = store->open(DIR);
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String n = f.name();
    int s = n.lastIndexOf('/');
    if (s >= 0) n = n.substring(s + 1);
    if (!f.isDirectory() && isPng(n)) pages.push_back(String(DIR) + "/" + n);
  }
  std::sort(pages.begin(), pages.end());
}

// PNGの幅と高さ（IHDR）を読む
static bool pngSize(const String &path, uint32_t &w, uint32_t &h) {
  File f = store->open(path);
  if (!f) return false;
  uint8_t b[24];
  bool ok = f.read(b, 24) == 24 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
  f.close();
  if (!ok) return false;
  w = ((uint32_t)b[16] << 24) | ((uint32_t)b[17] << 16) | ((uint32_t)b[18] << 8) | b[19];
  h = ((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16) | ((uint32_t)b[22] << 8) | b[23];
  return true;
}

static void showMessage() {
  M5.Display.setRotation(0);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setFont(&fonts::efontJA_24);
  M5.Display.setTextDatum(middle_center);
  int cx = M5.Display.width() / 2, cy = M5.Display.height() / 2;
  M5.Display.drawString("Marche QR Maker", cx, cy - 70);
  M5.Display.drawString("表示する画像がありません", cx, cy - 10);
  if (store) {
    M5.Display.drawString("microSDの marche フォルダに", cx, cy + 30);
    M5.Display.drawString("PNGを入れてください", cx, cy + 62);
  } else {
    M5.Display.drawString("保存先がありません", cx, cy + 30);
    M5.Display.drawString("microSDを差すか、Partition Scheme を", cx, cy + 62);
    M5.Display.drawString("SPIFFS / FATFS 付きにしてください", cx, cy + 94);
  }
  M5.Display.endWrite();
  M5.Display.waitDisplay();
}

static void showPage(int i) {
  if (pages.empty()) { showMessage(); return; }
  int n = pages.size();
  cur = ((i % n) + n) % n;
  const String &p = pages[cur];
  uint32_t w = 0, h = 0;
  pngSize(p, w, h);
  int rot = 0;   // 画像の縦横に合わせて画面の向きを選ぶ（上下の反転はアプリ側で済ませてある）
  for (int r = 0; r < 2; r++) {
    M5.Display.setRotation(r);
    if ((uint32_t)M5.Display.width() == w && (uint32_t)M5.Display.height() == h) { rot = r; break; }
  }
  M5.Display.setRotation(rot);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);   // 16階調をきれいに出す（残像も消える）
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.drawPngFile(*store, p.c_str(), 0, 0);
  M5.Display.endWrite();
  M5.Display.waitDisplay();
  prefs.putInt("cur", cur);
}

static String safeName(const String &n) {
  String o;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') o += c;
  }
  if (!o.length() || o[0] == '.') o = "page.png";
  return o;
}

static void handleCmd(String line) {
  line.trim();
  if (line == "HELLO") { Serial.printf("MARCHE-PAPER 2 %s\n", storeName); return; }
  if (!store) { Serial.println("ERR nostore"); return; }
  if (line == "LS") {
    scanPages();
    for (auto &p : pages) Serial.println(p);
    Serial.println("END");
    return;
  }
  if (line == "CLR") {
    scanPages();
    for (auto &p : pages) store->remove(p);
    pages.clear();
    Serial.println("OK");
    return;
  }
  if (line.startsWith("PUT ")) {
    int sp = line.indexOf(' ', 4);
    if (sp < 0) { Serial.println("ERR args"); return; }
    String path = String(DIR) + "/" + safeName(line.substring(4, sp));
    long size = line.substring(sp + 1).toInt();
    if (size <= 0 || size > 4L * 1024 * 1024) { Serial.println("ERR size"); return; }
    File f = store->open(path, FILE_WRITE);
    if (!f) { Serial.println("ERR open"); return; }
    Serial.println("READY");
    // 1024バイトずつ受け取り、フラッシュに書いてから ACK を返す（送る側はACKを待つので、受け取りがあふれない）
    static uint8_t buf[CHUNK];
    long got = 0;
    bool ok = true;
    while (got < size && ok) {
      size_t want = std::min<long>(CHUNK, size - got), have = 0;
      uint32_t t = millis();
      while (have < want) {
        int avail = Serial.available();
        if (avail <= 0) { if (millis() - t > 5000) { ok = false; break; } delay(1); continue; }
        size_t r = Serial.read(buf + have, std::min<size_t>(avail, want - have));
        if (r > 0) { have += r; t = millis(); }
      }
      if (!ok) break;
      if (f.write(buf, have) != have) { ok = false; break; }
      got += have;
      lastActive = millis();
      Serial.println("ACK");
    }
    f.close();
    if (!ok || got != size) { store->remove(path); Serial.println(ok ? "ERR size" : "ERR timeout"); return; }
    Serial.println("OK");
    return;
  }
  if (line.startsWith("SHOW")) {
    scanPages();
    showPage(line.length() > 5 ? line.substring(5).toInt() : 0);
    Serial.println("OK");
    return;
  }
  Serial.println("ERR cmd");
}

static void pollSerial() {
  while (Serial.available()) {
    lastActive = millis();
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (rx.length()) { String l = rx; rx = ""; handleCmd(l); }
    } else if (rx.length() < 200) rx += c;
  }
}

void setup() {
  Serial.setRxBufferSize(4096);   // 1回分（1024バイト）が余裕で入る大きさ
  auto cfg = M5.config();
  cfg.clear_display = false;   // 前の表示を消さずに起動
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  prefs.begin("marche", false);
  cur = prefs.getInt("cur", 0);
  mountStorage();
  scanPages();
  showPage(cur);
  lastActive = millis();
}

void loop() {
  M5.update();
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) lastActive = millis();
  if (M5.BtnB.wasClicked()) showPage(cur + 1);
  else if (M5.BtnA.wasHold()) showPage(0);
  else if (M5.BtnA.wasClicked()) showPage(cur - 1);
  pollSerial();
  if (Serial) lastActive = millis();   // PCのブラウザが接続しているあいだは切らない
  if (AUTO_OFF_SEC && millis() - lastActive > AUTO_OFF_SEC * 1000UL) {
    M5.Display.waitDisplay();   // 描き替えが終わってから
    M5.Power.powerOff();        // 電子ペーパーなので表示はそのまま残る
  }
  delay(10);
}
