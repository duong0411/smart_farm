import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

class AuthUser {
  final String email;
  final String name;

  const AuthUser({required this.email, required this.name});

  Map<String, dynamic> toJson() => {'email': email, 'name': name};

  factory AuthUser.fromJson(Map<String, dynamic> j) => AuthUser(
        email: j['email'] as String,
        name: j['name'] as String? ?? '',
      );
}

class AuthService extends ChangeNotifier {
  static const _usersKey = 'sf_users';
  static const _sessionKey = 'sf_session';
  static const _resetKey = 'sf_reset_tokens';

  AuthUser? _current;
  AuthUser? get currentUser => _current;
  bool get isLoggedIn => _current != null;

  Future<void> loadSession() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_sessionKey);
    if (raw == null) return;
    try {
      _current = AuthUser.fromJson(jsonDecode(raw) as Map<String, dynamic>);
      notifyListeners();
    } catch (_) {}
  }

  String _hash(String password) =>
      sha256.convert(utf8.encode('smartfarm::$password')).toString();

  Future<List<Map<String, dynamic>>> _readUsers(SharedPreferences prefs) async {
    final raw = prefs.getString(_usersKey);
    if (raw == null || raw.isEmpty) return [];
    final list = jsonDecode(raw) as List<dynamic>;
    return list.cast<Map<String, dynamic>>();
  }

  Future<void> _writeUsers(
    SharedPreferences prefs,
    List<Map<String, dynamic>> users,
  ) async {
    await prefs.setString(_usersKey, jsonEncode(users));
  }

  Future<void> _setSession(AuthUser user) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_sessionKey, jsonEncode(user.toJson()));
    _current = user;
    notifyListeners();
  }

  Future<String?> register({
    required String name,
    required String email,
    required String password,
  }) async {
    final e = email.trim().toLowerCase();
    if (name.trim().isEmpty) return 'Vui lòng nhập họ tên';
    if (!_validEmail(e)) return 'Email không hợp lệ';
    if (password.length < 6) return 'Mật khẩu tối thiểu 6 ký tự';

    final prefs = await SharedPreferences.getInstance();
    final users = await _readUsers(prefs);
    if (users.any((u) => (u['email'] as String).toLowerCase() == e)) {
      return 'Email đã được đăng ký';
    }

    users.add({
      'email': e,
      'name': name.trim(),
      'password': _hash(password),
    });
    await _writeUsers(prefs, users);
    await _setSession(AuthUser(email: e, name: name.trim()));
    return null;
  }

  Future<String?> login({
    required String email,
    required String password,
  }) async {
    final e = email.trim().toLowerCase();
    final prefs = await SharedPreferences.getInstance();
    final users = await _readUsers(prefs);
    final user = users.cast<Map<String, dynamic>?>().firstWhere(
          (u) =>
              u != null &&
              (u['email'] as String).toLowerCase() == e &&
              u['password'] == _hash(password),
          orElse: () => null,
        );
    if (user == null) return 'Email hoặc mật khẩu không đúng';
    await _setSession(
      AuthUser(email: user['email'] as String, name: user['name'] as String? ?? ''),
    );
    return null;
  }

  /// Tạo mã reset (demo local). Trả về mã để hiển thị — không có SMTP backend.
  Future<String?> requestPasswordReset(String email) async {
    final e = email.trim().toLowerCase();
    if (!_validEmail(e)) return null;

    final prefs = await SharedPreferences.getInstance();
    final users = await _readUsers(prefs);
    final exists = users.any((u) => (u['email'] as String).toLowerCase() == e);
    if (!exists) return null;

    final code = (100000 + DateTime.now().millisecondsSinceEpoch % 900000)
        .toString();
    final raw = prefs.getString(_resetKey);
    final map = raw == null || raw.isEmpty
        ? <String, dynamic>{}
        : jsonDecode(raw) as Map<String, dynamic>;
    map[e] = {
      'code': code,
      'exp': DateTime.now().add(const Duration(minutes: 15)).toIso8601String(),
    };
    await prefs.setString(_resetKey, jsonEncode(map));
    return code;
  }

  Future<String?> resetPassword({
    required String email,
    required String code,
    required String newPassword,
  }) async {
    final e = email.trim().toLowerCase();
    if (newPassword.length < 6) return 'Mật khẩu tối thiểu 6 ký tự';

    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_resetKey);
    if (raw == null || raw.isEmpty) return 'Mã xác nhận không hợp lệ';
    final map = jsonDecode(raw) as Map<String, dynamic>;
    final entry = map[e] as Map<String, dynamic>?;
    if (entry == null) return 'Mã xác nhận không hợp lệ';
    if (entry['code'] != code.trim()) return 'Mã xác nhận không đúng';
    final exp = DateTime.tryParse(entry['exp'] as String? ?? '');
    if (exp == null || exp.isBefore(DateTime.now())) {
      return 'Mã xác nhận đã hết hạn';
    }

    final users = await _readUsers(prefs);
    final idx = users.indexWhere((u) => (u['email'] as String).toLowerCase() == e);
    if (idx < 0) return 'Tài khoản không tồn tại';
    users[idx]['password'] = _hash(newPassword);
    await _writeUsers(prefs, users);
    map.remove(e);
    await prefs.setString(_resetKey, jsonEncode(map));
    return null;
  }

  Future<void> logout() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_sessionKey);
    _current = null;
    notifyListeners();
  }

  bool _validEmail(String e) =>
      RegExp(r'^[^@\s]+@[^@\s]+\.[^@\s]+$').hasMatch(e);
}
