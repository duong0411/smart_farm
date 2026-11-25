import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';
import 'package:intl/intl.dart';
import 'package:provider/provider.dart';

import '../services/leaf_service.dart';
import '../theme/app_theme.dart';

class DiseaseScreen extends StatefulWidget {
  const DiseaseScreen({super.key});

  @override
  State<DiseaseScreen> createState() => _DiseaseScreenState();
}

class _DiseaseScreenState extends State<DiseaseScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      context.read<LeafService>().load();
    });
  }

  String _fmtTs(String ts) {
    try {
      final dt = DateTime.parse(ts).toLocal();
      return DateFormat('dd/MM HH:mm:ss').format(dt);
    } catch (_) {
      return ts;
    }
  }

  Future<void> _openThreshold() async {
    final leaf = context.read<LeafService>();
    var thr = leaf.alertThreshold;
    var enabled = leaf.alertEnabled;
    final ok = await showModalBottomSheet<bool>(
      context: context,
      isScrollControlled: true,
      backgroundColor: AppColors.cream,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(20)),
      ),
      builder: (ctx) {
        return StatefulBuilder(
          builder: (ctx, setModal) {
            return Padding(
              padding: EdgeInsets.fromLTRB(
                20,
                16,
                20,
                20 + MediaQuery.of(ctx).viewInsets.bottom,
              ),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Center(
                    child: Container(
                      width: 40,
                      height: 4,
                      decoration: BoxDecoration(
                        color: AppColors.sand,
                        borderRadius: BorderRadius.circular(4),
                      ),
                    ),
                  ),
                  const SizedBox(height: 14),
                  Text(
                    'Ngưỡng cảnh báo sâu bệnh',
                    style: GoogleFonts.fraunces(
                      fontSize: 20,
                      fontWeight: FontWeight.w600,
                      color: AppColors.soil,
                    ),
                  ),
                  const SizedBox(height: 6),
                  const Text(
                    'Khi model phát hiện bệnh và độ tin cậy ≥ ngưỡng, app sẽ cảnh báo và gắn khung đỏ lên ảnh.',
                    style: TextStyle(color: AppColors.muted, fontSize: 13),
                  ),
                  const SizedBox(height: 16),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    title: const Text('Bật cảnh báo'),
                    value: enabled,
                    activeThumbColor: AppColors.leaf,
                    onChanged: (v) => setModal(() => enabled = v),
                  ),
                  Text(
                    'Ngưỡng: ${(thr * 100).round()}%',
                    style: GoogleFonts.dmSans(fontWeight: FontWeight.w700),
                  ),
                  Slider(
                    value: thr,
                    min: 0.2,
                    max: 0.95,
                    divisions: 15,
                    label: '${(thr * 100).round()}%',
                    activeColor: AppColors.alert,
                    onChanged: (v) => setModal(() => thr = v),
                  ),
                  const SizedBox(height: 8),
                  ElevatedButton(
                    onPressed: () => Navigator.pop(ctx, true),
                    child: const Text('Lưu ngưỡng'),
                  ),
                  TextButton(
                    onPressed: () => Navigator.pop(ctx, false),
                    child: const Text('Hủy'),
                  ),
                ],
              ),
            );
          },
        );
      },
    );
    if (ok == true && mounted) {
      final saved = await leaf.updateThreshold(thr, enabled: enabled);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(saved ? 'Đã lưu ngưỡng ${(thr * 100).round()}%' : 'Lưu thất bại'),
        ),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    final leaf = context.watch<LeafService>();
    final latest = leaf.latest;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Sâu bệnh'),
        actions: [
          IconButton(
            tooltip: 'Ngưỡng cảnh báo',
            onPressed: _openThreshold,
            icon: const Icon(Icons.tune),
          ),
          IconButton(
            tooltip: 'Làm mới',
            onPressed: leaf.loading ? null : () => leaf.refresh(),
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: RefreshIndicator(
        onRefresh: leaf.refresh,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 32),
          children: [
            if (leaf.hasOpenAlert)
              Container(
                padding: const EdgeInsets.all(14),
                decoration: BoxDecoration(
                  color: const Color(0xFFFFEBEE),
                  borderRadius: BorderRadius.circular(14),
                  border: Border.all(color: AppColors.alert.withValues(alpha: 0.45)),
                ),
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    const Icon(Icons.warning_amber_rounded, color: AppColors.alert),
                    const SizedBox(width: 10),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            'Cảnh báo sâu bệnh',
                            style: GoogleFonts.dmSans(
                              fontWeight: FontWeight.w700,
                              color: AppColors.alert,
                            ),
                          ),
                          const SizedBox(height: 4),
                          Text(
                            '${leaf.alerts.first.labelVi} · ${(leaf.alerts.first.score * 100).round()}%'
                            ' (≥ ${(leaf.alertThreshold * 100).round()}%)',
                            style: const TextStyle(color: AppColors.alert),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              )
            else
              Container(
                padding: const EdgeInsets.all(14),
                decoration: BoxDecoration(
                  color: const Color(0xFFE8F5E9),
                  borderRadius: BorderRadius.circular(14),
                  border: Border.all(color: AppColors.mint.withValues(alpha: 0.35)),
                ),
                child: Row(
                  children: [
                    const Icon(Icons.eco, color: AppColors.leaf),
                    const SizedBox(width: 10),
                    Expanded(
                      child: Text(
                        leaf.alertEnabled
                            ? 'Chưa có cảnh báo · ngưỡng ${(leaf.alertThreshold * 100).round()}%'
                            : 'Cảnh báo đang tắt',
                        style: const TextStyle(color: AppColors.leaf),
                      ),
                    ),
                  ],
                ),
              ),
            const SizedBox(height: 14),
            if (leaf.error != null)
              Padding(
                padding: const EdgeInsets.only(bottom: 12),
                child: Text(
                  'Không kết nối AI: ${leaf.error}',
                  style: const TextStyle(color: AppColors.alert, fontSize: 13),
                ),
              ),
            Text(
              'Ảnh mới nhất',
              style: GoogleFonts.fraunces(
                fontSize: 20,
                fontWeight: FontWeight.w600,
                color: AppColors.soil,
              ),
            ),
            const SizedBox(height: 10),
            if (latest == null)
              Container(
                height: 180,
                alignment: Alignment.center,
                decoration: BoxDecoration(
                  color: Colors.white,
                  borderRadius: BorderRadius.circular(16),
                  border: Border.all(color: const Color(0xFFDCE8DA)),
                ),
                child: Text(
                  leaf.loading ? 'Đang tải…' : 'Chưa có ảnh từ camera AI',
                  style: const TextStyle(color: AppColors.muted),
                ),
              )
            else ...[
              ClipRRect(
                borderRadius: BorderRadius.circular(16),
                child: AspectRatio(
                  aspectRatio: 4 / 3,
                  child: Image.network(
                    latest.vizUrl.isNotEmpty ? latest.vizUrl : latest.imageUrl,
                    fit: BoxFit.cover,
                    errorBuilder: (_, __, ___) => Container(
                      color: AppColors.sand,
                      alignment: Alignment.center,
                      child: const Text('Không tải được ảnh'),
                    ),
                  ),
                ),
              ),
              const SizedBox(height: 10),
              Container(
                padding: const EdgeInsets.all(14),
                decoration: BoxDecoration(
                  color: Colors.white,
                  borderRadius: BorderRadius.circular(16),
                  border: Border.all(
                    color: latest.alert
                        ? AppColors.alert.withValues(alpha: 0.45)
                        : const Color(0xFFDCE8DA),
                  ),
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      latest.labelVi,
                      style: GoogleFonts.dmSans(
                        fontSize: 18,
                        fontWeight: FontWeight.w700,
                        color: latest.alert ? AppColors.alert : AppColors.soil,
                      ),
                    ),
                    const SizedBox(height: 4),
                    Text(
                      'Tin cậy ${(latest.score * 100).round()}% · ${_fmtTs(latest.ts)}',
                      style: const TextStyle(color: AppColors.muted, fontSize: 13),
                    ),
                    if (latest.probs.isNotEmpty) ...[
                      const SizedBox(height: 10),
                      ...latest.probs.entries.map((e) {
                        return Padding(
                          padding: const EdgeInsets.only(bottom: 6),
                          child: Row(
                            children: [
                              SizedBox(
                                width: 110,
                                child: Text(
                                  e.key,
                                  style: const TextStyle(fontSize: 12, color: AppColors.muted),
                                ),
                              ),
                              Expanded(
                                child: ClipRRect(
                                  borderRadius: BorderRadius.circular(4),
                                  child: LinearProgressIndicator(
                                    value: e.value.clamp(0, 1),
                                    minHeight: 8,
                                    backgroundColor: AppColors.sand,
                                    color: e.key == latest.label
                                        ? (latest.alert ? AppColors.alert : AppColors.leaf)
                                        : AppColors.mint,
                                  ),
                                ),
                              ),
                              const SizedBox(width: 8),
                              Text('${(e.value * 100).round()}%'),
                            ],
                          ),
                        );
                      }),
                    ],
                  ],
                ),
              ),
            ],
            const SizedBox(height: 22),
            Text(
              'Lịch sử phát hiện',
              style: GoogleFonts.fraunces(
                fontSize: 20,
                fontWeight: FontWeight.w600,
                color: AppColors.soil,
              ),
            ),
            const SizedBox(height: 10),
            if (leaf.detections.isEmpty)
              const Text('Chưa có dữ liệu.', style: TextStyle(color: AppColors.muted))
            else
              ...leaf.detections.map((d) => _HistoryTile(item: d, fmtTs: _fmtTs)),
          ],
        ),
      ),
    );
  }
}

class _HistoryTile extends StatelessWidget {
  final LeafDetection item;
  final String Function(String) fmtTs;

  const _HistoryTile({required this.item, required this.fmtTs});

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(
          color: item.alert
              ? AppColors.alert.withValues(alpha: 0.4)
              : const Color(0xFFDCE8DA),
        ),
      ),
      child: ListTile(
        contentPadding: const EdgeInsets.all(10),
        leading: ClipRRect(
          borderRadius: BorderRadius.circular(10),
          child: SizedBox(
            width: 64,
            height: 64,
            child: Image.network(
              item.vizUrl.isNotEmpty ? item.vizUrl : item.imageUrl,
              fit: BoxFit.cover,
              errorBuilder: (_, __, ___) => Container(
                color: AppColors.sand,
                child: Icon(
                  item.alert ? Icons.bug_report : Icons.eco,
                  color: item.alert ? AppColors.alert : AppColors.leaf,
                ),
              ),
            ),
          ),
        ),
        title: Text(
          item.labelVi,
          style: TextStyle(
            fontWeight: FontWeight.w700,
            color: item.alert ? AppColors.alert : AppColors.ink,
          ),
        ),
        subtitle: Text(
          '${(item.score * 100).round()}% · ${fmtTs(item.ts)}'
          '${item.alert ? ' · CẢNH BÁO' : ''}',
        ),
        onTap: () {
          showDialog(
            context: context,
            builder: (_) => Dialog(
              insetPadding: const EdgeInsets.all(16),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  ClipRRect(
                    borderRadius: const BorderRadius.vertical(top: Radius.circular(12)),
                    child: Image.network(
                      item.vizUrl.isNotEmpty ? item.vizUrl : item.imageUrl,
                      fit: BoxFit.contain,
                    ),
                  ),
                  Padding(
                    padding: const EdgeInsets.all(12),
                    child: Text(
                      '${item.labelVi} · ${(item.score * 100).round()}%',
                      style: GoogleFonts.dmSans(fontWeight: FontWeight.w700),
                    ),
                  ),
                ],
              ),
            ),
          );
        },
      ),
    );
  }
}
