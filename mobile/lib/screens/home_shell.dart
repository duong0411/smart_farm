import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../services/leaf_service.dart';
import '../theme/app_theme.dart';
import 'camera_stream_screen.dart';
import 'dashboard_screen.dart';
import 'disease_screen.dart';

class HomeShell extends StatefulWidget {
  const HomeShell({super.key});

  @override
  State<HomeShell> createState() => _HomeShellState();
}

class _HomeShellState extends State<HomeShell> {
  int _index = 0;

  @override
  Widget build(BuildContext context) {
    final leaf = context.watch<LeafService>();
    final alertCount = leaf.alerts.length;

    return Scaffold(
      body: IndexedStack(
        index: _index,
        children: const [
          DashboardScreen(),
          CameraStreamScreen(),
          DiseaseScreen(),
        ],
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _index,
        onDestinationSelected: (i) => setState(() => _index = i),
        backgroundColor: Colors.white,
        indicatorColor: AppColors.sand,
        destinations: [
          const NavigationDestination(
            icon: Icon(Icons.agriculture_outlined),
            selectedIcon: Icon(Icons.agriculture, color: AppColors.leaf),
            label: 'Trang trại',
          ),
          const NavigationDestination(
            icon: Icon(Icons.videocam_outlined),
            selectedIcon: Icon(Icons.videocam, color: AppColors.water),
            label: 'Camera',
          ),
          NavigationDestination(
            icon: Badge(
              isLabelVisible: alertCount > 0,
              label: Text('$alertCount'),
              child: const Icon(Icons.bug_report_outlined),
            ),
            selectedIcon: Badge(
              isLabelVisible: alertCount > 0,
              label: Text('$alertCount'),
              child: const Icon(Icons.bug_report, color: AppColors.alert),
            ),
            label: 'Sâu bệnh',
          ),
        ],
      ),
    );
  }
}
