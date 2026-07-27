// -----------------------------------------------------------------------------
//  M5Stack Unit CardKB2 ESP-NOW レシーバ
//  ハード : piyopiyo-pcb v1.6 (Seeed XIAO ESP32S3 + ILI9488 480x320 SPI LCD)
//
//  CardKB2 側の準備:
//    Fn + Sym + 3 を押して ESP-NOW ブロードキャストモードにする
//    (白 LED が 3 回点滅 / 設定は電源を切っても保持される)
//
//  ESP-NOW データフレーム (UART モードと同一, 全 5 バイト)
//    AA [DATA_LEN] [KEY_ID] [KEY_STATE] [CHECKSUM]
//      DATA_LEN  : 固定 0x03
//      KEY_ID    : キーインデックス 0-43 (= 行番号 x 11 + 列番号, 行 0-3 / 列 0-10)
//      KEY_STATE : 0x01 = 押下 / 0x02 = 離した
//      CHECKSUM  : (DATA_LEN + KEY_ID + KEY_STATE) & 0xFF
//    送信は全 0xFF のブロードキャスト宛。300ms 以上長押しすると 50ms 間隔でリピート。
//
//  受信結果を USB Serial (115200bps) と LCD に表示する。
// -----------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

#include <LovyanGFX.hpp>
#include "lgfx_ILI9488_ESP32S3_SPI.h"

// ---- 設定 -------------------------------------------------------------------
#define ESPNOW_CHANNEL      1     // CardKB2 は「チャンネル 0 = 現在のチャンネル」で送信する。実機実測でも 1
#define CHANNEL_HUNT        0     // 1: 最初の 1 パケットを受信するまでチャンネルを自動で探す (0 = CH 固定)
#define CHANNEL_HUNT_MS     3000  // 何 ms 受信が無ければ次のチャンネルへ移るか
#define ECHO_TEXT           1     // 1: 変換後の文字をシリアルの "TEXT>" 行にも連結表示する

// 送信元 MAC フィルタ (なりすまし対策)
//   ESP-NOW のブロードキャストは認証されないため、同じ形式のフレームを投げれば
//   誰でも偽のキー入力を注入できる。ここに CardKB2 の MAC を書くと他を無視する。
//   全て 0 のままなら送信元を制限しない (MAC はシリアル出力の行頭に表示される)。
static const uint8_t ALLOWED_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static LGFX lcd;

// ---- 画面レイアウト (480 x 320 / setRotation(1)) ----------------------------
static const int SCR_W = 480, SCR_H = 320;

static const int HDR_H = 24;                      // ヘッダ帯

static const int GRID_X = 9, GRID_Y = 28;         // キーマトリクス表示 (11 列 x 4 行)
static const int CELL_W = 42, CELL_H = 18;

static const int INFO_Y = 104, INFO_H = 28;       // 最後に押されたキーの情報行

static const int TXT_X = 8, TXT_Y = 142;          // 入力テキスト表示エリア (内側)
static const int CH_W = 14, CH_H = 24;            // 等幅フォント 1 文字のサイズ
static const int TXT_COLS = 33, TXT_ROWS = 7;     // 33 文字 x 7 行
static const int TXT_W = TXT_COLS * CH_W;
static const int TXT_H = TXT_ROWS * CH_H;

// ---- キーコード表 -----------------------------------------------------------
// KEY_ID をそのまま添字にする (0-43)。ユーザーマニュアルの Table 3/4/5 に対応。
struct KeyDef {
  uint8_t     normal;   // 通常モードの ASCII (0 = 文字出力なし)
  uint8_t     caps;     // Caps (Aa) 有効時の ASCII
  uint8_t     sym;      // Sym モード時の ASCII
  const char *name;     // 特殊キー名 (nullptr なら通常の文字キー)
  const char *label;    // LCD のマトリクス表示用の短縮ラベル (nullptr なら文字そのもの)
};

static const KeyDef KEYMAP[44] = {
    // --- 行 0 : index 0-10 ---
    {'1', '1', '!', nullptr, nullptr},  {'2', '2', '@', nullptr, nullptr},
    {'3', '3', '#', nullptr, nullptr},  {'4', '4', '$', nullptr, nullptr},
    {'5', '5', '%', nullptr, nullptr},  {'6', '6', '^', nullptr, nullptr},
    {'7', '7', '&', nullptr, nullptr},  {'8', '8', '*', nullptr, nullptr},
    {'9', '9', '(', nullptr, nullptr},  {'0', '0', ')', nullptr, nullptr},
    {0, 0, 0, "(none)", ""},
    // --- 行 1 : index 11-21 ---
    {'q', 'Q', '~', nullptr, nullptr},  {'w', 'W', '`', nullptr, nullptr},
    {'e', 'E', '?', nullptr, nullptr},  {'r', 'R', '\\', nullptr, nullptr},
    {'t', 'T', '/', nullptr, nullptr},  {'y', 'Y', '|', nullptr, nullptr},
    {'u', 'U', '_', nullptr, nullptr},  {'i', 'I', '-', nullptr, nullptr},
    {'o', 'O', '+', nullptr, nullptr},  {'p', 'P', '=', nullptr, nullptr},
    {0x08, 0x08, 0x08, "Delete", "Del"},
    // --- 行 2 : index 22-32 ---
    {0, 0, 0, "Aa", "Aa"},
    {'a', 'A', '{', nullptr, nullptr},  {'s', 'S', '}', nullptr, nullptr},
    {'d', 'D', '^', nullptr, nullptr},  {'f', 'F', '[', nullptr, nullptr},
    {'g', 'G', ']', nullptr, nullptr},  {'h', 'H', '"', nullptr, nullptr},
    {'j', 'J', '\'', nullptr, nullptr}, {'k', 'K', ';', nullptr, nullptr},
    {'l', 'L', ':', nullptr, nullptr},
    {0x0A, 0x0A, 0x0A, "Enter", "Ent"},
    // --- 行 3 : index 33-43 ---
    {0, 0, 0, "Fn", "Fn"},
    {0, 0, 0, "Sym", "Sym"},
    {'z', 'Z', 'Z', nullptr, nullptr},  {'x', 'X', 'X', nullptr, nullptr},
    {'c', 'C', 'C', nullptr, nullptr},  {'v', 'V', '<', nullptr, nullptr},
    {'b', 'B', '>', nullptr, nullptr},  {'n', 'N', ',', nullptr, nullptr},
    {'m', 'M', '.', nullptr, nullptr},
    {0x20, 0x20, 0x20, "Space", "Spc"},
    {0, 0, 0, "(none)", ""},
};

static const uint8_t KEY_AA  = 22;
static const uint8_t KEY_FN  = 33;
static const uint8_t KEY_SYM = 34;

// ---- 受信イベントのリングバッファ (コールバック内では描画しない) ------------
struct RawEvent {
  uint8_t mac[6];
  uint8_t data[5];
  uint8_t len;
};

static const uint8_t     QUEUE_SIZE = 32;
static volatile RawEvent g_queue[QUEUE_SIZE];
static volatile uint8_t  g_qHead = 0;   // 書き込み位置 (コールバック)
static volatile uint8_t  g_qTail = 0;   // 読み出し位置 (loop)
static volatile uint32_t g_rxCount = 0;

// ---- 修飾キーの状態 ---------------------------------------------------------
static bool     g_capsLock   = false;  // Aa ダブルクリックによるロック
static bool     g_capsOnce   = false;  // Aa シングルクリックによる 1 文字だけ大文字
static bool     g_aaHeld     = false;  // Aa 長押し中
static bool     g_symMode    = false;  // Sym モード
static bool     g_fnHeld     = false;  // Fn 押下中
static uint32_t g_aaPressMs  = 0;
static uint32_t g_aaClickMs  = 0;

static uint8_t  g_channel    = ESPNOW_CHANNEL;

// ---- 入力テキストバッファ ---------------------------------------------------
static char g_txt[TXT_ROWS][TXT_COLS + 1];
static int  g_txtLen[TXT_ROWS];
static int  g_curRow = 0, g_curCol = 0;

// =============================================================================
//  ESP-NOW 受信コールバック
// =============================================================================
static void pushEvent(const uint8_t *mac, const uint8_t *data, int len)
{
  uint8_t next = (uint8_t)((g_qHead + 1) % QUEUE_SIZE);
  if (next == g_qTail) return;  // あふれたら捨てる
  RawEvent *e = (RawEvent *)&g_queue[g_qHead];
  memcpy(e->mac, mac, 6);
  if (len > (int)sizeof(e->data)) len = sizeof(e->data);
  memcpy(e->data, data, len);
  e->len   = (uint8_t)len;
  g_qHead  = next;
  g_rxCount++;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  pushEvent(info->src_addr, data, len);
}
#else
static void onDataRecv(const uint8_t *mac, const uint8_t *data, int len)
{
  pushEvent(mac, data, len);
}
#endif

// =============================================================================
//  LCD 描画
// =============================================================================

// --- ヘッダ帯 (タイトル / チャンネル / 受信数) ---
static void lcdDrawHeader()
{
  lcd.fillRect(0, 0, SCR_W, HDR_H, TFT_NAVY);
  lcd.setTextFont(2);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  lcd.setCursor(6, 3);
  lcd.print("Unit CardKB2  ESP-NOW receiver");
  lcd.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  lcd.setCursor(330, 3);
  lcd.printf("CH:%-2u  RX:%-6lu", g_channel, (unsigned long)g_rxCount);
}

// --- キーマトリクスの 1 セル ---
static void lcdDrawCell(uint8_t id, bool pressed)
{
  const KeyDef &k = KEYMAP[id];
  int x = GRID_X + (id % 11) * CELL_W;
  int y = GRID_Y + (id / 11) * CELL_H;

  const bool special = (k.name != nullptr);
  uint16_t   bg      = pressed ? TFT_ORANGE : (special ? 0x2124 /*濃灰*/ : TFT_BLACK);
  uint16_t   fg      = pressed ? TFT_BLACK : (special ? TFT_CYAN : TFT_WHITE);

  lcd.fillRect(x, y, CELL_W - 1, CELL_H - 1, bg);
  lcd.drawRect(x, y, CELL_W - 1, CELL_H - 1, TFT_DARKGREY);

  char s[8];
  if (k.label) {
    strncpy(s, k.label, sizeof(s) - 1);
    s[sizeof(s) - 1] = '\0';
  } else {
    s[0] = g_symMode ? k.sym : ((g_capsLock || g_capsOnce || g_aaHeld) ? k.caps : k.normal);
    s[1] = '\0';
  }
  if (s[0]) {
    lcd.setTextFont(2);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(fg);
    lcd.drawString(s, x + (CELL_W - 1) / 2, y + (CELL_H - 1) / 2);
    lcd.setTextDatum(top_left);
  }
}

// --- キーマトリクス全体 (モード切替でラベルが変わるので作り直す) ---
static void lcdDrawGrid()
{
  for (uint8_t id = 0; id < 44; id++) lcdDrawCell(id, false);
}

// --- CAPS / SYM / FN の状態チップ ---
static void lcdDrawModifiers()
{
  struct { const char *s; int x, w; bool on; } chip[] = {
      {"CAPS", 292, 62, (g_capsLock || g_capsOnce || g_aaHeld)},
      {"SYM",  360, 54, g_symMode},
      {"FN",   420, 50, g_fnHeld},
  };
  lcd.setTextFont(2);
  for (auto &c : chip) {
    uint16_t bg = c.on ? TFT_GREEN : 0x2124;
    lcd.fillRoundRect(c.x, INFO_Y, c.w, INFO_H - 4, 4, bg);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(c.on ? TFT_BLACK : TFT_DARKGREY);
    lcd.drawString(c.s, c.x + c.w / 2, INFO_Y + (INFO_H - 4) / 2);
    lcd.setTextDatum(top_left);
  }
  // CAPS ロック中は枠を付けて一時大文字と区別する
  if (g_capsLock) lcd.drawRoundRect(292, INFO_Y, 62, INFO_H - 4, 4, TFT_RED);
}

// --- 最後に押されたキーの情報行 ---
static void lcdDrawLastKey(uint8_t keyId, uint8_t state, uint8_t ch)
{
  const KeyDef &k = KEYMAP[keyId];
  lcd.fillRect(0, INFO_Y, 288, INFO_H, TFT_BLACK);
  lcd.setTextFont(4);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(state == 0x01 ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
  lcd.setCursor(6, INFO_Y + 1);

  char disp[12];
  if (k.name)             snprintf(disp, sizeof(disp), "%s", k.name);
  else if (ch >= 0x21 && ch <= 0x7E) snprintf(disp, sizeof(disp), "'%c'", ch);
  else                    snprintf(disp, sizeof(disp), "0x%02X", ch);

  lcd.printf("id%-3u r%uc%-2u %-6s %s", keyId, keyId / 11, keyId % 11,
             (state == 0x01) ? "PRESS" : "REL", disp);
}

// --- 入力テキストエリア ---
static void lcdDrawCaret(bool on)
{
  int x = TXT_X + g_curCol * CH_W;
  int y = TXT_Y + g_curRow * CH_H;
  lcd.fillRect(x, y + CH_H - 3, CH_W - 1, 2, on ? TFT_GREEN : TFT_BLACK);
}

static void lcdRedrawText()
{
  lcd.fillRect(TXT_X, TXT_Y, TXT_W, TXT_H, TFT_BLACK);
  lcd.setFont(&fonts::FreeMono12pt7b);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int r = 0; r < TXT_ROWS; r++) {
    if (g_txtLen[r] == 0) continue;
    lcd.setCursor(TXT_X, TXT_Y + r * CH_H);
    lcd.print(g_txt[r]);
  }
  lcd.setTextFont(2);
  lcdDrawCaret(true);
}

static void txtNewLine()
{
  lcdDrawCaret(false);
  if (g_curRow + 1 >= TXT_ROWS) {
    // 1 行スクロール
    for (int r = 0; r < TXT_ROWS - 1; r++) {
      memcpy(g_txt[r], g_txt[r + 1], TXT_COLS + 1);
      g_txtLen[r] = g_txtLen[r + 1];
    }
    g_txt[TXT_ROWS - 1][0] = '\0';
    g_txtLen[TXT_ROWS - 1] = 0;
    g_curCol = 0;
    lcdRedrawText();
  } else {
    g_curRow++;
    g_curCol = 0;
    lcdDrawCaret(true);
  }
}

static void txtPutChar(uint8_t ch)
{
  if (ch == 0x0A) { txtNewLine(); return; }

  if (ch == 0x08) {  // Delete (BS)
    lcdDrawCaret(false);
    if (g_curCol > 0) {
      g_curCol--;
      g_txt[g_curRow][g_curCol] = '\0';
      g_txtLen[g_curRow]        = g_curCol;
      lcd.fillRect(TXT_X + g_curCol * CH_W, TXT_Y + g_curRow * CH_H, CH_W, CH_H, TFT_BLACK);
    } else if (g_curRow > 0) {
      g_curRow--;
      g_curCol = g_txtLen[g_curRow];
    }
    lcdDrawCaret(true);
    return;
  }

  if (ch < 0x20 || ch > 0x7E) return;  // 表示できない文字は無視

  if (g_curCol >= TXT_COLS) txtNewLine();  // 折り返し

  lcdDrawCaret(false);
  g_txt[g_curRow][g_curCol]     = (char)ch;
  g_txt[g_curRow][g_curCol + 1] = '\0';
  g_txtLen[g_curRow]            = g_curCol + 1;

  lcd.setFont(&fonts::FreeMono12pt7b);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(TXT_X + g_curCol * CH_W, TXT_Y + g_curRow * CH_H);
  lcd.write(ch);
  lcd.setTextFont(2);

  g_curCol++;
  lcdDrawCaret(true);
}

static void lcdDrawStatic()
{
  lcd.fillScreen(TFT_BLACK);
  lcdDrawHeader();
  lcdDrawGrid();
  lcdDrawModifiers();
  lcd.drawRect(TXT_X - 4, TXT_Y - 4, TXT_W + 8, TXT_H + 8, TFT_DARKGREY);
  lcdRedrawText();
}

// =============================================================================
//  シリアル表示ヘルパ
// =============================================================================
static void printCharLiteral(uint8_t ch)
{
  switch (ch) {
    case 0x08: Serial.print("'\\b'"); break;
    case 0x0A: Serial.print("'\\n'"); break;
    case 0x20: Serial.print("' '");   break;
    default:
      if (ch >= 0x21 && ch <= 0x7E) Serial.printf("'%c'", ch);
      else                          Serial.print("---");
      break;
  }
}

static void printModifiers()
{
  Serial.printf("  [%s%s%s]",
                (g_capsLock ? "CAPSLOCK " : (g_capsOnce || g_aaHeld ? "CAPS " : "")),
                (g_symMode ? "SYM " : ""),
                (g_fnHeld ? "FN" : ""));
}

// =============================================================================
//  キーイベント 1 件の処理
// =============================================================================
static void handleKey(uint8_t keyId, uint8_t state)
{
  const bool pressed = (state == 0x01);
  const KeyDef &k    = KEYMAP[keyId];

  bool modeChanged = false;

  // --- 修飾キー ---
  if (keyId == KEY_AA) {
    if (pressed) {
      if (!g_aaHeld) {          // リピートではない最初の押下
        g_aaHeld    = true;
        g_aaPressMs = millis();
        modeChanged = true;
      }
    } else {
      uint32_t now  = millis();
      uint32_t held = now - g_aaPressMs;
      g_aaHeld      = false;
      if (held < 300) {         // クリックとみなす
        if (g_capsLock) {                          // ロック解除
          g_capsLock = false;
          g_capsOnce = false;
        } else if (now - g_aaClickMs < 400) {      // ダブルクリック → ロック
          g_capsLock = true;
          g_capsOnce = false;
        } else {                                   // シングルクリック → 1 文字だけ
          g_capsOnce = !g_capsOnce;
        }
        g_aaClickMs = now;
      }
      modeChanged = true;
    }
  } else if (keyId == KEY_SYM) {
    if (pressed && !g_fnHeld) { g_symMode = !g_symMode; modeChanged = true; }
  } else if (keyId == KEY_FN) {
    g_fnHeld    = pressed;
    modeChanged = true;
  }

  // --- 押下中のキーをマトリクス上でハイライト ---
  lcdDrawCell(keyId, pressed);

  if (keyId == KEY_AA || keyId == KEY_SYM || keyId == KEY_FN) {
    if (modeChanged) {
      lcdDrawGrid();               // ラベル (通常/大文字/記号) を貼り直す
      lcdDrawCell(keyId, pressed);
      lcdDrawModifiers();
    }
    return;
  }

  if (!pressed) {
    lcdDrawLastKey(keyId, state, 0);
    return;
  }

  // --- 文字キー ---
  // Sym モード中は Aa は無効 (マニュアル Table 5 の注記)
  const bool upper = !g_symMode && (g_capsLock || g_capsOnce || g_aaHeld);
  uint8_t    ch    = g_symMode ? k.sym : (upper ? k.caps : k.normal);

  if (k.name) {
    Serial.printf("  %s", k.name);
  } else {
    Serial.print("  ");
    printCharLiteral(ch);
    Serial.printf(" (0x%02X)", ch);
  }

  lcdDrawLastKey(keyId, state, ch);
  if (ch) txtPutChar(ch);

  if (!k.name && g_capsOnce) {     // ワンショット大文字を消費
    g_capsOnce = false;
    lcdDrawGrid();
    lcdDrawCell(keyId, true);
    lcdDrawModifiers();
  }

#if ECHO_TEXT
  if (ch) {
    static bool needHeader = true;
    if (needHeader) { Serial.print("\nTEXT> "); needHeader = false; }
    if (ch == 0x0A)      { Serial.print("\nTEXT> "); }
    else if (ch == 0x08) { Serial.print("\b \b"); }
    else                 { Serial.write(ch); }
  }
#endif
}

// =============================================================================
//  1 パケットの検証と表示
// =============================================================================
// ALLOWED_MAC が全て 0 なら制限なし、そうでなければ一致した送信元のみ受け入れる
static bool macAllowed(const uint8_t *mac)
{
  bool unset = true;
  for (int i = 0; i < 6; i++) {
    if (ALLOWED_MAC[i]) { unset = false; break; }
  }
  return unset || (memcmp(mac, ALLOWED_MAC, 6) == 0);
}

static void processPacket(const RawEvent &e)
{
  Serial.printf("\n[%02X:%02X:%02X:%02X:%02X:%02X] RAW:", e.mac[0], e.mac[1],
                e.mac[2], e.mac[3], e.mac[4], e.mac[5]);
  for (uint8_t i = 0; i < e.len; i++) Serial.printf(" %02X", e.data[i]);

  if (!macAllowed(e.mac)) { Serial.print("  -> IGNORED: sender MAC"); return; }
  if (e.len != 5)          { Serial.print("  -> ERR: length");   return; }
  if (e.data[0] != 0xAA)   { Serial.print("  -> ERR: header");   return; }
  if (e.data[1] != 0x03)   { Serial.print("  -> ERR: data_len"); return; }

  uint8_t keyId = e.data[2];
  uint8_t state = e.data[3];
  uint8_t sum   = (uint8_t)((e.data[1] + e.data[2] + e.data[3]) & 0xFF);
  if (sum != e.data[4]) {
    Serial.printf("  -> ERR: checksum recv=%02X calc=%02X", e.data[4], sum);
    return;
  }
  if (keyId > 43) { Serial.printf("  -> ERR: key_id %u", keyId); return; }
  if (state != 0x01 && state != 0x02) {
    Serial.printf("  -> ERR: key_state %02X", state);
    return;
  }

  Serial.printf("  id=%2u (r%u,c%-2u) %-7s", keyId, keyId / 11, keyId % 11,
                (state == 0x01) ? "PRESS" : "RELEASE");
  handleKey(keyId, state);
  printModifiers();
}

// =============================================================================
void setup()
{
  Serial.begin(115200);

  // --- LCD 初期化 & 起動画面 ---
  lcd.init();
  lcd.setRotation(1);   // 画面向き (USB位置基準 0:下 / 1:右 / 2:上 / 3:左)
  lcd.setTextSize(1);
  lcd.fillScreen(TFT_BLACK);

  lcd.setFont(&fonts::lgfxJapanGothicP_24);
  lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  lcd.setCursor(20, 60);
  lcd.println("Unit CardKB2 ESP-NOW receiver");
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(20, 110);
  lcd.println("CardKB2 で Fn + Sym + 3 を押して");
  lcd.setCursor(20, 145);
  lcd.println("ESP-NOW モードにしてください");

  delay(2000);  // USB CDC が列挙されるのを待つ

  Serial.println("\n================================================");
  Serial.println(" Unit CardKB2 - ESP-NOW receiver (piyopiyo-pcb v1.6)");
  Serial.println(" CardKB2 側で Fn + Sym + 3 を押して ESP-NOW モードに");
  Serial.println("================================================");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);  // AP には繋がない (ESP-NOW のみ)
  Serial.printf("Receiver MAC : %s\n", WiFi.macAddress().c_str());

  lcd.setFont(&fonts::lgfxJapanGothicP_24);
  lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  lcd.setCursor(20, 200);
  lcd.printf("MAC : %s", WiFi.macAddress().c_str());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: esp_now_init() failed");
    lcd.setTextColor(TFT_RED, TFT_BLACK);
    lcd.setCursor(20, 240);
    lcd.print("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);

  esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
  Serial.printf("Listening on channel %d ...\n", g_channel);

  delay(1200);

  // --- メイン画面へ ---
  for (int r = 0; r < TXT_ROWS; r++) { g_txt[r][0] = '\0'; g_txtLen[r] = 0; }
  lcdDrawStatic();
}

void loop()
{
  // --- 受信キューの消化 ---
  bool got = false;
  while (g_qTail != g_qHead) {
    RawEvent e;
    memcpy(&e, (const void *)&g_queue[g_qTail], sizeof(e));
    g_qTail = (uint8_t)((g_qTail + 1) % QUEUE_SIZE);
    processPacket(e);
    got = true;
  }
  if (got) lcdDrawHeader();   // 受信カウンタ更新

#if CHANNEL_HUNT
  // --- 一度も受信できていない間はチャンネルを巡回して CardKB2 を探す ---
  static uint32_t lastHunt = 0;
  if (g_rxCount == 0 && millis() - lastHunt > CHANNEL_HUNT_MS) {
    lastHunt  = millis();
    g_channel = (uint8_t)((g_channel % 13) + 1);
    esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("no packet yet, trying channel %u ...\n", g_channel);
    lcdDrawHeader();
  }
#endif

  delay(1);
}
