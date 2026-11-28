import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'cam_crypto.dart';
import 'mqtt_service.dart';

/// Continuous stream: tele/{chip}_cam/frame (binary SFC1) → AES → JPEG
class CamStreamService extends ChangeNotifier {
  static const _aesKeyPref = 'sf_cam_aes_hex';

  MqttService? _mqtt;
  String _aesKeyHex = CamCrypto.defaultKeyHex;
  Uint8List? _jpeg;
  int _width = 0;
  int _height = 0;
  bool _camOnline = false;
  bool _streamOn = true;
  String? _error;
  DateTime? _lastFrameAt;
  int _framesReceived = 0;

  String get aesKeyHex => _aesKeyHex;
  Uint8List? get jpeg => _jpeg;
  int get width => _width;
  int get height => _height;
  bool get camOnline => _camOnline;
  bool get streamOn => _streamOn;
  bool get assembling => false;
  String? get error => _error;
  DateTime? get lastFrameAt => _lastFrameAt;
  int get framesReceived => _framesReceived;
  bool get hasFrame => _jpeg != null && _jpeg!.isNotEmpty;

  Future<void> load() async {
    final prefs = await SharedPreferences.getInstance();
    _aesKeyHex = prefs.getString(_aesKeyPref) ?? CamCrypto.defaultKeyHex;
    notifyListeners();
  }

  void attachMqtt(MqttService mqtt) {
    _mqtt = mqtt;
  }

  Future<void> setAesKeyHex(String hex) async {
    final cleaned = hex.replaceAll(RegExp(r'\s+'), '');
    if (cleaned.length != 32) {
      _error = 'AES key phải 32 ký tự hex';
      notifyListeners();
      return;
    }
    try {
      CamCrypto.hexToBytes(cleaned);
    } catch (_) {
      _error = 'AES key hex không hợp lệ';
      notifyListeners();
      return;
    }
    _aesKeyHex = cleaned;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_aesKeyPref, cleaned);
    _error = null;
    notifyListeners();
  }

  String get _chip => _mqtt?.chipId ?? '';

  void setStream(bool on) {
    final id = _chip;
    if (id.isEmpty) return;
    _streamOn = on;
    _mqtt?.publish('cmnd/${id}_cam/STREAM', on ? 'ON' : 'OFF');
    notifyListeners();
  }

  void snap() {
    final id = _chip;
    if (id.isEmpty) return;
    _mqtt?.publish('cmnd/${id}_cam/SNAP', '1');
  }

  void handleMqtt(String topic, String payload) {
    final id = _chip;
    if (id.isEmpty) return;
    final prefix = 'tele/${id}_cam/';
    if (!topic.startsWith(prefix)) return;
    final leaf = topic.substring(prefix.length);
    if (leaf == 'status') {
      _camOnline = payload.trim().toLowerCase() == 'online';
      notifyListeners();
    }
  }

  void handleFrameBinary(Uint8List bytes) {
    try {
      final decoded = CamCrypto.decodeFramePacket(bytes, _aesKeyHex);
      _jpeg = decoded.jpeg;
      _width = decoded.width;
      _height = decoded.height;
      _lastFrameAt = DateTime.now();
      _framesReceived++;
      _camOnline = true;
      _error = null;
      notifyListeners();
    } catch (e) {
      _error = 'Decode frame: $e';
      if (kDebugMode) debugPrint(_error);
      notifyListeners();
    }
  }
}
