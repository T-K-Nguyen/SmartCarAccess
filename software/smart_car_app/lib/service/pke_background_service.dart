import 'dart:async';
import 'dart:io';
import 'dart:ui';

import 'package:flutter/widgets.dart';
import 'package:flutter_background_service/flutter_background_service.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'nfc_provisioning_service.dart';
import 'pke_rollout_flags.dart';
import 'pke_telemetry.dart';

class PkeBackgroundService {
  static const String _channelId = 'pke_background_runtime';
  static const String _channelName = 'PKE Background Runtime';
  static const String _channelDescription =
      'Maintains authentication orchestration in background mode.';
  static const int _notificationId = 7411;

  static final FlutterBackgroundService _service = FlutterBackgroundService();
  static bool _configured = false;

  static Future<void> ensureForRollout(PkeRolloutFlags flags) async {
    if (!Platform.isAndroid) {
      return;
    }

    await _ensureConfigured();

    final running = await _service.isRunning();
    if (flags.backgroundMode) {
      if (!running) {
        await _service.startService();
      }
      return;
    }

    if (running) {
      _service.invoke('stopService');
    }
  }

  static Future<bool> isRunning() {
    if (!Platform.isAndroid) {
      return Future<bool>.value(false);
    }
    return _service.isRunning();
  }

  static Future<void> _ensureConfigured() async {
    if (_configured) {
      return;
    }

    await _createNotificationChannel();

    await _service.configure(
      androidConfiguration: AndroidConfiguration(
        onStart: onStart,
        autoStart: false,
        autoStartOnBoot: false,
        isForegroundMode: true,
        notificationChannelId: _channelId,
        initialNotificationTitle: 'Smart Car Background Runtime',
        initialNotificationContent: 'Preparing background orchestration',
        foregroundServiceNotificationId: _notificationId,
      ),
      iosConfiguration: IosConfiguration(
        autoStart: false,
        onForeground: onStart,
        onBackground: onIosBackground,
      ),
    );

    _configured = true;
  }

  static Future<void> _createNotificationChannel() async {
    const channel = AndroidNotificationChannel(
      _channelId,
      _channelName,
      description: _channelDescription,
      importance: Importance.low,
      playSound: false,
      enableVibration: false,
      showBadge: false,
    );

    final notificationsPlugin = FlutterLocalNotificationsPlugin();
    final androidImplementation = notificationsPlugin
        .resolvePlatformSpecificImplementation<
          AndroidFlutterLocalNotificationsPlugin
        >();
    await androidImplementation?.createNotificationChannel(channel);
  }
}

@pragma('vm:entry-point')
Future<bool> onIosBackground(ServiceInstance service) async {
  WidgetsFlutterBinding.ensureInitialized();
  return true;
}

@pragma('vm:entry-point')
void onStart(ServiceInstance service) async {
  WidgetsFlutterBinding.ensureInitialized();
  DartPluginRegistrant.ensureInitialized();

  await NfcProvisioningService.initialize(ownerIdHint: 'background');

  final telemetry = PkeTelemetry(source: 'app_bg');
  telemetry.startAttempt();
  telemetry.emit(
    event: PkeTelemetryEvent.scanWake,
    details: 'background_service_started',
  );

  if (service is AndroidServiceInstance) {
    service.setAsForegroundService();
    service.setForegroundNotificationInfo(
      title: 'Smart Car Background Runtime',
      content: 'Background orchestration active',
    );
  }

  Timer? heartbeat;

  void stopService() {
    heartbeat?.cancel();
    telemetry.emit(
      event: PkeTelemetryEvent.unlockDecision,
      unlockDecision: 'deny',
      details: 'background_service_stopped',
    );
    service.stopSelf();
  }

  service.on('stopService').listen((_) {
    stopService();
  });

  heartbeat = Timer.periodic(const Duration(seconds: 20), (_) {
    final now = DateTime.now();
    if (service is AndroidServiceInstance) {
      service.setForegroundNotificationInfo(
        title: 'Smart Car Background Runtime',
        content:
            'Active at ${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}:${now.second.toString().padLeft(2, '0')}',
      );
    }

    telemetry.emit(
      event: PkeTelemetryEvent.rssiSnapshot,
      details: 'background_service_heartbeat',
    );
  });
}
