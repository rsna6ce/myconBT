#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

const char expected[] = "______________________________H";
const int WIFI_CH = 1;

// 送信指示用の構造体
struct SendQueue {
  uint8_t target_mac[6];
  bool pending = false;
} replyQueue;

void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  // 受信処理は極力短く！
  if (len == 31 && memcmp(incomingData, expected, 31) == 0) {
    if (!replyQueue.pending) {
      memcpy(replyQueue.target_mac, mac, 6);
      replyQueue.pending = true; // loopで送信するようにフラグを立てる
    }
  }
  if (len > 0) {
    Serial.print("[RX ASCII] \"");
    
    // lenバイト分だけ出力（ゴミ防止）
    for (int i = 0; i < len; i++) {
      char c = (char)incomingData[i];
      if (c >= 32 && c <= 126) {  // 可視ASCIIのみ（制御文字除外）
        Serial.print(c);
      } else {
        // 制御文字や非ASCIIは16進数でエスケープ表示（デバッグ用）
        Serial.printf("\\x%02X", (unsigned char)c);
      }
    }
    
    Serial.println("\"");
  } else {
    Serial.println("[RX ASCII] (empty packet)");
  }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "送信成功" : "送信失敗");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  // チャンネル設定
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WIFI_CH, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) return;
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);  // 1Mbps固定
  esp_wifi_set_max_tx_power(78);  // 19.5dBm最大
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {
  if (replyQueue.pending) {
    // Peer登録
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, replyQueue.target_mac, 6);
    peerInfo.channel = 0; // 自身のチャンネルと合わせる
    peerInfo.encrypt = false;

    if (!esp_now_is_peer_exist(replyQueue.target_mac)) {
      esp_now_add_peer(&peerInfo);
    }

    // 返信
    uint8_t reply = 'H';
    for(int i=0; i<1; i++) {
      esp_err_t result = esp_now_send(replyQueue.target_mac, &reply, 1);
      if (result != ESP_OK) Serial.printf("Error: %d\n", result);
      delay(200); // loop内ならdelayも安全
    }

    replyQueue.pending = false;
  }
}