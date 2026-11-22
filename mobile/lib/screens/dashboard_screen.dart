import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:intl/intl.dart';
import 'package:provider/provider.dart';

import '../services/auth_service.dart';
import '../services/mqtt_service.dart';
import '../theme/app_theme.dart';
class DashboardScreen extends StatelessWidget {
  const DashboardScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final mqtt = context.watch<MqttService>();
    final s = mqtt.state;
    final time = s.lastUpdate == null
        ? '—'
        : DateFormat('HH:mm:ss').format(s.lastUpdate!);

    return Scaffold(
      appBar: AppBar(
        title: Text('Chip ${s.chipId}'),
        actions: [
          IconButton(
            tooltip: 'Đổi Chip ID',
            onPressed: () => mqtt.clearChipId(),
            icon: const Icon(Icons.edit),
          ),
          IconButton(
            tooltip: 'Kết nối lại',
            onPressed: mqtt.connecting ? null : () => mqtt.reconnect(),
            icon: const Icon(Icons.refresh),
          ),
          IconButton(
            tooltip: 'Đăng xuất',
            onPressed: () async {
              final auth = context.read<AuthService>();
              await mqtt.disconnect();
              await auth.logout();
            },
            icon: const Icon(Icons.logout),
          ),
        ],
      ),
      body: RefreshIndicator(
        onRefresh: () => mqtt.reconnect(),
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 32),
          children: [
            _StatusBanner(
              online: s.online,
              mqtt: s.mqttConnected,
              connecting: mqtt.connecting,
              error: mqtt.error,
              mode: s.mode,
              time: time,
            ),
            if (s.waterEmpty) ...[
              const SizedBox(height: 12),
              Container(
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: const Color(0xFFFFEBEE),
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: AppColors.alert.withValues(alpha: 0.4)),
                ),
                child: const Row(
                  children: [
                    Icon(Icons.warning_amber_rounded, color: AppColors.alert),
                    SizedBox(width: 10),
                    Expanded(
                      child: Text(
                        'Cảnh báo: bể nước hết / thấp — bơm có thể bị khóa',
                        style: TextStyle(color: AppColors.alert),
                      ),
                    ),
                  ],
                ),
              ),
            ],
            const SizedBox(height: 18),
            Text(
              'Cảm biến',
              style: GoogleFonts.fraunces(
                fontSize: 20,
                fontWeight: FontWeight.w600,
                color: AppColors.soil,
              ),
            ),
            const SizedBox(height: 10),
            GridView.count(
              crossAxisCount: 2,
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              mainAxisSpacing: 10,
              crossAxisSpacing: 10,
              childAspectRatio: 1.35,
              children: [
                _SensorTile(
                  icon: Icons.thermostat,
                  label: 'Nhiệt độ',
                  value: '${s.temp.toStringAsFixed(1)}°C',
                  color: const Color(0xFFE07A3D),
                ),
                _SensorTile(
                  icon: Icons.water_drop_outlined,
                  label: 'Độ ẩm KK',
                  value: '${s.humi.toStringAsFixed(1)}%',
                  color: AppColors.water,
                ),
                _SensorTile(
                  icon: Icons.grass,
                  label: 'Độ ẩm đất',
                  value: '${s.soil}',
                  color: AppColors.mint,
                ),
                _SensorTile(
                  icon: Icons.opacity,
                  label: 'Mực nước',
                  value: s.waterEmpty ? '${s.water} (hết)' : '${s.water}',
                  color: s.waterEmpty ? AppColors.alert : AppColors.water,
                ),
                _SensorTile(
                  icon: Icons.co2,
                  label: 'CO₂',
                  value: '${s.co2Ppm.toStringAsFixed(0)} ppm',
                  color: AppColors.amber,
                ),
                _SensorTile(
                  icon: Icons.wb_sunny_outlined,
                  label: 'Ánh sáng',
                  value: s.dark ? 'TỐI' : 'SÁNG',
                  color: s.dark ? AppColors.muted : AppColors.amber,
                ),
              ],
            ),
            const SizedBox(height: 22),
            Text(
              'Điều khiển',
              style: GoogleFonts.fraunces(
                fontSize: 20,
                fontWeight: FontWeight.w600,
                color: AppColors.soil,
              ),
            ),
            const SizedBox(height: 6),
            Text(
              s.isManual
                  ? 'Chế độ MANUAL — bật/tắt thiết bị thủ công'
                  : 'Chế độ AUTO — chuyển MANUAL để điều khiển tay',
              style: const TextStyle(color: AppColors.muted, fontSize: 13),
            ),
            const SizedBox(height: 10),
            _ControlCard(
              title: 'Chế độ',
              subtitle: s.mode,
              trailing: Switch(
                value: !s.isManual,
                activeThumbColor: AppColors.leaf,
                onChanged: mqtt.isConnected
                    ? (auto) => mqtt.setMode(auto)
                    : null,
              ),
            ),
            _ControlCard(
              title: 'Bơm nước',
              subtitle: s.pump ? 'Đang chạy' : 'Tắt',
              icon: Icons.water,
              trailing: Switch(
                value: s.pump,
                activeThumbColor: AppColors.water,
                onChanged: mqtt.isConnected && s.isManual
                    ? (v) => mqtt.setPump(v)
                    : null,
              ),
            ),
            _ControlCard(
              title: 'Đèn',
              subtitle: s.light ? 'Bật' : 'Tắt',
              icon: Icons.lightbulb_outline,
              trailing: Switch(
                value: s.light,
                activeThumbColor: AppColors.amber,
                onChanged: mqtt.isConnected && s.isManual
                    ? (v) => mqtt.setLight(v)
                    : null,
              ),
            ),
            _ControlCard(
              title: 'Quạt',
              subtitle: (s.fan1 || s.fan2) ? 'Bật' : 'Tắt',
              icon: Icons.air,
              trailing: Switch(
                value: s.fan1 || s.fan2,
                activeThumbColor: AppColors.mint,
                onChanged: mqtt.isConnected && s.isManual
                    ? (v) => mqtt.setFans(v)
                    : null,
              ),
            ),
            const SizedBox(height: 8),
            OutlinedButton.icon(
              onPressed: mqtt.isConnected ? mqtt.calibrateCo2 : null,
              icon: const Icon(Icons.tune),
              label: const Text('Calibrate CO₂ (ngoài trời)'),
              style: OutlinedButton.styleFrom(
                foregroundColor: AppColors.leaf,
                minimumSize: const Size.fromHeight(48),
                side: const BorderSide(color: AppColors.mint),
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(14),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _StatusBanner extends StatelessWidget {
  final bool online;
  final bool mqtt;
  final bool connecting;
  final String? error;
  final String mode;
  final String time;

  const _StatusBanner({
    required this.online,
    required this.mqtt,
    required this.connecting,
    required this.error,
    required this.mode,
    required this.time,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        gradient: const LinearGradient(
          colors: [AppColors.soil, AppColors.leaf],
        ),
        borderRadius: BorderRadius.circular(18),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              _pill(online ? 'ESP Online' : 'ESP Offline', online),
              const SizedBox(width: 8),
              _pill(
                connecting
                    ? 'MQTT…'
                    : (mqtt ? 'MQTT OK' : 'MQTT mất'),
                mqtt && !connecting,
              ),
              const Spacer(),
              Text(
                mode,
                style: GoogleFonts.dmSans(
                  color: Colors.white,
                  fontWeight: FontWeight.w700,
                ),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Text(
            'Cập nhật: $time',
            style: const TextStyle(color: Colors.white70, fontSize: 13),
          ),
          if (error != null) ...[
            const SizedBox(height: 8),
            Text(error!, style: const TextStyle(color: Color(0xFFFFCDD2))),
          ],
        ],
      ),
    );
  }

  Widget _pill(String text, bool ok) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      decoration: BoxDecoration(
        color: ok ? Colors.white24 : Colors.black26,
        borderRadius: BorderRadius.circular(20),
      ),
      child: Text(
        text,
        style: const TextStyle(color: Colors.white, fontSize: 12),
      ),
    );
  }
}

class _SensorTile extends StatelessWidget {
  final IconData icon;
  final String label;
  final String value;
  final Color color;

  const _SensorTile({
    required this.icon,
    required this.label,
    required this.value,
    required this.color,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: const Color(0xFFDCE8DA)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(icon, color: color, size: 22),
          const Spacer(),
          Text(
            label,
            style: const TextStyle(color: AppColors.muted, fontSize: 12),
          ),
          const SizedBox(height: 2),
          Text(
            value,
            style: GoogleFonts.dmSans(
              fontSize: 18,
              fontWeight: FontWeight.w700,
              color: AppColors.ink,
            ),
          ),
        ],
      ),
    );
  }
}

class _ControlCard extends StatelessWidget {
  final String title;
  final String subtitle;
  final Widget trailing;
  final IconData? icon;

  const _ControlCard({
    required this.title,
    required this.subtitle,
    required this.trailing,
    this.icon,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: const Color(0xFFDCE8DA)),
      ),
      child: ListTile(
        contentPadding: EdgeInsets.zero,
        leading: icon == null
            ? null
            : CircleAvatar(
                backgroundColor: AppColors.sand,
                child: Icon(icon, color: AppColors.leaf),
              ),
        title: Text(title, style: const TextStyle(fontWeight: FontWeight.w600)),
        subtitle: Text(subtitle),
        trailing: trailing,
      ),
    );
  }
}
