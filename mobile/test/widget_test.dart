import 'package:flutter_test/flutter_test.dart';
import 'package:provider/provider.dart';
import 'package:smart_farm_app/main.dart';
import 'package:smart_farm_app/services/auth_service.dart';
import 'package:smart_farm_app/services/mqtt_service.dart';

void main() {
  testWidgets('Login screen renders', (tester) async {
    await tester.pumpWidget(
      MultiProvider(
        providers: [
          ChangeNotifierProvider(create: (_) => AuthService()),
          ChangeNotifierProvider(create: (_) => MqttService()),
        ],
        child: const SmartFarmApp(),
      ),
    );
    expect(find.text('Đăng nhập'), findsOneWidget);
    expect(find.text('Smart Farm'), findsWidgets);
  });
}
