import 'dart:typed_data';

import 'package:pointycastle/export.dart';

/// AES-128-CBC + PKCS7 — khớp firmware continuous stream (SFC1)
class CamCrypto {
  static const defaultKeyHex = '536d6172744661726d43616d4b657921'; // SmartFarmCamKey!
  static const magic = [0x53, 0x46, 0x43, 0x31]; // SFC1

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

  static Uint8List aes128CbcDecrypt(
    Uint8List data,
    Uint8List key,
    Uint8List iv,
  ) {
    if (key.length != 16) throw ArgumentError('AES-128 key must be 16 bytes');
    if (iv.length != 16) throw ArgumentError('IV must be 16 bytes');
    if (data.isEmpty || data.length % 16 != 0) {
      throw StateError('cipher length invalid');
    }

    final cbc = CBCBlockCipher(AESEngine())
      ..init(false, ParametersWithIV(KeyParameter(key), iv));

    final out = Uint8List(data.length);
    var offset = 0;
    while (offset < data.length) {
      offset += cbc.processBlock(data, offset, out, offset);
    }

    final pad = out.last;
    if (pad < 1 || pad > 16) throw StateError('bad PKCS7 pad');
    for (var i = out.length - pad; i < out.length; i++) {
      if (out[i] != pad) throw StateError('PKCS7 mismatch');
    }
    return Uint8List.sublistView(out, 0, out.length - pad);
  }

  /// Packet: SFC1 | w u16le | h u16le | iv[16] | cipher
  static ({int width, int height, Uint8List jpeg}) decodeFramePacket(
    Uint8List packet,
    String aesKeyHex,
  ) {
    if (packet.length < 24) throw StateError('packet too short');
    for (var i = 0; i < 4; i++) {
      if (packet[i] != magic[i]) throw StateError('bad magic');
    }
    final w = packet[4] | (packet[5] << 8);
    final h = packet[6] | (packet[7] << 8);
    final iv = Uint8List.sublistView(packet, 8, 24);
    final cipher = Uint8List.sublistView(packet, 24);
    final key = hexToBytes(aesKeyHex);
    final jpeg = aes128CbcDecrypt(cipher, key, iv);
    if (jpeg.length < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
      throw StateError('not jpeg');
    }
    return (width: w, height: h, jpeg: jpeg);
  }
}
