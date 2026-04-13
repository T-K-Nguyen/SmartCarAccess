import 'dart:async';
import 'dart:io';
import 'dart:ui';

import 'package:flutter/widgets.dart';
import 'package:flutter_background_service/flutter_background_service.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'nfc_provisioning_service.dart';
import 'pke_auth_orchestrator.dart';
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

  static Future<void> setPreferredDeviceAddress(String deviceAddress) {
    return PkeAuthOrchestrator.savePreferredDeviceAddress(deviceAddress);
  }

  static Future<void> triggerAuthOnce({String? deviceAddress}) async {
    if (!Platform.isAndroid) {
      return;
    }

    await _ensureConfigured();
    if (deviceAddress != null && deviceAddress.trim().isNotEmpty) {
      await PkeAuthOrchestrator.savePreferredDeviceAddress(deviceAddress);
    }

    final running = await _service.isRunning();
    if (!running) {
      await _service.startService();
    }

    _service.invoke('runAuthOnce', {
      if (deviceAddress != null) 'deviceAddress': deviceAddress,
    });
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
  final authOrchestrator = PkeAuthOrchestrator(telemetry: telemetry);
  var authInFlight = false;
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

  Future<void> runAuthOnce(String trigger, {String? deviceAddress}) async {
    if (authInFlight) {
      return;
    }

    authInFlight = true;
    try {
      final override = deviceAddress?.trim();
      if (override != null && override.isNotEmpty) {
        await PkeAuthOrchestrator.savePreferredDeviceAddress(override);
      }

      final target = (override != null && override.isNotEmpty)
          ? override
          : await PkeAuthOrchestrator.loadPreferredDeviceAddress();

      if (target == null || target.isEmpty) {
        telemetry.emit(
          event: PkeTelemetryEvent.scanWake,
          details: 'background_auth_skipped_no_target',
        );
        return;
      }

      telemetry.startAttempt();
      telemetry.emit(
        event: PkeTelemetryEvent.scanWake,
        details: 'background_auth_trigger_$trigger',
      );

      final result = await authOrchestrator.authenticate(
        deviceAddress: target,
        timeout: const Duration(seconds: 25),
      );

      if (result.success) {
        telemetry.emit(
          event: PkeTelemetryEvent.unlockDecision,
          unlockDecision: 'allow',
          details: 'background_auth_success',
        );
      } else {
        telemetry.emit(
          event: PkeTelemetryEvent.unlockDecision,
          unlockDecision: 'deny',
          details: 'background_auth_failed',
        );
      }
    } catch (_) {
      telemetry.emit(
        event: PkeTelemetryEvent.unlockDecision,
        unlockDecision: 'deny',
        details: 'background_auth_exception',
      );
    } finally {
      await authOrchestrator.disconnect();
      authInFlight = false;
    }
  }

  void stopService() {
    heartbeat?.cancel();
    unawaited(authOrchestrator.disconnect());
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

  service.on('runAuthOnce').listen((event) {
    final payload = event is Map<dynamic, dynamic> ? event : null;
    final deviceAddress = payload?['deviceAddress'] as String?;
    runAuthOnce('invoke', deviceAddress: deviceAddress);
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

  unawaited(runAuthOnce('startup'));
}
