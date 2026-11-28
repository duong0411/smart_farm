import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'cam_crypto.dart';
import 'mqtt_service.dart';

class _PendingFrame {
  _PendingFrame({
    required this.id,
    required this.ivHex,
    required this.chunks,
    required this.width,
    required this.height,
  });

  final String id;
  final String ivHex;
  final int chunks;
  final int width;
  final int height;
  final Map<int, String> parts = {};
}

/// Nhận meta/part từ ESP32-CAM MQTT → AES decrypt → JPEG
class CamStreamService extends ChangeNotifier {
  static const _aesKeyPref = 'sf_cam_aes_hex';

  MqttService? _mqtt;
  String _aesKeyHex = CamCrypto.defaultKeyHex;
  Uint8List? _jpeg;
  int _width = 0;
  int _height = 0;
  bool _camOnline = false;
  bool _streamOn = true;
  bool _assembling = false;
  String? _error;
  DateTime? _lastFrameAt;
  int _framesReceived = 0;
  _PendingFrame? _pending;

  String get aesKeyHex => _aesKeyHex;
  Uint8List? get jpeg => _jpeg;
  int get width => _width;
  int get height => _height;
  bool get camOnline => _camOnline;
  bool get streamOn => _streamOn;
  bool get assembling => _assembling;
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
      return;
    }

    final json = _tryJson(payload);
    if (json == null) return;

    if (leaf == 'meta') {
      _onMeta(json);
    } else if (leaf == 'part') {
      _onPart(json);
    }
  }

  Map<String, dynamic>? _tryJson(String payload) {
    try {
      final trimmed = payload.trim();
      if (!trimmed.startsWith('{')) return null;
      final decoded = jsonDecode(trimmed);
      if (decoded is Map) return Map<String, dynamic>.from(decoded);
    } catch (_) {}
    return null;
  }

  void _onMeta(Map<String, dynamic> json) {
    final fid = json['id']?.toString() ?? '';
    final iv = json['iv']?.toString() ?? '';
    final chunks = (json['chunks'] as num?)?.toInt() ?? 0;
    if (fid.isEmpty || iv.isEmpty || chunks < 1) return;

    _pending = _PendingFrame(
      id: fid,
      ivHex: iv,
      chunks: chunks,
      width: (json['w'] as num?)?.toInt() ?? 0,
      height: (json['h'] as num?)?.toInt() ?? 0,
    );
    _assembling = true;
    _error = null;
    notifyListeners();
  }

  void _onPart(Map<String, dynamic> json) {
    final pending = _pending;
    if (pending == null) return;
    final fid = json['id']?.toString() ?? '';
    if (fid != pending.id) return;

    final i = (json['i'] as num?)?.toInt() ?? -1;
    final d = json['d']?.toString() ?? '';
    if (i < 0 || d.isEmpty) return;
    pending.parts[i] = d;

    if (pending.parts.length < pending.chunks) {
      _assembling = true;
      notifyListeners();
      return;
    }

    final buf = StringBuffer();
    for (var n = 0; n < pending.chunks; n++) {
      final part = pending.parts[n];
      if (part == null) {
        _error = 'Thiếu chunk $n';
        _assembling = false;
        notifyListeners();
        return;
      }
      buf.write(part);
    }

    try {
      final jpeg = CamCrypto.decryptJpeg(
        ivHex: pending.ivHex,
        base64Cipher: buf.toString(),
        aesKeyHex: _aesKeyHex,
      );
      if (jpeg.length < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        throw StateError('không phải JPEG');
      }
      _jpeg = jpeg;
      _width = pending.width;
      _height = pending.height;
      _lastFrameAt = DateTime.now();
      _framesReceived++;
      _camOnline = true;
      _error = null;
      _pending = null;
      _assembling = false;
      notifyListeners();
    } catch (e) {
      _error = 'Giải mã frame lỗi: $e';
      _assembling = false;
      _pending = null;
      if (kDebugMode) debugPrint(_error);
      notifyListeners();
    }
  }
}
