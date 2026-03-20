#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

#define MYCON_KEY_COUNT     14
#define MYCON_RECV_TIMEOUT  500   // ms
#define MYCON_DEFAULT_CHANNEL 1

// キー対応テーブル（新仕様：C→X, D→Y）
static const char key_letter[MYCON_KEY_COUNT] = {
    'U','D','L','R', 'A','B','X','Y', 'L','l','R','r', 'E','T'
};

// インデックス（UDP版と互換を保ちつつ、C/DをX/Yに変更）
enum key_index {
    key_Upward = 0,
    key_Downward,
    key_Left,
    key_Right,
    key_A,
    key_B,
    key_X,
    key_Y,
    key_L1,
    key_L2,
    key_R1,
    key_R2,
    key_Select,
    key_Start
    // key_heartbeat は内部処理で使用（配列外）
};

struct GamepadState {
    bool     keys[MYCON_KEY_COUNT] = {false};
    int16_t  joy1_x = 0;
    int16_t  joy1_y = 0;
    int16_t  joy2_x = 0;
    int16_t  joy2_y = 0;
    uint32_t last_recv_ms = 0;
    bool     paired = false;
};

class MyconReceiverBT {
public:
    MyconReceiverBT(int channel = MYCON_DEFAULT_CHANNEL)
        : _channel(channel) {
        _state.last_recv_ms = 0;
        _debug_output = false;
    }

    ~MyconReceiverBT() {
        end();
    }

    void begin() {
        WiFi.mode(WIFI_STA);

        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);

        if (esp_now_init() != ESP_OK) {
            Serial.println("ESP-NOW init failed");
            return;
        }

        esp_now_register_recv_cb(onDataRecvStatic);
        esp_now_register_send_cb(onDataSentStatic);

        // ラジコン用途向けの安定性設定
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);
        esp_wifi_set_max_tx_power(78);  // 19.5dBm
    }

    void end() {
        esp_now_deinit();
    }

    void set_debug_output(bool enable) {
        _debug_output = enable;
    }

    // キー押下判定
    bool is_key_down(int index) const {
        if (index < 0 || index >= MYCON_KEY_COUNT) return false;
        return _state.keys[index];
    }

    // ジョイスティック値取得
    int16_t get_joy1_x() const { return _state.joy1_x; }
    int16_t get_joy1_y() const { return _state.joy1_y; }
    int16_t get_joy2_x() const { return _state.joy2_x; }
    int16_t get_joy2_y() const { return _state.joy2_y; }

    uint32_t time_since_last_recv() const {
        return millis() - _state.last_recv_ms;
    }

    bool is_timeout() const {
        return time_since_last_recv() > MYCON_RECV_TIMEOUT;
    }

    bool is_paired() const {
        return _state.paired;
    }

    const uint8_t* get_peer_mac() const {
        return _peer_mac;
    }

    static MyconReceiverBT* _instance;

private:
    GamepadState _state;
    uint8_t      _peer_mac[6]      = {0};
    bool         _debug_output     = false;
    bool         _peer_registered  = false;
    int          _channel;

    static void onDataRecvStatic(const uint8_t *mac, const uint8_t *data, int len) {
        if (_instance) {
            _instance->onDataRecv(mac, data, len);
        }
    }

    static void onDataSentStatic(const uint8_t *mac_addr, esp_now_send_status_t status) {
        if (_instance && _instance->_debug_output) {
            Serial.println(status == ESP_NOW_SEND_SUCCESS ? "[ESPNOW] Send OK" : "[ESPNOW] Send FAIL");
        }
    }

    void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
        if (len == 0) return;

        // デバッグ出力
        if (_debug_output) {
            Serial.print("[RX] from ");
            for (int i = 0; i < 6; i++) {
                Serial.printf("%02X%s", mac[i], i<5 ? ":" : " ");
            }
            Serial.print(" len=");
            Serial.print(len);
            Serial.print(" \"");
            for (int i = 0; i < len; i++) {
                char c = (char)data[i];
                if (c >= 32 && c <= 126) Serial.print(c);
                else                     Serial.printf("\\x%02X", (uint8_t)c);
            }
            Serial.println("\"");
        }

        // ペアリング要求（ブロードキャスト期待メッセージ）
        static const char expected[] = "______________________________H"; // 31バイト
        if (len == 31 && memcmp(data, expected, 31) == 0) {
            memcpy(_peer_mac, mac, 6);

            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, mac, 6);
            peer.channel = _channel;
            peer.encrypt = false;

            if (!esp_now_is_peer_exist(mac)) {
                esp_now_add_peer(&peer);
                _peer_registered = true;
            }

            uint8_t reply = 'H';
            esp_now_send(mac, &reply, 1);

            _state.paired = true;
            _state.last_recv_ms = millis();

            if (_debug_output) {
                Serial.println("[PAIR] Paired & replied 'H'");
            }
            return;
        }

        // 通常のゲームパッドデータ（期待：14キー + 数値4つ + 'H'）
        if (len < 25) {
            return; // 不正フォーマット
        }

        // キー部分を更新
        for (int i = 0; i < MYCON_KEY_COUNT; i++) {
            _state.keys[i] = (data[i] == key_letter[i]);
        }

        // ジョイスティック部分のパース（例: +123-045+511-512H）
        char buf[32] = {0};
        size_t num_part_len = len - MYCON_KEY_COUNT - 1; // Hの手前まで
        if (num_part_len >= sizeof(buf)) num_part_len = sizeof(buf)-1;
        memcpy(buf, data + MYCON_KEY_COUNT, num_part_len);
        buf[num_part_len] = '\0';

        int j1x = 0, j1y = 0, j2x = 0, j2y = 0;
        if (sscanf(buf, "%d%d%d%d", &j1x, &j1y, &j2x, &j2y) == 4) {
            _state.joy1_x = constrain(j1x, -512, 512);
            _state.joy1_y = constrain(j1y, -512, 512);
            _state.joy2_x = constrain(j2x, -512, 512);
            _state.joy2_y = constrain(j2y, -512, 512);
        }

        _state.last_recv_ms = millis();

        if (!_state.paired) {
            _state.paired = true;
            memcpy(_peer_mac, mac, 6);
        }
    }

    // タイムアウトチェック＆安全リセット（loop内で呼び出し推奨）
public:
    void update() {
        if (is_timeout()) {
            // ラジコン安全のため全データをニュートラルに
            memset(_state.keys, 0, sizeof(_state.keys));
            _state.joy1_x = _state.joy1_y = 0;
            _state.joy2_x = _state.joy2_y = 0;

            // pairedフラグは維持（再接続待ち）
            // 必要ならここで _state.paired = false; も可能
        }
    }
};

// 静的メンバの実体
MyconReceiverBT* MyconReceiverBT::_instance = nullptr;

// グローバルインスタンス登録ヘルパー（必須）
inline void mycon_receiver_global_init(MyconReceiverBT* inst) {
    MyconReceiverBT::_instance = inst;
}