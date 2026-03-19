#include <stdint.h>
#include <U8g2lib.h>
#include <Bluepad32.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <esp_now.h>
#include <time.h>
//#include "FS.h"
#include "SPIFFSIni.h"

//static char pin_letter[] = {'U','D','L','R', 'A','B','C','D', 'L','l','R','r', 'E','T', '1','2'};
#define NO_INPUT_WAIT_COUNT_LIMIT 10

const int pinSW = 0;
const int pinLED = 2;
const int pinSTART = 18;
const int pinSELECT = 19;

#define WIRE_FREQ 400*1000 /*fast mode*/
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C
#define PIN_SDA 21
#define PIN_SCL 22
#define SCREEN_LINE(n) ((16*(n))+15)
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

ControllerPtr myControllers[BP32_MAX_GAMEPADS] = {nullptr};

static uint8_t current_target_mac[6] = {0};

// ペアリングモード関連
esp_now_peer_info_t broadcastPeer = {};
bool pairingMode = false;
bool pairingScanning = false;
unsigned long pairingStartTime = 0;
bool broadcastSent = false;  // ワンショット送信フラグ

const int WIFI_CH = 1;
#define MAX_PEERS 16
uint8_t discoveredPeers[MAX_PEERS][6];
int peerCount = 0;
int selectedIndex = 0;

uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

int startPressCount = 0;
int selectPressCount = 0;
const int DEBOUNCE_THRESHOLD = 3;  // 連続LOW回数でデバウンス

void onConnectedController(ControllerPtr ctl) {
  bool found = false;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == nullptr) {
      myControllers[i] = ctl;
      found = true;
      break;
    }
  }
  if (found) {
    Serial.println("=== コントローラ接続されました！ ===");
    Serial.print("モデル: ");
    Serial.println(ctl->getModelName());

    // MACアドレス出力（配列で取得）
    const uint8_t* addr = ctl->getProperties().btaddr;
    Serial.printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
  } else {
    Serial.println("接続スロット満杯！");
  }
}

void onDisconnectedController(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == ctl) {
      myControllers[i] = nullptr;
      break;
    }
  }
  Serial.println("=== コントローラ切断されました！ ===");
}

// byte → 12文字String
String macToStr(const uint8_t mac[6]) {
    char buf[13];
    sprintf(buf, "%02X%02X%02X%02X%02X%02X", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

// 12文字String → byte
bool strToMac(const String& s, uint8_t mac[6]) {
    if (s.length() != 12) return false;
    for (int i = 0; i < 6; i++) {
        mac[i] = strtoul(s.substring(i*2, i*2+2).c_str(), NULL, 16);
    }
    return true;
}

void screen_write_line(int line, char* msg) {
    String smsg = String(msg);
    screen_write_line(line, smsg);
}

void screen_write_line(int line, String smsg) {
    String smsg2 = smsg + "               ";
    u8g2.drawStr(0,SCREEN_LINE(line),(smsg2.substring(0, 16)).c_str());
    u8g2.sendBuffer();
}

int split(String data, char delimiter, String *dst){
    int index = 0;
    int datalength = data.length();
    for (int i = 0; i < datalength; i++) {
        char tmp = data.charAt(i);
        if ( tmp == delimiter ) {
            index++;
        }
        else dst[index] += tmp;
    }
    return (index + 1);
}

// 選択中のMACを表示更新
void updateSelectedMacDisplay() {
    if (peerCount <= 0) return;
    String macStr = macToStr(discoveredPeers[selectedIndex]);
    screen_write_line(1, "NOW>" + macStr);
    Serial.println("Selected MAC: " + macStr);
}

// ESP-NOW受信コールバック
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (!pairingMode) return;
    if (len == 1 && data[0] == 'H') {
        bool exists = false;
        for (int i = 0; i < peerCount; i++) {
            if (memcmp(discoveredPeers[i], mac, 6) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists && peerCount < MAX_PEERS) {
            memcpy(discoveredPeers[peerCount], mac, 6);
            peerCount++;
            Serial.printf("Found peer %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
                          peerCount, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("hello. myconBT.");

    pinMode(pinLED, OUTPUT);
    pinMode(pinSW, INPUT_PULLUP);
    pinMode(pinSTART, INPUT_PULLUP);
    pinMode(pinSELECT, INPUT_PULLUP);

    u8g2.setBusClock(WIRE_FREQ);
    u8g2.begin();
    u8g2.setFlipMode(0);
    u8g2.setContrast(128);
    u8g2.setFont(u8g2_font_8x13B_mf);
    u8g2.clearBuffer();
    screen_write_line(0, "Welcome!!");
    screen_write_line(1, "MyConBT");
    delay(1*1000);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);              // プロミスキャスモードON
    esp_wifi_set_channel(WIFI_CH, WIFI_SECOND_CHAN_NONE);  // チャンネル11に固定
    delay(50);
    esp_wifi_set_promiscuous(false);             // OFFに戻す（ESP-NOWが正常動作）
    WiFi.channel(WIFI_CH);
    Serial.print("Current channel: ");
    Serial.println(WiFi.channel());

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        screen_write_line(1, "NOW: init ERR");
        while(1);
    }
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);  // 1Mbps固定
    esp_wifi_set_max_tx_power(78);  // 19.5dBm最大
    esp_now_register_recv_cb(OnDataRecv);

  memset(&broadcastPeer, 0, sizeof(broadcastPeer));
  memcpy(broadcastPeer.peer_addr, broadcastAddr, 6);  // FF:FF:FF:FF:FF:FF
  broadcastPeer.channel = 0;
  broadcastPeer.encrypt = false;
  if (esp_now_add_peer(&broadcastPeer) != ESP_OK) {
      Serial.println("Broadcast peer add failed on receiver");
  } else {
      Serial.println("Broadcast peer registered OK on receiver");
  }

    // config読み込み
    SPIFFSIni config("/config.ini", true);
    bool hasSavedMac = false;
    if (config.exist("current_target_mac")) {
        String macStr = config.read("current_target_mac");
        if (strToMac(macStr, current_target_mac) && macStr.length() == 12) {
            hasSavedMac = true;
            screen_write_line(1, "NOW:" + macStr);
            Serial.println("Loaded saved MAC: " + macStr);
        }
    }

    if (!hasSavedMac) {
        // 保存MACなし → 自動ペアリングモード開始
        pairingMode = true;
        pairingScanning = true;
        pairingStartTime = millis();
        broadcastSent = false;
        peerCount = 0;
        selectedIndex = 0;
        screen_write_line(1, "NOW> scan...");
        Serial.println("No saved MAC → Auto pairing mode");
    }

    BP32.setup(&onConnectedController, &onDisconnectedController);
    BP32.enableNewBluetoothConnections(true);
    Serial.println("スキャン開始... 8BitDo SN30 Pro を Y + Start 長押しで起動");
}

void loop() {
    BP32.update();

    bool startPressed = (digitalRead(pinSTART) == LOW);
    bool selectPressed = (digitalRead(pinSELECT) == LOW);
    if (startPressed) startPressCount++; else startPressCount = 0;
    if (selectPressed) selectPressCount++; else selectPressCount = 0;

    unsigned long now = millis();

    // START長押し1.5秒でペアリングモード入場（いつでも再入場可）
    if (!pairingMode && startPressCount >= 150) {  // 10ms × 150 = 1500ms
        pairingMode = true;
        pairingScanning = true;
        pairingStartTime = now;
        broadcastSent = false;
        peerCount = 0;
        selectedIndex = 0;
        screen_write_line(1, "NOW> scan...");
        Serial.println("Pairing mode ON (manual)");
        startPressCount = 0;
    }

    if (pairingMode) {
        // ブロードキャストは入場直後に1回だけ
        if (!broadcastSent) {
            char packet[32];
            memset(packet, '_', 30);
            packet[30] = 'H';
            packet[31] = '\0';

            esp_now_send(broadcastAddr, (uint8_t*)packet, 31);
            broadcastSent = true;
            Serial.println("Single broadcast sent");
        }

        // 3秒待機後リスト表示モードへ
        if (now - pairingStartTime >= 3000) {
          if (peerCount == 0 && pairingScanning) {
              screen_write_line(1, "NOW: no device");
          } else if (selectedIndex == 0 && pairingScanning) {  // 初回のみ表示更新
              updateSelectedMacDisplay();
          }
          pairingScanning = false;

          // SELECT押下でインデックス切り替え
          if (selectPressCount >= DEBOUNCE_THRESHOLD && peerCount > 0) {
              selectedIndex = (selectedIndex + 1) % peerCount;
              updateSelectedMacDisplay();
              selectPressCount = 0;
          }

          // START押下で確定・保存
          if (startPressCount >= DEBOUNCE_THRESHOLD) {
              if (peerCount > 0) {
                  memcpy(current_target_mac, discoveredPeers[selectedIndex], 6);
                  SPIFFSIni config("/config.ini", true);
                  config.write("current_target_mac", macToStr(current_target_mac));
                  screen_write_line(1, "NOW:" + macToStr(current_target_mac));
                  Serial.println("Paired & saved: " + macToStr(current_target_mac));
              } else {
                  screen_write_line(1, "NOW: canceled");
              }
              pairingMode = false;
              startPressCount = 0;
          }
        }
    } else {
      // ゲームパッドの入力
      
      //ユニキャスト送信
    }

    delay(10);
}