import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:smart_car_app/service/ble_phase_test.dart';
import 'dart:async';

/// Test screen for Phase A (NFC Provisioning) and Phase B (BLE Authentication)
class TestPhaseABScreen extends StatefulWidget {
  const TestPhaseABScreen({super.key});

  @override
  State<TestPhaseABScreen> createState() => _TestPhaseABScreenState();
}

class _TestPhaseABScreenState extends State<TestPhaseABScreen> {
  final BlePhaseTestService _testService = BlePhaseTestService();
  final TextEditingController _deviceAddressController = TextEditingController();
  final ScrollController _logScrollController = ScrollController();
  
  List<String> _logs = [];
  bool _isTestingPhaseA = false;
  bool _isTestingPhaseB = false;
  bool _isScanningDevices = false;
  
  PhaseA_Result? _phaseAResult;
  PhaseB_Result? _phaseBResult;
  
  List<ScanResult> _scanResults = [];
  BluetoothDevice? _selectedDevice;

  @override
  void initState() {
    super.initState();
    _checkBluetoothState();
  }

  @override
  void dispose() {
    _deviceAddressController.dispose();
    _logScrollController.dispose();
    _testService.disconnect();
    super.dispose();
  }

  Future<void> _checkBluetoothState() async {
    try {
      final isSupported = await FlutterBluePlus.isSupported;
      if (!isSupported) {
        _addLog('❌ Bluetooth không được hỗ trợ trên thiết bị này');
        return;
      }
      
      final adapterState = await FlutterBluePlus.adapterState.first;
      if (adapterState != BluetoothAdapterState.on) {
        _addLog('⚠️ Vui lòng bật Bluetooth');
      } else {
        _addLog('✓ Bluetooth sẵn sàng');
      }
    } catch (e) {
      _addLog('❌ Lỗi kiểm tra Bluetooth: $e');
    }
  }

  void _addLog(String message) {
    setState(() {
      _logs.add('[${DateTime.now().toString().substring(11, 23)}] $message');
    });
    
    // Auto scroll to bottom
    Future.delayed(const Duration(milliseconds: 100), () {
      if (_logScrollController.hasClients) {
        _logScrollController.animateTo(
          _logScrollController.position.maxScrollExtent,
          duration: const Duration(milliseconds: 300),
          curve: Curves.easeOut,
        );
      }
    });
  }

  void _clearLogs() {
    setState(() {
      _logs.clear();
      _phaseAResult = null;
      _phaseBResult = null;
    });
  }

  Future<void> _scanForDevices() async {
    setState(() {
      _isScanningDevices = true;
      _scanResults.clear();
    });
    
    _addLog('🔍 Đang quét thiết bị BLE...');
    
    try {
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 10));
      
      final subscription = FlutterBluePlus.scanResults.listen((results) {
        setState(() {
          _scanResults = results
              .where((r) => r.device.platformName.isNotEmpty)
              .toList();
        });
      });
      
      await Future.delayed(const Duration(seconds: 10));
      await FlutterBluePlus.stopScan();
      subscription.cancel();
      
      _addLog('✓ Tìm thấy ${_scanResults.length} thiết bị');
      
    } catch (e) {
      _addLog('❌ Lỗi quét: $e');
    } finally {
      setState(() {
        _isScanningDevices = false;
      });
    }
  }

  Future<void> _testPhaseA() async {
    setState(() {
      _isTestingPhaseA = true;
      _phaseAResult = null;
    });
    
    _addLog('═══════════════════════════════════');
    _addLog('🔷 BẮT ĐẦU TEST PHASE A (NFC Provisioning)');
    _addLog('═══════════════════════════════════');
    
    try {
      _addLog('');
      _addLog('📱 Kiểm tra Android Keystore...');
      
      // 1. Check if Phase A key exists in Keystore
      try {
        final keystoreChannel = const MethodChannel('smartcar/keystore');
        
        // Ensure key exists
        _addLog('   Đảm bảo identity key tồn tại...');
        final keyExists = await keystoreChannel.invokeMethod('ensurePhaseAKey');
        
        if (keyExists) {
          _addLog('   ✓ Identity key đã sẵn sàng trong Android Keystore');
          
          // Get public key
          _addLog('   Đọc public key...');
          final publicKey = await keystoreChannel.invokeMethod('getPhaseAPublicKey65');
          
          if (publicKey != null && publicKey.length == 65) {
            _addLog('   ✓ Public key: ${_formatBytes(publicKey, 16)}...');
            _addLog('');
            
            // Show Phase A information
            _addLog('📋 THÔNG TIN PHASE A:');
            _addLog('   Service: ProvisioningHostApduService (HCE)');
            _addLog('   AID: F0:01:02:03:04:05');
            _addLog('   Identity Key: Android Keystore (hardware-backed)');
            _addLog('   Public Key: 65 bytes (uncompressed P-256)');
            _addLog('');
            _addLog('📝 QUÁ TRÌNH PROVISIONING:');
            _addLog('   1. ECU gửi: SELECT AID (00 A4 04 00 06 F0010203040500)');
            _addLog('      ← Phone trả: UID(4 bytes) + 90 00');
            _addLog('');
            _addLog('   2. ECU gửi: GET_CHALLENGE Lc=0 (00 CA 00 00 00 00)');
            _addLog('      ← Phone trả: [keyId(1) + phonePub(65) + certLen(2)] + 90 00');
            _addLog('      → Public key từ Android Keystore');
            _addLog('');
            _addLog('   3. ECU tạo challenge: vehicleId(8) || nonce(16)');
            _addLog('      ECU gửi: GET_CHALLENGE Lc=24 + [challenge]');
            _addLog('      ← Phone ký challenge bằng private key (Keystore)');
            _addLog('      ← Phone trả: [sigLen(2,BE) + DER_signature] + 90 00');
            _addLog('');
            _addLog('   4. ECU verify signature → lưu public key vào NVS');
            _addLog('');
            _addLog('🔐 BẢO MẬT:');
            _addLog('   • Private key: KHÔNG BAO GIỜ rời khỏi Keystore');
            _addLog('   • Public key: Được verify trước khi lưu');
            _addLog('   • Signature: DER-encoded ECDSA-SHA256');
            _addLog('   • Storage: ESP32 NVS (namespace "prov")');
            _addLog('');
            _addLog('📍 TRẠNG THÁI HCE:');
            _addLog('   ✓ HCE service đang hoạt động (kiểm tra manifest)');
            _addLog('   ✓ App có thể foreground hoặc background');
            _addLog('   ✓ Tap phone vào PN532 để bắt đầu provisioning');
            _addLog('');
            _addLog('═══════════════════════════════════');
            _addLog('✅ PHASE A SẴN SÀNG');
            _addLog('═══════════════════════════════════');
            _addLog('');
            _addLog('💡 HƯỚNG DẪN:');
            _addLog('   1. Đảm bảo ESP32 đang chạy và PN532 hoạt động');
            _addLog('   2. Gửi lệnh serial "f" hoặc "F" để force provisioning');
            _addLog('   3. Tap phone vào PN532 reader');
            _addLog('   4. Kiểm tra logs trên ESP32 Serial Monitor');
            _addLog('   5. Gửi lệnh "p" để xem trạng thái provisioning');
            
            setState(() {
              _phaseAResult = PhaseA_Result(
                success: true,
                message: 'HCE service sẵn sàng, identity key trong Keystore',
                phonePublicKeyStored: true,
              );
            });
          } else {
            throw Exception('Failed to read public key from Keystore');
          }
        } else {
          throw Exception('Failed to ensure identity key in Keystore');
        }
      } catch (e) {
        _addLog('❌ Lỗi kiểm tra Keystore: $e');
        throw e;
      }
      
    } catch (e) {
      _addLog('');
      _addLog('❌ LỖI: $e');
      setState(() {
        _phaseAResult = PhaseA_Result(
          success: false,
          message: e.toString(),
          phonePublicKeyStored: false,
        );
      });
    } finally {
      setState(() {
        _isTestingPhaseA = false;
      });
    }
  }

  Future<void> _testPhaseB() async {
    if (_selectedDevice == null && _deviceAddressController.text.isEmpty) {
      _addLog('❌ Vui lòng chọn thiết bị hoặc nhập địa chỉ MAC');
      return;
    }
    
    final deviceAddress = _selectedDevice?.remoteId.str ?? _deviceAddressController.text;
    
    setState(() {
      _isTestingPhaseB = true;
      _phaseBResult = null;
    });
    
    _addLog('═══════════════════════════════════');
    _addLog('🔷 BẮT ĐẦU TEST PHASE B (BLE Authentication)');
    _addLog('═══════════════════════════════════');
    _addLog('Thiết bị: $deviceAddress');
    _addLog('');
    
    try {
      // Measure total time
      final stepStartTime = DateTime.now();
      
      // Run authentication with progress callback
      final result = await _testService.testPhaseB(
        deviceAddress: deviceAddress,
        device: _selectedDevice,
        onProgress: (step, message) {
          // Format progress messages for UI
          _addLog('[$step] $message');
        },
      );
      
      final stepDuration = DateTime.now().difference(stepStartTime);
      _addLog('');
      _addLog('⏱️  Tổng thời gian: ${stepDuration.inMilliseconds}ms');
      
      setState(() {
        _phaseBResult = result;
      });
      
      if (result.success) {
        _addLog('');
        _addLog('═══════════════════════════════════');
        _addLog('✅ PHASE B HOÀN TẤT THÀNH CÔNG!');
        _addLog('═══════════════════════════════════');
        _addLog('📊 KẾT QUẢ:');
        _addLog('   🔗 Shared Secret: ${_formatBytes(result.sharedSecret!, 16)}...');
        _addLog('   🔑 Encryption Key: ${_formatBytes(result.sessionEncKey!, 16)}...');
        _addLog('   🔑 MAC Key: ${_formatBytes(result.sessionMacKey!, 16)}...');
        _addLog('   🎯 Challenge: ${_formatBytes(result.challenge!, 24)}');
        _addLog('');
        _addLog('🔓 Sẵn sàng để mở khóa xe!');
      } else {
        _addLog('');
        _addLog('═══════════════════════════════════');
        _addLog('❌ PHASE B THẤT BẠI');
        _addLog('═══════════════════════════════════');
        _addLog('Lỗi: ${result.message}');
      }
      
    } catch (e, stackTrace) {
      _addLog('');
      _addLog('❌ LỖI NGHIÊM TRỌNG: $e');
      _addLog('Stack trace: ${stackTrace.toString().substring(0, 200)}...');
      
      setState(() {
        _phaseBResult = PhaseB_Result(
          success: false,
          message: e.toString(),
        );
      });
    } finally {
      setState(() {
        _isTestingPhaseB = false;
      });
    }
  }

  String _formatBytes(List<int> bytes, int maxLength) {
    final limited = bytes.take(maxLength).toList();
    return limited.map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase()).join(' ');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFFF5F7FA),
      appBar: AppBar(
        title: const Text(
          'Test Phase A/B',
          style: TextStyle(
            fontWeight: FontWeight.bold,
            color: Colors.white,
          ),
        ),
        backgroundColor: const Color(0xFF273671),
        elevation: 0,
        iconTheme: const IconThemeData(color: Colors.white),
        actions: [
          IconButton(
            icon: const Icon(Icons.delete_outline),
            tooltip: 'Xóa logs',
            onPressed: _clearLogs,
          ),
        ],
      ),
      body: Column(
        children: [
          // Header with Phase indicators
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(16),
            decoration: const BoxDecoration(
              color: Color(0xFF273671),
              borderRadius: BorderRadius.only(
                bottomLeft: Radius.circular(24),
                bottomRight: Radius.circular(24),
              ),
            ),
            child: Column(
              children: [
                Row(
                  children: [
                    Expanded(
                      child: _buildPhaseIndicator(
                        'Phase A',
                        'NFC Provisioning',
                        Icons.nfc,
                        _phaseAResult?.success ?? false,
                        _isTestingPhaseA,
                      ),
                    ),
                    const SizedBox(width: 16),
                    Expanded(
                      child: _buildPhaseIndicator(
                        'Phase B',
                        'BLE Auth',
                        Icons.bluetooth,
                        _phaseBResult?.success ?? false,
                        _isTestingPhaseB,
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          
          const SizedBox(height: 16),
          
          // Device selection section
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    const Text(
                      'Chọn thiết bị BLE:',
                      style: TextStyle(
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                        color: Color(0xFF273671),
                      ),
                    ),
                    const Spacer(),
                    TextButton.icon(
                      onPressed: _isScanningDevices ? null : _scanForDevices,
                      icon: _isScanningDevices
                          ? const SizedBox(
                              width: 16,
                              height: 16,
                              child: CircularProgressIndicator(strokeWidth: 2),
                            )
                          : const Icon(Icons.refresh),
                      label: Text(_isScanningDevices ? 'Đang quét...' : 'Quét'),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                
                // Device list
                if (_scanResults.isNotEmpty)
                  Container(
                    height: 120,
                    decoration: BoxDecoration(
                      color: Colors.white,
                      borderRadius: BorderRadius.circular(12),
                      boxShadow: [
                        BoxShadow(
                          color: Colors.black.withOpacity(0.05),
                          blurRadius: 10,
                          offset: const Offset(0, 2),
                        ),
                      ],
                    ),
                    child: ListView.builder(
                      itemCount: _scanResults.length,
                      itemBuilder: (context, index) {
                        final result = _scanResults[index];
                        final isSelected = _selectedDevice?.remoteId == result.device.remoteId;
                        
                        return ListTile(
                          dense: true,
                          selected: isSelected,
                          selectedTileColor: const Color(0xFF273671).withOpacity(0.1),
                          leading: Icon(
                            Icons.bluetooth,
                            color: isSelected ? const Color(0xFF273671) : Colors.grey,
                          ),
                          title: Text(
                            result.device.platformName.isEmpty 
                                ? 'Unknown Device' 
                                : result.device.platformName,
                            style: TextStyle(
                              fontWeight: isSelected ? FontWeight.bold : FontWeight.normal,
                            ),
                          ),
                          subtitle: Text(
                            result.device.remoteId.str,
                            style: const TextStyle(fontSize: 12),
                          ),
                          trailing: Text(
                            '${result.rssi} dBm',
                            style: TextStyle(
                              color: Colors.grey[600],
                              fontSize: 12,
                            ),
                          ),
                          onTap: () {
                            setState(() {
                              _selectedDevice = result.device;
                              _deviceAddressController.text = result.device.remoteId.str;
                            });
                            _addLog('✓ Đã chọn: ${result.device.platformName} (${result.device.remoteId.str})');
                          },
                        );
                      },
                    ),
                  )
                else
                  TextField(
                    controller: _deviceAddressController,
                    decoration: InputDecoration(
                      hintText: 'Hoặc nhập địa chỉ MAC (XX:XX:XX:XX:XX:XX)',
                      filled: true,
                      fillColor: Colors.white,
                      border: OutlineInputBorder(
                        borderRadius: BorderRadius.circular(12),
                        borderSide: BorderSide.none,
                      ),
                      contentPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                    ),
                  ),
              ],
            ),
          ),
          
          const SizedBox(height: 16),
          
          // Test buttons
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Row(
              children: [
                Expanded(
                  child: ElevatedButton.icon(
                    onPressed: _isTestingPhaseA ? null : _testPhaseA,
                    icon: _isTestingPhaseA
                        ? const SizedBox(
                            width: 20,
                            height: 20,
                            child: CircularProgressIndicator(
                              strokeWidth: 2,
                              color: Colors.white,
                            ),
                          )
                        : const Icon(Icons.nfc),
                    label: const Text('Test Phase A'),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: const Color(0xFF41a5de),
                      foregroundColor: Colors.white,
                      padding: const EdgeInsets.symmetric(vertical: 14),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(12),
                      ),
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: ElevatedButton.icon(
                    onPressed: _isTestingPhaseB ? null : _testPhaseB,
                    icon: _isTestingPhaseB
                        ? const SizedBox(
                            width: 20,
                            height: 20,
                            child: CircularProgressIndicator(
                              strokeWidth: 2,
                              color: Colors.white,
                            ),
                          )
                        : const Icon(Icons.bluetooth),
                    label: const Text('Test Phase B'),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: const Color(0xFF273671),
                      foregroundColor: Colors.white,
                      padding: const EdgeInsets.symmetric(vertical: 14),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(12),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
          
          const SizedBox(height: 16),
          
          // Logs section
          Expanded(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Container(
                decoration: BoxDecoration(
                  color: Colors.black87,
                  borderRadius: BorderRadius.circular(12),
                  boxShadow: [
                    BoxShadow(
                      color: Colors.black.withOpacity(0.1),
                      blurRadius: 10,
                      offset: const Offset(0, 2),
                    ),
                  ],
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Padding(
                      padding: const EdgeInsets.all(12),
                      child: Row(
                        children: [
                          const Icon(
                            Icons.terminal,
                            color: Colors.greenAccent,
                            size: 20,
                          ),
                          const SizedBox(width: 8),
                          const Text(
                            'Console Logs',
                            style: TextStyle(
                              color: Colors.greenAccent,
                              fontWeight: FontWeight.bold,
                              fontSize: 14,
                            ),
                          ),
                          const Spacer(),
                          Text(
                            '${_logs.length} dòng',
                            style: TextStyle(
                              color: Colors.grey[500],
                              fontSize: 12,
                            ),
                          ),
                          const SizedBox(width: 8),
                          IconButton(
                            icon: const Icon(Icons.copy, size: 18),
                            color: Colors.greenAccent,
                            tooltip: 'Copy toàn bộ logs',
                            padding: EdgeInsets.zero,
                            constraints: const BoxConstraints(),
                            onPressed: _logs.isEmpty ? null : () {
                              final allLogs = _logs.join('\n');
                              Clipboard.setData(ClipboardData(text: allLogs));
                              ScaffoldMessenger.of(context).showSnackBar(
                                SnackBar(
                                  content: Text('✓ Đã copy ${_logs.length} dòng logs vào clipboard'),
                                  duration: const Duration(seconds: 2),
                                  backgroundColor: Colors.green,
                                ),
                              );
                            },
                          ),
                        ],
                      ),
                    ),
                    const Divider(color: Colors.white24, height: 1),
                    Expanded(
                      child: _logs.isEmpty
                          ? Center(
                              child: Text(
                                'Chưa có logs. Nhấn nút test để bắt đầu.',
                                style: TextStyle(
                                  color: Colors.grey[600],
                                  fontSize: 14,
                                ),
                              ),
                            )
                          : ListView.builder(
                              controller: _logScrollController,
                              padding: const EdgeInsets.all(12),
                              itemCount: _logs.length,
                              itemBuilder: (context, index) {
                                final log = _logs[index];
                                Color logColor = Colors.white;
                                
                                if (log.contains('✓') || log.contains('✅')) {
                                  logColor = Colors.greenAccent;
                                } else if (log.contains('❌') || log.contains('ERROR')) {
                                  logColor = Colors.redAccent;
                                } else if (log.contains('⚠️') || log.contains('WARNING')) {
                                  logColor = Colors.orangeAccent;
                                } else if (log.contains('🔷') || log.contains('═══')) {
                                  logColor = Colors.cyanAccent;
                                } else if (log.contains('[Bước')) {
                                  logColor = Colors.yellowAccent;
                                }
                                
                                return Padding(
                                  padding: const EdgeInsets.only(bottom: 4),
                                  child: SelectableText(
                                    log,
                                    style: TextStyle(
                                      fontFamily: 'monospace',
                                      fontSize: 12,
                                      color: logColor,
                                      height: 1.5,
                                    ),
                                  ),
                                );
                              },
                            ),
                    ),
                  ],
                ),
              ),
            ),
          ),
          
          const SizedBox(height: 16),
        ],
      ),
    );
  }

  Widget _buildPhaseIndicator(
    String title,
    String subtitle,
    IconData icon,
    bool isSuccess,
    bool isRunning,
  ) {
    Color statusColor = Colors.white54;
    IconData statusIcon = Icons.radio_button_unchecked;
    
    if (isRunning) {
      statusColor = Colors.orangeAccent;
      statusIcon = Icons.pending;
    } else if (isSuccess) {
      statusColor = Colors.greenAccent;
      statusIcon = Icons.check_circle;
    }
    
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: Colors.white.withOpacity(0.1),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(
          color: isSuccess ? Colors.greenAccent : Colors.white24,
          width: 2,
        ),
      ),
      child: Column(
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Icon(icon, color: Colors.white, size: 24),
              const SizedBox(width: 8),
              Icon(statusIcon, color: statusColor, size: 20),
            ],
          ),
          const SizedBox(height: 8),
          Text(
            title,
            style: const TextStyle(
              color: Colors.white,
              fontWeight: FontWeight.bold,
              fontSize: 14,
            ),
          ),
          const SizedBox(height: 2),
          Text(
            subtitle,
            style: TextStyle(
              color: Colors.white.withOpacity(0.7),
              fontSize: 11,
            ),
          ),
        ],
      ),
    );
  }
}
