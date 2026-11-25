import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../models/farm_state.dart';

/// Khớp firmware SmartFarm_ESP32.ino (WSS MQTT) — CHIP_ID mặc định "123"
class MqttConfig {
  static const host = 'mqtt.duynguyen.io.vn';
  static const port = 443;
  static const path = '/mqtt';
  /// mqtt_client cần full WSS URL (giống AloT / face_alot)
  static const wssUrl = 'wss://mqtt.duynguyen.io.vn/mqtt';
}

class MqttService extends ChangeNotifier {
  static const _chipKey = 'sf_chip_id';

  MqttServerClient? _client;
  String _chipId = '';
  FarmState _state = const FarmState();
  String? _error;
  bool _connecting = false;

  String get chipId => _chipId;
  FarmState get state => _state;
  String? get error => _error;
  bool get connecting => _connecting;
  bool get isConnected =>
      _client?.connectionStatus?.state == MqttConnectionState.connected;

  Future<void> loadChipId() async {
    final prefs = await SharedPreferences.getInstance();
    _chipId = prefs.getString(_chipKey) ?? '';
    if (_chipId.isNotEmpty) {
      _state = _state.copyWith(chipId: _chipId);
      notifyListeners();
    }
  }

  Future<void> setChipId(String id) async {
    final cleaned = id.trim();
    if (cleaned.isEmpty) return;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_chipKey, cleaned);
    _chipId = cleaned;
    _state = FarmState(chipId: cleaned, mqttConnected: isConnected);
    notifyListeners();
    await reconnect();
  }

  Future<void> clearChipId() async {
    await disconnect();
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_chipKey);
    _chipId = '';
    _state = const FarmState();
    notifyListeners();
  }

  Future<void> connect() async {
    if (_chipId.isEmpty || _connecting) return;
    if (isConnected) return;

    _connecting = true;
    _error = null;
    notifyListeners();

    final clientId =
        'SF-App-${DateTime.now().millisecondsSinceEpoch}-${Random().nextInt(9999)}';

    // QUAN TRỌNG: truyền wss://host/mqtt — không chỉ hostname
    final client = MqttServerClient.withPort(
      MqttConfig.wssUrl,
      clientId,
      MqttConfig.port,
    );
    client.useWebSocket = true;
    client.secure = true;
    client.websocketProtocols = MqttClientConstants.protocolsSingleDefault;
    client.logging(on: kDebugMode);
    client.keepAlivePeriod = 60;
    client.connectTimeoutPeriod = 15000;
    client.autoReconnect = true;
    client.resubscribeOnAutoReconnect = true;
    client.onConnected = _onConnected;
    client.onDisconnected = _onDisconnected;
    client.onAutoReconnect = () {
      _connecting = true;
      notifyListeners();
    };
    client.onAutoReconnected = () {
      _onConnected();
      _subscribeAll();
    };

    final connMsg = MqttConnectMessage()
        .withClientIdentifier(clientId)
        .startClean()
        .withWillTopic('tele/SF-App/status')
        .withWillMessage('offline')
        .withWillQos(MqttQos.atLeastOnce);
    client.connectionMessage = connMsg;

    _client = client;

    try {
      await client.connect().timeout(const Duration(seconds: 20));
      if (client.connectionStatus?.state != MqttConnectionState.connected) {
        throw Exception(
          client.connectionStatus?.returnCode.toString() ?? 'CONNACK missing',
        );
      }
      _subscribeAll();
      client.updates?.listen(_onMessage);
      publish('cmnd/${_chipId}_rules/GET', 'GET');
      _error = null;
      if (kDebugMode) {
        debugPrint('MQTT OK → ${MqttConfig.wssUrl} chip=$_chipId');
      }
    } catch (e) {
      _error = 'Không kết nối MQTT: $e';
      if (kDebugMode) debugPrint(_error);
      try {
        client.disconnect();
      } catch (_) {}
      _client = null;
      _state = _state.copyWith(mqttConnected: false);
    } finally {
      _connecting = false;
      notifyListeners();
    }
  }

  Future<void> reconnect() async {
    await disconnect();
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await connect();
  }

  Future<void> disconnect() async {
    try {
      _client?.disconnect();
    } catch (_) {}
    _client = null;
    _state = _state.copyWith(mqttConnected: false, online: false);
    notifyListeners();
  }

  void _onConnected() {
    _connecting = false;
    _error = null;
    _state = _state.copyWith(mqttConnected: true);
    notifyListeners();
  }

  void _onDisconnected() {
    _state = _state.copyWith(mqttConnected: false);
    notifyListeners();
  }

  void _subscribeAll() {
    final c = _client;
    if (c == null || _chipId.isEmpty) return;
    final id = _chipId;
    final topics = [
      'tele/$id/status',
      'tele/${id}_vitals/status',
      'tele/${id}_temp_greenhouse/status',
      'tele/${id}_humi_greenhouse/status',
      'tele/${id}_soil_greenhouse/status',
      'tele/${id}_water_tank/status',
      'tele/${id}_co2_greenhouse/status',
      'tele/${id}_ldr_greenhouse/status',
      'tele/${id}_pump/status',
      'tele/${id}_light/status',
      'tele/${id}_fan1/status',
      'tele/${id}_fan2/status',
      'tele/${id}_mode/status',
      'tele/${id}_rules/status',
    ];
    for (final t in topics) {
      c.subscribe(t, MqttQos.atMostOnce);
    }
  }

  void _onMessage(List<MqttReceivedMessage<MqttMessage>> events) {
    for (final event in events) {
      final msg = event.payload as MqttPublishMessage;
      final topic = event.topic;
      final payload =
          MqttPublishPayload.bytesToStringAsString(msg.payload.message);
      _handle(topic, payload);
    }
  }

  void _handle(String topic, String payload) {
    final id = _chipId;
    final now = DateTime.now();

    if (topic == 'tele/$id/status') {
      final online = payload.trim().toLowerCase() == 'online';
      _state = _state.copyWith(online: online, lastUpdate: now);
      notifyListeners();
      return;
    }

    Map<String, dynamic>? json;
    try {
      if (payload.trim().startsWith('{')) {
        json = jsonDecode(payload) as Map<String, dynamic>;
      }
    } catch (_) {}

    if (topic.endsWith('_vitals/status') && json != null) {
      final vitals = FarmState.fromVitals(json, id);
      _state = vitals.copyWith(
        mqttConnected: true,
        online: true,
      );
      notifyListeners();
      return;
    }

    if (json == null) return;

    if (topic.contains('_temp_greenhouse')) {
      _state = _state.copyWith(
        temp: (json['value'] as num?)?.toDouble() ?? _state.temp,
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_humi_greenhouse')) {
      _state = _state.copyWith(
        humi: (json['value'] as num?)?.toDouble() ?? _state.humi,
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_soil_greenhouse')) {
      _state = _state.copyWith(
        soil: (json['value'] as num?)?.toInt() ?? _state.soil,
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_water_tank')) {
      _state = _state.copyWith(
        water: (json['value'] as num?)?.toInt() ?? _state.water,
        waterEmpty: json['empty'] == 1 || json['empty'] == true,
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_co2_greenhouse')) {
      _state = _state.copyWith(
        co2Raw: (json['raw'] as num?)?.toInt() ?? _state.co2Raw,
        co2Ppm: (json['ppm'] as num?)?.toDouble() ?? _state.co2Ppm,
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_ldr_greenhouse')) {
      final v = json['value']?.toString().toUpperCase();
      _state = _state.copyWith(dark: v == 'DARK', lastUpdate: now, online: true);
    } else if (topic.contains('_pump/status')) {
      _state = _state.copyWith(
        pump: json['value']?.toString().toUpperCase() == 'ON',
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_light/status')) {
      _state = _state.copyWith(
        light: json['value']?.toString().toUpperCase() == 'ON',
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_fan1/status')) {
      _state = _state.copyWith(
        fan1: json['value']?.toString().toUpperCase() == 'ON',
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_fan2/status')) {
      _state = _state.copyWith(
        fan2: json['value']?.toString().toUpperCase() == 'ON',
        lastUpdate: now,
        online: true,
      );
    } else if (topic.contains('_mode/status')) {
      _state = _state.copyWith(
        mode: json['value']?.toString() ?? _state.mode,
        lastUpdate: now,
        online: true,
      );
    }

    notifyListeners();
  }

  void publish(String topic, String payload) {
    final c = _client;
    if (c == null || !isConnected) return;
    final builder = MqttClientPayloadBuilder();
    builder.addString(payload);
    c.publishMessage(topic, MqttQos.atMostOnce, builder.payload!);
  }

  void setPump(bool on) {
    if (!_state.isManual) return;
    publish('cmnd/${_chipId}_pump/POWER', on ? 'ON' : 'OFF');
  }

  void setLight(bool on) {
    if (!_state.isManual) return;
    publish('cmnd/${_chipId}_light/POWER', on ? 'ON' : 'OFF');
  }

  void setFans(bool on) {
    if (!_state.isManual) return;
    publish('cmnd/${_chipId}_fan1/POWER', on ? 'ON' : 'OFF');
  }

  void setMode(bool auto) {
    publish('cmnd/${_chipId}_mode/POWER', auto ? 'AUTO' : 'MANUAL');
  }

  void calibrateCo2() {
    publish('cmnd/${_chipId}_co2_cal/SET', 'CAL');
  }

  @override
  void dispose() {
    disconnect();
    super.dispose();
  }
}
