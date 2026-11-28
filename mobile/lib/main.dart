import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import 'screens/chip_id_screen.dart';
import 'screens/home_shell.dart';
import 'screens/login_screen.dart';
import 'services/auth_service.dart';
import 'services/cam_stream_service.dart';
import 'services/mqtt_service.dart';
import 'theme/app_theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  SystemChrome.setSystemUIOverlayStyle(
    const SystemUiOverlayStyle(
      statusBarColor: Colors.transparent,
      statusBarIconBrightness: Brightness.light,
    ),
  );

  final auth = AuthService();
  final mqtt = MqttService();
  final cam = CamStreamService();
  await auth.loadSession();
  await mqtt.loadChipId();
  await cam.load();
  mqtt.attachCam(cam);

  runApp(
    MultiProvider(
      providers: [
        ChangeNotifierProvider.value(value: auth),
        ChangeNotifierProvider.value(value: mqtt),
        ChangeNotifierProvider.value(value: cam),
      ],
      child: const SmartFarmApp(),
    ),
  );
}

class SmartFarmApp extends StatelessWidget {
  const SmartFarmApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Smart Farm',
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(),
      home: const _RootGate(),
    );
  }
}

class _RootGate extends StatefulWidget {
  const _RootGate();

  @override
  State<_RootGate> createState() => _RootGateState();
}

class _RootGateState extends State<_RootGate> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final auth = context.read<AuthService>();
      final mqtt = context.read<MqttService>();
      if (auth.isLoggedIn && mqtt.chipId.isNotEmpty) {
        mqtt.connect();
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final auth = context.watch<AuthService>();
    final mqtt = context.watch<MqttService>();

    if (!auth.isLoggedIn) return const LoginScreen();
    if (mqtt.chipId.isEmpty) return const ChipIdScreen();
    return const HomeShell();
  }
}
