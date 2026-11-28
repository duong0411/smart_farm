import 'dart:convert';
import 'dart:typed_data';

import 'package:pointycastle/export.dart';

/// AES-128-CBC + PKCS7 — khớp firmware ESP32_CAM_MqttStream
class CamCrypto {
  static const defaultKeyHex = '536d6172744661726d43616d4b657921'; // SmartFarmCamKey!

  static Uint8List hexToBytes(String hex) {
    final cleaned = hex.replaceAll(RegExp(r'\s+'), '');
    if (cleaned.length.isOdd) {
      throw FormatException('hex length odd');
    }
    final out = Uint8List(cleaned.length ~/ 2);
    for (var i = 0; i < out.length; i++) {
      out[i] = int.parse(cleaned.substring(i * 2, i * 2 + 2), radix: 16);
    }
    return out;
  }

  static Uint8List decryptJpeg({
    required String ivHex,
    required String base64Cipher,
    required String aesKeyHex,
  }) {
    final key = hexToBytes(aesKeyHex);
    final iv = hexToBytes(ivHex);
    if (key.length != 16) {
      throw ArgumentError('AES-128 key must be 16 bytes');
    }
    if (iv.length != 16) {
      throw ArgumentError('IV must be 16 bytes');
    }

    final cipherBytes = base64Decode(base64Cipher);
    if (cipherBytes.isEmpty || cipherBytes.length % 16 != 0) {
      throw StateError('cipher length invalid');
    }

    final cbc = CBCBlockCipher(AESEngine())
      ..init(false, ParametersWithIV(KeyParameter(key), iv));

    final out = Uint8List(cipherBytes.length);
    var offset = 0;
    while (offset < cipherBytes.length) {
      offset += cbc.processBlock(cipherBytes, offset, out, offset);
    }

    final pad = out.last;
    if (pad < 1 || pad > 16) {
      throw StateError('bad PKCS7 pad');
    }
    for (var i = out.length - pad; i < out.length; i++) {
      if (out[i] != pad) throw StateError('PKCS7 mismatch');
    }
    return Uint8List.sublistView(out, 0, out.length - pad);
  }
}
