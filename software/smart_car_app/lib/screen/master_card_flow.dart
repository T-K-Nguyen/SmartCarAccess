import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:smart_car_app/service/master_card_provisioning.dart';

class MasterCardFlowScreen extends StatefulWidget {
  const MasterCardFlowScreen({
    super.key,
    required this.payload,
    required this.targetName,
  });

  final MasterCardPayload payload;
  final String targetName;

  @override
  State<MasterCardFlowScreen> createState() => _MasterCardFlowScreenState();
}

enum _FlowStep {
  tapCar,
  success,
  error,
}

class _MasterCardFlowScreenState extends State<MasterCardFlowScreen> {
  final MasterCardProvisioningService _service = MasterCardProvisioningService();
  _FlowStep _step = _FlowStep.tapCar;
  bool _isBusy = false;
  String? _errorMessage;
  bool _provisioningActive = false;

  Timer? _countdownTimer;
  int _secondsLeft = 60;

  @override
  void dispose() {
    _countdownTimer?.cancel();
    _service.clearHceSession();
    super.dispose();
  }

  Future<void> _startProvisioning() async {
    if (_isBusy || _provisioningActive) return;
    setState(() {
      _isBusy = true;
      _errorMessage = null;
    });

    try {
      await HapticFeedback.mediumImpact();
      await _service.activateHceSession(widget.payload, ttl: const Duration(seconds: 60));
      _startCountdown();
      setState(() {
        _provisioningActive = true;
      });
    } catch (e) {
      setState(() {
        _step = _FlowStep.error;
        _errorMessage = e.toString();
      });
    } finally {
      if (mounted) {
        setState(() {
          _isBusy = false;
        });
      }
    }
  }

  void _startCountdown() {
    _countdownTimer?.cancel();
    _secondsLeft = 60;
    _countdownTimer = Timer.periodic(const Duration(seconds: 1), (timer) async {
      if (!mounted) return;
      setState(() {
        _secondsLeft -= 1;
      });
      if (_secondsLeft <= 0) {
        timer.cancel();
        await _service.clearHceSession();
        if (mounted) {
          setState(() {
            _step = _FlowStep.error;
            _errorMessage = 'Hết thời gian. Vui lòng thử lại.';
          });
        }
      }
    });
  }

  Future<void> _finishFlow() async {
    await _service.clearHceSession();
    if (!mounted) return;
    setState(() {
      _step = _FlowStep.success;
      _provisioningActive = false;
    });
  }

  Future<void> _resetFlow() async {
    _countdownTimer?.cancel();
    await _service.clearHceSession();
    if (!mounted) return;
    setState(() {
      _step = _FlowStep.tapCar;
      _errorMessage = null;
      _secondsLeft = 60;
      _provisioningActive = false;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Add New Digital Key'),
        backgroundColor: Colors.white,
        foregroundColor: const Color(0xFF273671),
        elevation: 0,
      ),
      backgroundColor: Colors.grey[50],
      body: SafeArea(
        child: AnimatedSwitcher(
          duration: const Duration(milliseconds: 250),
          child: _buildStep(context),
        ),
      ),
    );
  }

  Widget _buildStep(BuildContext context) {
    switch (_step) {
      case _FlowStep.tapCar:
        return _buildTapCar(context);
      case _FlowStep.success:
        return _buildSuccess(context);
      case _FlowStep.error:
        return _buildError(context);
    }
  }

  Widget _buildTapCar(BuildContext context) {
    return _buildCard(
      title: 'Bước 2: Chạm vào xe',
      description: 'Xe: ${widget.targetName}. Nhấn Provision để bật HCE, sau đó chạm điện thoại vào tay nắm cửa xe trong ${_secondsLeft}s.',
      icon: Icons.directions_car,
      actions: [
        ElevatedButton(
          onPressed: _isBusy ? null : _startProvisioning,
          style: ElevatedButton.styleFrom(
            backgroundColor: const Color(0xFF273671),
            foregroundColor: Colors.white,
            padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
          ),
          child: Text(_provisioningActive ? 'Provisioning...' : 'Provision'),
        ),
        const SizedBox(width: 12),
        OutlinedButton(
          onPressed: _resetFlow,
          style: OutlinedButton.styleFrom(
            foregroundColor: const Color(0xFF273671),
            side: const BorderSide(color: Color(0xFF273671)),
            padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
          ),
          child: const Text('Hủy'),
        ),
      ],
      footer: _provisioningActive ? _buildCountdownIndicator() : null,
    );
  }

  Widget _buildSuccess(BuildContext context) {
    return _buildCard(
      title: 'Hoàn tất',
      description: 'Đã kích hoạt phiên HCE. Bạn có thể tiếp tục bước cấu hình tiếp theo.',
      icon: Icons.check_circle,
      iconColor: Colors.green,
      actions: [
        ElevatedButton(
          onPressed: () => Navigator.pop(context, true),
          style: ElevatedButton.styleFrom(
            backgroundColor: const Color(0xFF273671),
            foregroundColor: Colors.white,
            padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
          ),
          child: const Text('Đóng'),
        ),
      ],
    );
  }

  Widget _buildError(BuildContext context) {
    return _buildCard(
      title: 'Có lỗi xảy ra',
      description: _errorMessage ?? 'Không thể hoàn tất quy trình. Vui lòng thử lại.',
      icon: Icons.error_outline,
      iconColor: Colors.red,
      actions: [
        ElevatedButton(
          onPressed: _resetFlow,
          style: ElevatedButton.styleFrom(
            backgroundColor: const Color(0xFF273671),
            foregroundColor: Colors.white,
            padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 12),
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
          ),
          child: const Text('Thử lại'),
        ),
      ],
    );
  }

  Widget _buildCountdownIndicator() {
    final progress = _secondsLeft / 60.0;
    return Column(
      children: [
        const SizedBox(height: 16),
        LinearProgressIndicator(
          value: progress.clamp(0.0, 1.0),
          backgroundColor: Colors.grey[300],
          color: const Color(0xFF41a5de),
          minHeight: 6,
        ),
        const SizedBox(height: 8),
        Text(
          'Còn $_secondsLeft giây',
          style: TextStyle(color: Colors.grey[600]),
        ),
      ],
    );
  }

  Widget _buildCard({
    required String title,
    required String description,
    required IconData icon,
    List<Widget> actions = const [],
    Widget? footer,
    Color? iconColor,
  }) {
    return Center(
      key: ValueKey<String>(title),
      child: Container(
        margin: const EdgeInsets.all(24),
        padding: const EdgeInsets.all(24),
        decoration: BoxDecoration(
          color: Colors.white,
          borderRadius: BorderRadius.circular(20),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withOpacity(0.05),
              blurRadius: 16,
              offset: const Offset(0, 6),
            ),
          ],
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: 64, color: iconColor ?? const Color(0xFF273671)),
            const SizedBox(height: 16),
            Text(
              title,
              style: const TextStyle(
                fontSize: 20,
                fontWeight: FontWeight.bold,
                color: Color(0xFF273671),
              ),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 12),
            Text(
              description,
              style: TextStyle(fontSize: 14, color: Colors.grey[700]),
              textAlign: TextAlign.center,
            ),
            if (footer != null) footer,
            const SizedBox(height: 20),
            Wrap(
              alignment: WrapAlignment.center,
              spacing: 12,
              children: actions,
            ),
          ],
        ),
      ),
    );
  }
}
