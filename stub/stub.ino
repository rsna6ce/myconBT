#include "myconBT.h"

MyconReceiverBT receiver;

void setup() {
  Serial.begin(115200);
  receiver.begin();
// receiver.begin(6);  ← チャンネル変更したい場合は引数で指定可能（省略時は1）
  receiver.set_debug_output(false);

  // グローバルにインスタンスを登録（コールバックがstaticのため）
  mycon_receiver_global_init(&receiver);
}

void loop() {
  receiver.update();
  if (receiver.is_timeout()) {
    // タイムアウト処理（必要なら）
  }

  if (receiver.is_paired()) {
    Serial.printf(
      "U:%d D:%d L:%d R:%d A:%d B:%d X:%d Y:%d L1:%d L2:%d R1:%d R2:%d E:%d T:%d  "
      "Joy1:%4d,%4d  Joy2:%4d,%4d\n",
      receiver.is_key_down(key_Upward),
      receiver.is_key_down(key_Downward),
      receiver.is_key_down(key_Left),
      receiver.is_key_down(key_Right),
      receiver.is_key_down(key_A),
      receiver.is_key_down(key_B),
      receiver.is_key_down(key_X),
      receiver.is_key_down(key_Y),
      receiver.is_key_down(key_L1),
      receiver.is_key_down(key_L2),
      receiver.is_key_down(key_R1),
      receiver.is_key_down(key_R2),
      receiver.is_key_down(key_Select),
      receiver.is_key_down(key_Start),
      receiver.get_joy1_x(), receiver.get_joy1_y(),
      receiver.get_joy2_x(), receiver.get_joy2_y()
    );
  }

  delay(100);
}