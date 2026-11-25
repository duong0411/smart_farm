import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

class LeafDetection {
  final String id;
  final String ts;
  final String label;
  final String labelVi;
  final double score;
  final bool alert;
  final double threshold;
  final String imageUrl;
  final String vizUrl;
  final Map<String, double> probs;

  LeafDetection({
    required this.id,
    required this.ts,
    required this.label,
    required this.labelVi,
    required this.score,
    required this.alert,
    required this.threshold,
    required this.imageUrl,
    required this.vizUrl,
    required this.probs,
  });

  factory LeafDetection.fromJson(Map<String, dynamic> j, String base) {
    final probsRaw = (j['probs'] as Map?) ?? {};
    return LeafDetection(
      id: (j['id'] ?? '').toString(),
      ts: (j['ts'] ?? '').toString(),
      label: (j['label'] ?? '').toString(),
      labelVi: (j['label_vi'] ?? j['label'] ?? '').toString(),
      score: (j['score'] as num?)?.toDouble() ?? 0,
      alert: j['alert'] == true,
      threshold: (j['threshold'] as num?)?.toDouble() ?? 0.55,
      imageUrl: _abs(base, (j['image_url'] ?? '').toString()),
      vizUrl: _abs(base, (j['viz_url'] ?? '').toString()),
      probs: probsRaw.map(
        (k, v) => MapEntry(k.toString(), (v as num?)?.toDouble() ?? 0),
      ),
    );
  }

  static String _abs(String base, String path) {
    if (path.isEmpty) return '';
    if (path.startsWith('http')) return path;
    final b = base.endsWith('/') ? base.substring(0, base.length - 1) : base;
    return '$b$path';
  }
}

class LeafService extends ChangeNotifier {
  static const _kBase = 'leaf_api_base';
  static const defaultBase = 'https://api.duynguyen.io.vn';

  String baseUrl = defaultBase;
  double alertThreshold = 0.55;
  bool alertEnabled = true;
  bool loading = false;
  String? error;
  List<LeafDetection> detections = [];
  List<LeafDetection> alerts = [];
  LeafDetection? latest;
  Timer? _poll;

  Future<void> load() async {
    final prefs = await SharedPreferences.getInstance();
    baseUrl = prefs.getString(_kBase) ?? defaultBase;
    await refresh();
    _poll?.cancel();
    _poll = Timer.periodic(const Duration(seconds: 12), (_) => refresh());
  }

  Future<void> setBaseUrl(String url) async {
    baseUrl = url.trim().replaceAll(RegExp(r'/$'), '');
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_kBase, baseUrl);
    await refresh();
  }

  Uri _u(String path, [Map<String, String>? q]) {
    final b = baseUrl.endsWith('/') ? baseUrl.substring(0, baseUrl.length - 1) : baseUrl;
    return Uri.parse('$b$path').replace(queryParameters: q);
  }

  Future<void> refresh() async {
    loading = true;
    error = null;
    notifyListeners();
    try {
      final settingsRes = await http.get(_u('/api/leaf/settings')).timeout(const Duration(seconds: 12));
      if (settingsRes.statusCode == 200) {
        final j = jsonDecode(settingsRes.body) as Map<String, dynamic>;
        alertThreshold = (j['alert_threshold'] as num?)?.toDouble() ?? alertThreshold;
        alertEnabled = j['alert_enabled'] != false;
      }

      final detRes = await http
          .get(_u('/api/leaf/detections', {'limit': '30'}))
          .timeout(const Duration(seconds: 15));
      if (detRes.statusCode != 200) {
        throw Exception('HTTP ${detRes.statusCode}');
      }
      final dj = jsonDecode(detRes.body) as Map<String, dynamic>;
      final items = (dj['items'] as List? ?? [])
          .whereType<Map>()
          .map((e) => LeafDetection.fromJson(Map<String, dynamic>.from(e), baseUrl))
          .toList();
      detections = items;
      alerts = items.where((e) => e.alert).toList();
      latest = items.isEmpty ? null : items.first;
      error = null;
    } catch (e) {
      error = e.toString();
    } finally {
      loading = false;
      notifyListeners();
    }
  }

  Future<bool> updateThreshold(double value, {bool? enabled}) async {
    try {
      final body = <String, dynamic>{
        'alert_threshold': value.clamp(0.2, 0.99),
        if (enabled != null) 'alert_enabled': enabled,
      };
      final res = await http
          .post(
            _u('/api/leaf/settings'),
            headers: {'Content-Type': 'application/json'},
            body: jsonEncode(body),
          )
          .timeout(const Duration(seconds: 12));
      if (res.statusCode != 200) return false;
      final j = jsonDecode(res.body) as Map<String, dynamic>;
      alertThreshold = (j['alert_threshold'] as num?)?.toDouble() ?? value;
      alertEnabled = j['alert_enabled'] != false;
      notifyListeners();
      await refresh();
      return true;
    } catch (_) {
      return false;
    }
  }

  bool get hasOpenAlert => alerts.isNotEmpty;

  @override
  void dispose() {
    _poll?.cancel();
    super.dispose();
  }
}
