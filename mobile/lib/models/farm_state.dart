class FarmState {
  final String chipId;
  final bool online;
  final bool mqttConnected;
  final double temp;
  final double humi;
  final int soil;
  final int water;
  final bool waterEmpty;
  final int co2Raw;
  final double co2Ppm;
  final bool dark;
  final bool pump;
  final bool light;
  final bool fan1;
  final bool fan2;
  final String mode; // AUTO | MANUAL
  final DateTime? lastUpdate;

  const FarmState({
    this.chipId = '',
    this.online = false,
    this.mqttConnected = false,
    this.temp = 0,
    this.humi = 0,
    this.soil = 0,
    this.water = 0,
    this.waterEmpty = false,
    this.co2Raw = 0,
    this.co2Ppm = 0,
    this.dark = false,
    this.pump = false,
    this.light = false,
    this.fan1 = false,
    this.fan2 = false,
    this.mode = 'AUTO',
    this.lastUpdate,
  });

  bool get isManual => mode.toUpperCase() == 'MANUAL';

  FarmState copyWith({
    String? chipId,
    bool? online,
    bool? mqttConnected,
    double? temp,
    double? humi,
    int? soil,
    int? water,
    bool? waterEmpty,
    int? co2Raw,
    double? co2Ppm,
    bool? dark,
    bool? pump,
    bool? light,
    bool? fan1,
    bool? fan2,
    String? mode,
    DateTime? lastUpdate,
  }) {
    return FarmState(
      chipId: chipId ?? this.chipId,
      online: online ?? this.online,
      mqttConnected: mqttConnected ?? this.mqttConnected,
      temp: temp ?? this.temp,
      humi: humi ?? this.humi,
      soil: soil ?? this.soil,
      water: water ?? this.water,
      waterEmpty: waterEmpty ?? this.waterEmpty,
      co2Raw: co2Raw ?? this.co2Raw,
      co2Ppm: co2Ppm ?? this.co2Ppm,
      dark: dark ?? this.dark,
      pump: pump ?? this.pump,
      light: light ?? this.light,
      fan1: fan1 ?? this.fan1,
      fan2: fan2 ?? this.fan2,
      mode: mode ?? this.mode,
      lastUpdate: lastUpdate ?? this.lastUpdate,
    );
  }

  factory FarmState.fromVitals(Map<String, dynamic> j, String chipId) {
    bool onOff(dynamic v) =>
        v?.toString().toUpperCase() == 'ON' || v == true || v == 1;

    return FarmState(
      chipId: chipId,
      online: true,
      temp: (j['temp'] as num?)?.toDouble() ?? 0,
      humi: (j['humi'] as num?)?.toDouble() ?? 0,
      soil: (j['soil'] as num?)?.toInt() ?? 0,
      water: (j['water'] as num?)?.toInt() ?? 0,
      waterEmpty: j['waterEmpty'] == 1 || j['waterEmpty'] == true,
      co2Raw: (j['co2Raw'] as num?)?.toInt() ?? 0,
      co2Ppm: (j['co2Ppm'] as num?)?.toDouble() ?? 0,
      dark: j['dark'] == 1 || j['dark'] == true,
      pump: onOff(j['pump']),
      light: onOff(j['light']),
      fan1: onOff(j['fan1']),
      fan2: onOff(j['fan2']),
      mode: (j['mode'] ?? 'AUTO').toString(),
      lastUpdate: DateTime.now(),
    );
  }
}
