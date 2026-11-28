import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:intl/intl.dart';
import 'package:provider/provider.dart';

import '../services/cam_crypto.dart';
import '../services/cam_stream_service.dart';
import '../services/mqtt_service.dart';
import '../theme/app_theme.dart';

class CameraStreamScreen extends StatelessWidget {
  const CameraStreamScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final mqtt = context.watch<MqttService>();
    final cam = context.watch<CamStreamService>();
    final timeFmt = DateFormat('HH:mm:ss');

    return Scaffold(
      backgroundColor: AppColors.cream,
      appBar: AppBar(
        title: const Text('Camera'),
        actions: [
          IconButton(
            tooltip: 'Cấu hình AES',
            onPressed: () => _showAesDialog(context, cam),
            icon: const Icon(Icons.vpn_key_outlined),
          ),
          IconButton(
            tooltip: 'Chụp 1 frame',
            onPressed: mqtt.isConnected ? cam.snap : null,
            icon: const Icon(Icons.camera_alt_outlined),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 12, 16, 28),
        children: [
          _StatusRow(
            mqttOk: mqtt.isConnected,
            camOk: cam.camOnline,
            streaming: cam.streamOn,
            assembling: cam.assembling,
          ),
          const SizedBox(height: 12),
          SwitchListTile.adaptive(
            contentPadding: EdgeInsets.zero,
            title: const Text('Stream liên tục'),
            subtitle: Text(
              mqtt.chipId.isEmpty
                  ? 'Chưa có Chip ID'
                  : 'tele/${mqtt.chipId}_cam/frame · JPEG liên tục',
              style: const TextStyle(fontSize: 12, color: AppColors.muted),
            ),
            value: cam.streamOn,
            activeTrackColor: AppColors.mint,
            onChanged: mqtt.isConnected ? cam.setStream : null,
          ),
          const SizedBox(height: 8),
          AspectRatio(
            aspectRatio: cam.width > 0 && cam.height > 0
                ? cam.width / cam.height
                : 4 / 3,
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: const Color(0xFF0B1610),
                borderRadius: BorderRadius.circular(16),
              ),
              child: ClipRRect(
                borderRadius: BorderRadius.circular(16),
                child: cam.hasFrame
                    ? Image.memory(
                        cam.jpeg!,
                        fit: BoxFit.contain,
                        gaplessPlayback: true,
                        filterQuality: FilterQuality.medium,
                      )
                    : Center(
                        child: Padding(
                          padding: const EdgeInsets.all(24),
                          child: Text(
                            mqtt.isConnected
                                ? 'Chờ stream từ ESP32-CAM\n(Chip ID trùng firmware)'
                                : 'Chưa kết nối MQTT',
                            textAlign: TextAlign.center,
                            style: const TextStyle(
                              color: Color(0xFF9BB5A4),
                              height: 1.4,
                            ),
                          ),
                        ),
                      ),
              ),
            ),
          ),
          const SizedBox(height: 12),
          if (cam.error != null)
            Text(
              cam.error!,
              style: const TextStyle(color: AppColors.alert, fontSize: 13),
            ),
          if (cam.hasFrame)
            Text(
              '${cam.width}×${cam.height} · frame #${cam.framesReceived}'
              '${cam.lastFrameAt != null ? ' · ${timeFmt.format(cam.lastFrameAt!)}' : ''}',
              style: const TextStyle(color: AppColors.muted, fontSize: 13),
            ),
          const SizedBox(height: 16),
          Text(
            'Firmware SoftAP ESP32-CAM-MQTT · broker mqtt.duynguyen.io.vn\n'
            'AES mặc định: SmartFarmCamKey!',
            style: TextStyle(
              color: AppColors.muted.withValues(alpha: 0.9),
              fontSize: 12,
              height: 1.35,
            ),
          ),
        ],
      ),
    );
  }

  Future<void> _showAesDialog(BuildContext context, CamStreamService cam) async {
    final ctrl = TextEditingController(text: cam.aesKeyHex);
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Khóa AES-128'),
        content: TextField(
          controller: ctrl,
          decoration: const InputDecoration(
            labelText: 'Hex 32 ký tự',
            hintText: CamCrypto.defaultKeyHex,
          ),
          inputFormatters: [
            FilteringTextInputFormatter.allow(RegExp(r'[0-9a-fA-F]')),
            LengthLimitingTextInputFormatter(32),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Hủy'),
          ),
          TextButton(
            onPressed: () async {
              await cam.setAesKeyHex(ctrl.text);
              if (ctx.mounted) Navigator.pop(ctx, true);
            },
            child: const Text('Lưu'),
          ),
        ],
      ),
    );
    ctrl.dispose();
    if (ok == true && context.mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Đã lưu khóa AES')),
      );
    }
  }
}

class _StatusRow extends StatelessWidget {
  const _StatusRow({
    required this.mqttOk,
    required this.camOk,
    required this.streaming,
    required this.assembling,
  });

  final bool mqttOk;
  final bool camOk;
  final bool streaming;
  final bool assembling;

  @override
  Widget build(BuildContext context) {
    Widget chip(String label, bool ok) {
      return Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
        decoration: BoxDecoration(
          color: ok ? const Color(0xFFE2F3E8) : const Color(0xFFF3E4E2),
          borderRadius: BorderRadius.circular(999),
        ),
        child: Text(
          label,
          style: TextStyle(
            fontSize: 12,
            fontWeight: FontWeight.w600,
            color: ok ? AppColors.leaf : AppColors.alert,
          ),
        ),
      );
    }

    return Wrap(
      spacing: 8,
      runSpacing: 8,
      children: [
        chip(mqttOk ? 'MQTT OK' : 'MQTT mất', mqttOk),
        chip(camOk ? 'CAM online' : 'CAM offline', camOk),
        chip(streaming ? 'Stream ON' : 'Stream OFF', streaming),
        if (assembling) chip('Ráp frame…', true),
      ],
    );
  }
}
