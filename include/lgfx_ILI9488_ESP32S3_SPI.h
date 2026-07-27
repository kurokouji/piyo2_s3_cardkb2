#pragma once
// ---------------------------------------------
// ハードウェアバージョン切替
// ビルド時に -DHW_VERSION=15 または 16 を指定
// 指定がない場合は v1.6 以降をデフォルトにする
// ---------------------------------------------
#ifndef HW_VERSION
#define HW_VERSION 16
#endif

// =============================================
// v1.5 以前
// =============================================
#if HW_VERSION <= 15

#define TFT_MISO  -1
#define TFT_MOSI  D10
#define TFT_SCLK  D8
#define TFT_CS    D6
#define TFT_DC    D2
#define TFT_RST   D1

#define T_CLK     D8
#define T_DIN     D10
#define T_DO      D7
#define T_CS      D9

#define PIR_PIN   D4    // v1.5: D6はLCD CSのためD4を使用

// =============================================
// v1.6 以降
// =============================================
#elif HW_VERSION >= 16

#define TFT_MISO -1
#define TFT_MOSI D10
#define TFT_SCLK D8
#define TFT_CS   D3
#define TFT_DC   D2
#define TFT_RST  D1

#define T_CLK  D8
#define T_DIN  D10
#define T_DO   D9
#define T_CS   D7

#define PIR_PIN  D6     // v1.6: D6が空きのためPIRに使用

#else
  #error "Unknown HW_VERSION. Use 15 or 16."
#endif
//----------------------------------------------------------------------
// https://github.com/lovyan03/LovyanGFX/blob/master/examples/HowToUse/2_user_setting/2_user_setting.ino
class LGFX : public lgfx::LGFX_Device{
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Touch_XPT2046 _touch_instance;
//----------------------------------------------------------------------  
public:LGFX(void){
  {                            // バス制御の設定を行います。
  auto cfg = _bus_instance.config();// バス設定用の構造体を取得します。
                               // SPIバスの設定
  cfg.spi_host   = SPI2_HOST;  // 使用するSPIを選択 (VSPI_HOST or HSPI_HOST)
  cfg.spi_mode   = 0;          // SPI通信モードを設定 (0 ~ 3)
  cfg.freq_write = 40000000;   // 送信時のSPIクロック(最大80MHz,80MHzを整数割値に丸め)
  cfg.freq_read  = 16000000;   // 受信時のSPIクロック
  cfg.spi_3wire  = false;      // 受信をMOSIピンで行う場合はtrueを設定
  cfg.use_lock   =  true;      // トランザクションロックを使用する場合はtrueを設定
  cfg.dma_channel= 3;          // 使用DMAチャンネル設定(1or2,0=disable)(0=DMA不使用)
  cfg.pin_sclk   = TFT_SCLK;          // SPIのSCLKピン番号を設定
  cfg.pin_mosi   = TFT_MOSI;          // SPIのMOSIピン番号を設定
  cfg.pin_miso   = TFT_MISO;          // SPIのMISOピン番号を設定 (-1 = disable)
  cfg.pin_dc     = TFT_DC;          // SPIのD/C ピン番号を設定 (-1 = disable)
  // SDカードと共通のSPIバスを使う場合、MISOは省略せず必ず設定してください。
  _bus_instance.config(cfg);   // 設定値をバスに反映します。
  _panel_instance.setBus(&_bus_instance);// バスをパネルにセットします。
  }
  {                            // 表示パネル制御の設定を行います。
  auto cfg = _panel_instance.config();// 表示パネル設定用の構造体を取得します。
  cfg.pin_cs          =    TFT_CS; // CS  が接続されているピン番号(-1 = disable)
  cfg.pin_rst         =    TFT_RST; // RST が接続されているピン番号(-1 = disable)
  cfg.pin_busy        =    -1; // BUSYが接続されているピン番号(-1 = disable)
  cfg.memory_width    =   320; // ドライバICがサポートしている最大の幅
  cfg.memory_height   =   480; // ドライバICがサポートしている最大の高さ
  cfg.panel_width     =   320; // 実際に表示可能な幅
  cfg.panel_height    =   480; // 実際に表示可能な高さ
  cfg.offset_x        =     0; // パネルのX方向オフセット量
  cfg.offset_y        =     0; // パネルのY方向オフセット量
  cfg.offset_rotation =     0; // 回転方向の値のオフセット 0~7 (4~7は上下反転)
  cfg.dummy_read_pixel=     8; // ピクセル読出し前のダミーリードのビット数
  cfg.dummy_read_bits =     1; // ピクセル外のデータ読出し前のダミーリードのビット数
  cfg.readable        = false; // データ読出しが可能な場合 trueに設定
  cfg.invert          = false; // パネルの明暗が反転場合 trueに設定
  cfg.rgb_order       = false; // パネルの赤と青が入れ替わる場合 trueに設定 ok
  cfg.dlen_16bit      = false; // データ長16bit単位で送信するパネル trueに設定
  cfg.bus_shared      = false; // SDカードとバスを共有 trueに設定
  _panel_instance.config(cfg);
  }
  { // タッチスクリーン制御の設定を行います。（必要なければ削除）
  auto cfg = _touch_instance.config();
  cfg.x_min      = 208;    // タッチスクリーンから得られる最小のX値(生の値)
  cfg.x_max      = 3905;   // タッチスクリーンから得られる最大のX値(生の値)
  cfg.y_min      = 288;    // タッチスクリーンから得られる最小のY値(生の値)
  cfg.y_max      = 3910;   // タッチスクリーンから得られる最大のY値(生の値)
  cfg.pin_int    = -1;     // INTが接続されているピン番号 : T_IRQ
  cfg.bus_shared = true;   // 画面と共通のバスを使用している場合 trueを設定
  cfg.offset_rotation = 1; // 表示とタッチの向きのが一致しない場合の調整 0~7の値で設定
  // SPI接続の場合
  cfg.spi_host = SPI2_HOST;// 使用するSPIを選択 (HSPI_HOST or VSPI_HOST)
  cfg.freq     = 1000000;  // SPIクロックを設定
  cfg.pin_sclk = T_CLK;        // SCLKが接続されているピン番号 : T_CLK
  cfg.pin_mosi = T_DIN;        // MOSIが接続されているピン番号 : T_DIN
  cfg.pin_miso = T_DO;        // MISOが接続されているピン番号 : T_DO
  cfg.pin_cs   = T_CS;        //   CSが接続されているピン番号 : T_CS
  _touch_instance.config(cfg);
  _panel_instance.setTouch(&_touch_instance);// タッチスクリーンをパネルにセットします。
  }
  setPanel(&_panel_instance);// 使用するパネルをセットします。
  }
};

//=====================================================================
