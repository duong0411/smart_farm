import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:provider/provider.dart';

import '../services/auth_service.dart';
import '../services/mqtt_service.dart';
import '../theme/app_theme.dart';

class ChipIdScreen extends StatefulWidget {
  const ChipIdScreen({super.key});

  @override
  State<ChipIdScreen> createState() => _ChipIdScreenState();
}

class _ChipIdScreenState extends State<ChipIdScreen> {
  late final TextEditingController _chip;
  bool _loading = false;

  @override
  void initState() {
    super.initState();
    _chip = TextEditingController(
      text: context.read<MqttService>().chipId,
    );
  }

  @override
  void dispose() {
    _chip.dispose();
    super.dispose();
  }

  Future<void> _connect() async {
    final id = _chip.text.trim();
    if (id.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Nhập Chip ID (ví dụ: 123)')),
      );
      return;
    }
    setState(() => _loading = true);
    await context.read<MqttService>().setChipId(id);
    if (!mounted) return;
    setState(() => _loading = false);
  }

  @override
  Widget build(BuildContext context) {
    final auth = context.watch<AuthService>();
    return Scaffold(
      appBar: AppBar(
        title: const Text('Kết nối thiết bị'),
        actions: [
          IconButton(
            tooltip: 'Đăng xuất',
            onPressed: () async {
              final mqtt = context.read<MqttService>();
              final auth = context.read<AuthService>();
              await mqtt.disconnect();
              await auth.logout();
            },
            icon: const Icon(Icons.logout),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(24),
        children: [
          Text(
            'Xin chào, ${auth.currentUser?.name.isNotEmpty == true ? auth.currentUser!.name : auth.currentUser?.email ?? ''}',
            style: GoogleFonts.fraunces(
              fontSize: 22,
              fontWeight: FontWeight.w600,
              color: AppColors.soil,
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            'Nhập Chip ID đã cấu hình trên ESP32 (mặc định firmware: 123). '
            'App sẽ subscribe tele/{id}_* và điều khiển cmnd/{id}_pump/POWER.',
            style: TextStyle(color: AppColors.muted, height: 1.4),
          ),
          const SizedBox(height: 24),
          TextField(
            controller: _chip,
            textInputAction: TextInputAction.done,
            onSubmitted: (_) => _connect(),
            decoration: const InputDecoration(
              labelText: 'Chip ID',
              hintText: '123',
              prefixIcon: Icon(Icons.memory),
            ),
          ),
          const SizedBox(height: 16),
          ElevatedButton(
            onPressed: _loading ? null : _connect,
            child: _loading
                ? const SizedBox(
                    height: 22,
                    width: 22,
                    child: CircularProgressIndicator(
                      strokeWidth: 2,
                      color: Colors.white,
                    ),
                  )
                : const Text('Kết nối MQTT'),
          ),
          const SizedBox(height: 20),
          Container(
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              color: AppColors.sand,
              borderRadius: BorderRadius.circular(16),
            ),
            child: const Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  'Broker',
                  style: TextStyle(fontWeight: FontWeight.w700),
                ),
                SizedBox(height: 6),
                Text('wss://mqtt.duynguyen.io.vn:443/mqtt'),
                SizedBox(height: 10),
                Text(
                  'Ví dụ topic',
                  style: TextStyle(fontWeight: FontWeight.w700),
                ),
                SizedBox(height: 6),
                Text('tele/123_vitals/status'),
                Text('cmnd/123_pump/POWER  →  ON | OFF'),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
