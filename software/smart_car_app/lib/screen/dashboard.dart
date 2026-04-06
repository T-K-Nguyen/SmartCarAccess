import 'package:flutter/material.dart';
import 'package:smart_car_app/screen/location.dart';
import 'package:smart_car_app/screen/profile.dart';
import 'package:smart_car_app/screen/test_phase_ab.dart';
import 'package:smart_car_app/screen/master_card_flow.dart';
import 'package:smart_car_app/widgets/car_dialogs.dart';
import 'package:smart_car_app/widgets/dashboard_widgets.dart';
import 'package:smart_car_app/service/car_service.dart';
import 'package:smart_car_app/service/initial_data_helper.dart';
import 'package:smart_car_app/service/master_card_provisioning.dart';
import 'package:flutter/services.dart';
import 'package:cloud_firestore/cloud_firestore.dart';

class Dashboard extends StatefulWidget {
  const Dashboard({super.key});

  @override
  State<Dashboard> createState() => _DashboardState();
}

class _DashboardState extends State<Dashboard> {
  int _currentIndex = 0;
  final CarService _carService = CarService();
  final MasterCardProvisioningService _masterCardService =
      MasterCardProvisioningService();

  List<Map<String, dynamic>> _cars = [];
  List<Map<String, dynamic>> _digitalKeys = [];
  bool _isLoading = true;
  final Map<String, MasterCardPayload> _pendingVehicleProvision = {};

  @override
  void initState() {
    super.initState();
    _loadData();
    // Show sample data dialog after a short delay
    Future.delayed(const Duration(milliseconds: 500), () {
      if (mounted) {
        InitialDataHelper.addSampleDataIfNeeded(context);
      }
    });
  }

  void _loadData() {
    // Listen to cars stream
    _carService.getUserCars().listen(
      (cars) {
        print('Loaded ${cars.length} cars: $cars'); // Debug log
        if (mounted) {
          final normalizedCars = cars.map((car) {
            // Convert color string to Color object if needed
            if (car['color'] is String) {
              switch (car['color']) {
                case 'blue':
                  car['color'] = Colors.blue;
                  break;
                case 'purple':
                  car['color'] = Colors.purple;
                  break;
                case 'orange':
                  car['color'] = Colors.orange;
                  break;
                case 'green':
                  car['color'] = Colors.green;
                  break;
                default:
                  car['color'] = Colors.blue;
              }
            }
            return car;
          }).toList();
          setState(() {
            _cars = normalizedCars;
            _isLoading = false;
          });
          _refreshPendingProvision(normalizedCars);
        }
      },
      onError: (error) {
        print('Error loading cars: $error');
        if (mounted) {
          setState(() {
            _isLoading = false;
          });
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text('Failed to load vehicles: $error'),
              backgroundColor: Colors.red,
            ),
          );
        }
      },
    ); // Listen to digital keys stream
    _carService.getUserDigitalKeys().listen(
      (keys) {
        print('Loaded ${keys.length} digital keys: $keys'); // Debug log
        if (mounted) {
          setState(() {
            _digitalKeys = keys;
          });
        }
      },
      onError: (error) {
        print('Error loading digital keys: $error');
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text('Failed to load digital keys: $error'),
              backgroundColor: Colors.red,
            ),
          );
        }
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.grey[50],
      appBar: _buildAppBar(),
      body: SafeArea(
        child: _currentIndex == 3
            ? const ProfileContent()
            : _currentIndex == 2
            ? const LocationContent()
            : _buildDashboardContent(),
      ),
      bottomNavigationBar: _buildBottomNavigationBar(),
      floatingActionButton: _currentIndex == 0
          ? FloatingActionButton(
              onPressed: _showAddCarDialog,
              backgroundColor: const Color(0xFF273671),
              child: const Icon(Icons.add, color: Colors.white),
            )
          : _currentIndex == 1
          ? FloatingActionButton(
              onPressed: _showAddDigitalKeyDialog,
              backgroundColor: const Color(0xFF273671),
              child: const Icon(Icons.vpn_key, color: Colors.white),
            )
          : null,
    );
  }

  PreferredSizeWidget _buildAppBar() {
    String title = '';
    switch (_currentIndex) {
      case 0:
        title = 'Home';
        break;
      case 1:
        title = 'Digital Keys';
        break;
      case 2:
        title = 'Location';
        break;
      case 3:
        title = 'Profile';
        break;
    }

    return AppBar(
      title: Text(
        title,
        style: const TextStyle(
          fontSize: 24,
          fontWeight: FontWeight.bold,
          color: Color(0xFF273671),
        ),
      ),
      backgroundColor: Colors.transparent,
      elevation: 0,
      centerTitle: false,
      actions: [
        IconButton(
          icon: const Icon(Icons.science_outlined, color: Color(0xFF273671)),
          tooltip: 'Test Phase A/B',
          onPressed: () {
            Navigator.of(context).push(
              MaterialPageRoute(builder: (_) => const TestPhaseABScreen()),
            );
          },
        ),
        IconButton(
          icon: const Icon(
            Icons.notifications_outlined,
            color: Color(0xFF273671),
          ),
          onPressed: () {
            // Handle notifications
          },
        ),
        IconButton(
          icon: const Icon(Icons.settings_outlined, color: Color(0xFF273671)),
          onPressed: () {
            // Handle settings
          },
        ),
      ],
    );
  }

  Widget _buildDashboardContent() {
    if (_isLoading) {
      return const Center(
        child: CircularProgressIndicator(color: Color(0xFF273671)),
      );
    }

    return SingleChildScrollView(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          if (_currentIndex == 0) ...[
            _buildQuickStats(),
            const SizedBox(height: 24),
            _buildMyVehicles(),
          ] else ...[
            _buildDigitalKeysSection(),
          ],
        ],
      ),
    );
  }

  Widget _buildQuickStats() {
    return Column(
      children: [
        Row(
          children: [
            Expanded(
              child: StatCard(
                title: 'Total Cars',
                value: '${_cars.length}',
                icon: Icons.directions_car,
                color: const Color(0xFF273671),
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: StatCard(
                title: 'Active Keys',
                value:
                    '${_digitalKeys.where((k) => k['status'] == 'Active').length}',
                icon: Icons.vpn_key,
                color: const Color(0xFF41a5de),
              ),
            ),
          ],
        ),
        const SizedBox(height: 16),
        Row(
          children: [
            Expanded(
              child: StatCard(
                title: 'Available',
                value:
                    '${_cars.where((c) => c['keyStatus'] == 'Active').length}',
                icon: Icons.check_circle,
                color: Colors.green,
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: QuickActionCard(
                title: 'Quick Actions',
                subtitle: 'Control your vehicles',
                icon: Icons.touch_app,
                color: const Color(0xFF273671),
                onTap: () {
                  setState(() {
                    _currentIndex = 1;
                  });
                },
              ),
            ),
          ],
        ),
      ],
    );
  }

  Widget _buildMyVehicles() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'My Vehicles',
          style: TextStyle(
            fontSize: 20,
            fontWeight: FontWeight.bold,
            color: Color(0xFF273671),
          ),
        ),
        const SizedBox(height: 16),
        _cars.isEmpty
            ? Container(
                padding: const EdgeInsets.all(32),
                decoration: BoxDecoration(
                  color: Colors.white,
                  borderRadius: BorderRadius.circular(16),
                  boxShadow: [
                    BoxShadow(
                      color: Colors.black.withOpacity(0.05),
                      blurRadius: 10,
                      offset: const Offset(0, 2),
                    ),
                  ],
                ),
                child: Center(
                  child: Column(
                    children: [
                      Icon(
                        Icons.directions_car_outlined,
                        size: 64,
                        color: Colors.grey[400],
                      ),
                      const SizedBox(height: 16),
                      Text(
                        'No vehicles added yet',
                        style: TextStyle(
                          fontSize: 18,
                          fontWeight: FontWeight.w500,
                          color: Colors.grey[600],
                        ),
                      ),
                      const SizedBox(height: 8),
                      Text(
                        'Tap the + button to add your first vehicle',
                        style: TextStyle(fontSize: 14, color: Colors.grey[500]),
                      ),
                      const SizedBox(height: 16),
                      OutlinedButton.icon(
                        onPressed: () {
                          InitialDataHelper.addSampleDataIfNeeded(context);
                        },
                        icon: const Icon(Icons.auto_awesome),
                        label: const Text('Add Sample Data'),
                        style: OutlinedButton.styleFrom(
                          foregroundColor: const Color(0xFF273671),
                        ),
                      ),
                    ],
                  ),
                ),
              )
            : ListView.builder(
                shrinkWrap: true,
                physics: const NeverScrollableScrollPhysics(),
                itemCount: _cars.length,
                itemBuilder: (context, index) {
                  final car = _cars[index];
                  final carId = car['id'] as String?;
                  final provisioned = car['provisioned'] == true;
                  final pendingPayload = (carId != null)
                      ? _pendingVehicleProvision[carId]
                      : null;

                  return Column(
                    children: [
                      VehicleStatusCard(
                        vehicle: car,
                        onTap: () => _handleVehicleControlTap(car),
                        onLockToggle: () => _toggleLock(car),
                        onControl: () => _handleVehicleControlTap(car),
                        onDelete: () => _confirmDeleteVehicle(car),
                        isProvisioned: provisioned,
                      ),
                      if (!provisioned && pendingPayload != null) ...[
                        Padding(
                          padding: const EdgeInsets.only(bottom: 16),
                          child: SizedBox(
                            width: double.infinity,
                            child: ElevatedButton.icon(
                              onPressed: () => _startProvisionForVehicle(
                                car,
                                pendingPayload,
                              ),
                              icon: const Icon(Icons.nfc),
                              label: const Text('Provision for this vehicle'),
                              style: ElevatedButton.styleFrom(
                                backgroundColor: const Color(0xFF273671),
                                foregroundColor: Colors.white,
                                shape: RoundedRectangleBorder(
                                  borderRadius: BorderRadius.circular(12),
                                ),
                              ),
                            ),
                          ),
                        ),
                      ],
                    ],
                  );
                },
              ),
      ],
    );
  }

  Widget _buildDigitalKeysSection() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            const Expanded(
              child: Text(
                'Digital Keys',
                style: TextStyle(
                  fontSize: 20,
                  fontWeight: FontWeight.bold,
                  color: Color(0xFF273671),
                ),
              ),
            ),
            ElevatedButton.icon(
              onPressed: _showAddDigitalKeyDialog,
              icon: const Icon(Icons.add),
              label: const Text('Add Key'),
              style: ElevatedButton.styleFrom(
                backgroundColor: const Color(0xFF273671),
                foregroundColor: Colors.white,
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(12),
                ),
              ),
            ),
          ],
        ),
        const SizedBox(height: 16),
        _digitalKeys.isEmpty
            ? Container(
                padding: const EdgeInsets.all(32),
                decoration: BoxDecoration(
                  color: Colors.white,
                  borderRadius: BorderRadius.circular(16),
                  boxShadow: [
                    BoxShadow(
                      color: Colors.black.withOpacity(0.05),
                      blurRadius: 10,
                      offset: const Offset(0, 2),
                    ),
                  ],
                ),
                child: Center(
                  child: Column(
                    children: [
                      Icon(
                        Icons.vpn_key_outlined,
                        size: 64,
                        color: Colors.grey[400],
                      ),
                      const SizedBox(height: 16),
                      Text(
                        'No digital keys created yet',
                        style: TextStyle(
                          fontSize: 18,
                          fontWeight: FontWeight.w500,
                          color: Colors.grey[600],
                        ),
                      ),
                      const SizedBox(height: 8),
                      Text(
                        'Create digital keys to share vehicle access',
                        style: TextStyle(fontSize: 14, color: Colors.grey[500]),
                      ),
                    ],
                  ),
                ),
              )
            : ListView.builder(
                shrinkWrap: true,
                physics: const NeverScrollableScrollPhysics(),
                itemCount: _digitalKeys.length,
                itemBuilder: (context, index) {
                  return _buildDigitalKeyCard(_digitalKeys[index]);
                },
              ),
      ],
    );
  }

  Widget _buildDigitalKeyCard(Map<String, dynamic> key) {
    final car = _cars.firstWhere(
      (c) => c['id'] == key['carId'],
      orElse: () => {},
    );

    return Container(
      margin: const EdgeInsets.only(bottom: 16),
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(16),
        boxShadow: [
          BoxShadow(
            color: Colors.black.withOpacity(0.05),
            blurRadius: 10,
            offset: const Offset(0, 2),
          ),
        ],
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 50,
                height: 50,
                decoration: BoxDecoration(
                  color: const Color(0xFF273671).withOpacity(0.1),
                  borderRadius: BorderRadius.circular(12),
                ),
                child: const Icon(
                  Icons.vpn_key,
                  color: Color(0xFF273671),
                  size: 24,
                ),
              ),
              const SizedBox(width: 16),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      key['name'],
                      style: const TextStyle(
                        fontSize: 16,
                        fontWeight: FontWeight.bold,
                        color: Color(0xFF273671),
                      ),
                    ),
                    Text(
                      car.isNotEmpty ? car['name'] : 'Unknown Vehicle',
                      style: TextStyle(color: Colors.grey[600], fontSize: 14),
                    ),
                  ],
                ),
              ),
              _buildStatusChip(key['status']),
              IconButton(
                onPressed: () => _confirmDeleteDigitalKey(key),
                icon: const Icon(Icons.delete_outline),
                color: Colors.red[400],
                tooltip: 'Delete key',
              ),
            ],
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(
                child: Text(
                  'Type: ${key['type']}',
                  style: TextStyle(color: Colors.grey[600], fontSize: 12),
                ),
              ),
              Expanded(
                child: Text(
                  'Valid until: ${_formatDate(key['validUntil'])}',
                  style: TextStyle(color: Colors.grey[600], fontSize: 12),
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 8,
            children: (key['permissions'] as List<dynamic>).cast<String>().map((
              permission,
            ) {
              return Chip(
                label: Text(
                  permission.replaceAll('_', ' ').toUpperCase(),
                  style: const TextStyle(fontSize: 10),
                ),
                backgroundColor: const Color(0xFF41a5de).withOpacity(0.1),
                side: BorderSide.none,
              );
            }).toList(),
          ),
        ],
      ),
    );
  }

  Widget _buildStatusChip(String status) {
    Color color = status == 'Active' ? Colors.green : Colors.orange;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      decoration: BoxDecoration(
        color: color.withOpacity(0.1),
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: color.withOpacity(0.3)),
      ),
      child: Text(
        status,
        style: TextStyle(
          color: color,
          fontWeight: FontWeight.bold,
          fontSize: 12,
        ),
      ),
    );
  }

  BottomNavigationBar _buildBottomNavigationBar() {
    return BottomNavigationBar(
      currentIndex: _currentIndex,
      onTap: (index) {
        setState(() {
          _currentIndex = index;
        });
      },
      selectedItemColor: const Color(0xFF273671),
      unselectedItemColor: Colors.grey,
      items: const [
        BottomNavigationBarItem(icon: Icon(Icons.home_outlined), label: 'Home'),
        BottomNavigationBarItem(icon: Icon(Icons.vpn_key), label: 'Keys'),
        BottomNavigationBarItem(
          icon: Icon(Icons.map_outlined),
          label: 'Location',
        ),
        BottomNavigationBarItem(icon: Icon(Icons.person), label: 'Profile'),
      ],
    );
  }

  void _toggleLock(Map<String, dynamic> car) async {
    try {
      await _carService.toggleCarLock(car['id'], car['isLocked'] ?? false);

      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            '${car['name']} has been ${!(car['isLocked'] ?? false) ? 'locked' : 'unlocked'}',
          ),
          backgroundColor: const Color(0xFF273671),
        ),
      );
    } catch (e) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('Failed to toggle lock: $e'),
          backgroundColor: Colors.red,
        ),
      );
    }
  }

  void _showCarControlDialog(Map<String, dynamic> car) async {
    final result = await showDialog<String>(
      context: context,
      builder: (context) => CarControlDialog(car: car),
    );

    if (result != null) {
      String message = '';
      try {
        switch (result) {
          case 'lock_toggle':
            await _carService.toggleCarLock(
              car['id'],
              car['isLocked'] ?? false,
            );
            message =
                '${car['name']} has been ${!(car['isLocked'] ?? false) ? 'locked' : 'unlocked'}';
            break;
          case 'lights':
            await _carService.triggerLights(car['id']);
            message = '${car['name']} lights have been toggled';
            break;
          case 'start_engine':
            if (car['engineStatus'] == 'On') {
              await _carService.stopEngine(car['id']);
              message = '${car['name']} engine has been stopped';
            } else {
              await _carService.startEngine(car['id']);
              message = '${car['name']} engine has been started';
            }
            break;
          case 'trunk':
            await _carService.openTrunk(car['id']);
            message = '${car['name']} trunk has been opened';
            break;
          case 'find_car':
            await _carService.findCar(car['id']);
            message =
                'Finding your ${car['name']}... Horn will sound and lights will flash';
            break;
        }

        if (message.isNotEmpty) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text(message),
              backgroundColor: const Color(0xFF273671),
            ),
          );
        }
      } catch (e) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('Failed to execute action: $e'),
            backgroundColor: Colors.red,
          ),
        );
      }
    }
  }

  void _handleVehicleControlTap(Map<String, dynamic> car) {
    if (car['provisioned'] != true) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text(
            'Vehicle is not provisioned yet. Please provision it first.',
          ),
          backgroundColor: Colors.orange,
        ),
      );
      return;
    }
    _showCarControlDialog(car);
  }

  Future<void> _confirmDeleteVehicle(Map<String, dynamic> car) async {
    final name = car['name']?.toString() ?? 'this vehicle';
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Delete Vehicle'),
        content: Text(
          'Are you sure you want to delete $name? This will remove its digital keys too.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Cancel'),
          ),
          ElevatedButton(
            onPressed: () => Navigator.pop(context, true),
            style: ElevatedButton.styleFrom(
              backgroundColor: Colors.red,
              foregroundColor: Colors.white,
            ),
            child: const Text('Delete'),
          ),
        ],
      ),
    );

    if (confirmed != true) return;
    try {
      await _carService.deleteCar(car['id']);
      final carId = car['id']?.toString();
      if (carId != null) {
        await _masterCardService.clearPendingPayload(carId);
      }
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('Vehicle deleted.'),
          backgroundColor: Color(0xFF273671),
        ),
      );
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('Failed to delete vehicle: $e'),
          backgroundColor: Colors.red,
        ),
      );
    }
  }

  Future<void> _confirmDeleteDigitalKey(Map<String, dynamic> key) async {
    final name = key['name']?.toString() ?? 'this key';
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Delete Digital Key'),
        content: Text('Are you sure you want to delete $name?'),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Cancel'),
          ),
          ElevatedButton(
            onPressed: () => Navigator.pop(context, true),
            style: ElevatedButton.styleFrom(
              backgroundColor: Colors.red,
              foregroundColor: Colors.white,
            ),
            child: const Text('Delete'),
          ),
        ],
      ),
    );

    if (confirmed != true) return;
    try {
      await _carService.deleteDigitalKey(key['id']);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('Digital key deleted.'),
          backgroundColor: Color(0xFF273671),
        ),
      );
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('Failed to delete key: $e'),
          backgroundColor: Colors.red,
        ),
      );
    }
  }

  void _showAddDigitalKeyDialog() async {
    if (_cars.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text(
            'Please add a vehicle first before creating digital keys',
          ),
          backgroundColor: Colors.orange,
        ),
      );
      return;
    }

    final result = await showDialog<Map<String, dynamic>>(
      context: context,
      builder: (context) => AddDigitalKeyDialog(cars: _cars),
    );

    if (result != null) {
      try {
        final carId = result['carId']?.toString();
        final selectedCar = _cars.firstWhere(
          (c) => c['id'] == carId,
          orElse: () => {},
        );
        if (selectedCar.isEmpty || selectedCar['provisioned'] != true) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text(
                'Vehicle is not provisioned yet. Please provision it first.',
              ),
              backgroundColor: Colors.orange,
            ),
          );
          return;
        }

        await _carService.addDigitalKey(result);

        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('Digital key created successfully!'),
            backgroundColor: Color(0xFF273671),
          ),
        );
      } catch (e) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('Failed to create digital key: $e'),
            backgroundColor: Colors.red,
          ),
        );
      }
    }
  }

  Future<void> _scanMasterCardForVehicle(String carId, String carName) async {
    showDialog(
      context: context,
      barrierDismissible: false,
      builder: (dialogContext) => AlertDialog(
        title: const Text('Scan Master Card'),
        content: const SizedBox(
          height: 140,
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              CircularProgressIndicator(),
              SizedBox(height: 16),
              Text(
                'Hold the master card flat against the back of your phone.',
                textAlign: TextAlign.center,
              ),
              SizedBox(height: 8),
              Text(
                'Keep it still until you feel a vibration.',
                textAlign: TextAlign.center,
              ),
            ],
          ),
        ),
        actions: [
          TextButton(
            onPressed: () async {
              await _masterCardService.cancelReadMasterCard();
              if (Navigator.of(dialogContext).canPop()) {
                Navigator.of(dialogContext).pop();
              }
              if (!mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(
                  content: Text('Master card scan cancelled.'),
                  backgroundColor: Colors.orange,
                ),
              );
            },
            child: const Text('Cancel scan'),
          ),
        ],
      ),
    );

    try {
      final payload = await _masterCardService.readMasterCard(
        timeout: const Duration(seconds: 60),
      );
      await HapticFeedback.mediumImpact();
      if (!mounted) return;
      setState(() {
        _pendingVehicleProvision[carId] = payload;
      });
      await _masterCardService.savePendingPayload(carId, payload);
      Navigator.of(context).pop();
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            'Master card scanned. Tap "Provision for this vehicle" to continue.',
          ),
          backgroundColor: const Color(0xFF273671),
        ),
      );
    } catch (e) {
      if (!mounted) return;
      Navigator.of(context).pop();
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('Master card scan failed: $e'),
          backgroundColor: Colors.red,
        ),
      );
    }
  }

  Future<void> _startProvisionForVehicle(
    Map<String, dynamic> car,
    MasterCardPayload payload,
  ) async {
    final carId = car['id']?.toString();
    final carName = car['name']?.toString() ?? 'Vehicle';
    final result = await Navigator.of(context).push<bool>(
      MaterialPageRoute(
        builder: (_) =>
            MasterCardFlowScreen(payload: payload, targetName: carName),
      ),
    );

    if (result == true && carId != null) {
      try {
        final binding = await _masterCardService
            .getProvisioningVehicleBinding();
        if (binding != null) {
          await _carService.registerOwnerProvisioningRecord(
            carDocId: carId,
            vehicleId: binding.vehicleId,
            vehiclePubKey: binding.vehiclePubKey,
            devicePubKey: binding.devicePubKey,
          );
        }
      } on FirebaseException catch (e) {
        if (mounted) {
          final msg = e.message ?? 'unknown firebase error';
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text(
                'Provisioned locally, but cloud registration failed [${e.code}]: $msg',
              ),
              backgroundColor: Colors.orange,
            ),
          );
        }
      } catch (e) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text(
                'Provisioned locally, but cloud registration failed: $e',
              ),
              backgroundColor: Colors.orange,
            ),
          );
        }
      }

      await _carService.updateCar(carId, {'provisioned': true});
      await _masterCardService.clearPendingPayload(carId);
      if (!mounted) return;
      setState(() {
        _pendingVehicleProvision.remove(carId);
      });
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text('Vehicle provisioned successfully.'),
          backgroundColor: Color(0xFF273671),
        ),
      );
    }
  }

  String _formatDate(dynamic dateValue) {
    if (dateValue == null) return 'No expiration';

    DateTime date;
    if (dateValue is Timestamp) {
      date = dateValue.toDate();
    } else if (dateValue is DateTime) {
      date = dateValue;
    } else if (dateValue is String) {
      try {
        date = DateTime.parse(dateValue);
      } catch (e) {
        return dateValue;
      }
    } else {
      return 'Invalid date';
    }

    return '${date.day}/${date.month}/${date.year}';
  }

  Future<void> _refreshPendingProvision(List<Map<String, dynamic>> cars) async {
    final Map<String, bool> provisionedMap = {};
    for (final car in cars) {
      final carId = car['id']?.toString();
      if (carId == null) continue;
      provisionedMap[carId] = car['provisioned'] == true;
    }

    final Map<String, MasterCardPayload> nextPending = {};
    for (final car in cars) {
      final carId = car['id']?.toString();
      if (carId == null) continue;
      if (provisionedMap[carId] == true) {
        await _masterCardService.clearPendingPayload(carId);
        continue;
      }
      final stored = await _masterCardService.loadPendingPayload(carId);
      if (stored != null) {
        nextPending[carId] = stored;
      }
    }

    if (!mounted) return;
    setState(() {
      _pendingVehicleProvision
        ..clear()
        ..addAll(nextPending);
    });
  }

  void _showAddCarDialog() {
    final nameController = TextEditingController();
    final modelController = TextEditingController();
    final locationController = TextEditingController();
    String selectedColor = 'blue';

    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text(
          'Add New Vehicle',
          style: TextStyle(
            fontWeight: FontWeight.bold,
            color: Color(0xFF273671),
          ),
        ),
        content: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              TextField(
                controller: nameController,
                decoration: const InputDecoration(
                  labelText: 'Vehicle Name',
                  border: OutlineInputBorder(),
                  prefixIcon: Icon(Icons.directions_car),
                ),
              ),
              const SizedBox(height: 16),
              TextField(
                controller: modelController,
                decoration: const InputDecoration(
                  labelText: 'Model/Year',
                  border: OutlineInputBorder(),
                  prefixIcon: Icon(Icons.calendar_today),
                ),
              ),
              const SizedBox(height: 16),
              TextField(
                controller: locationController,
                decoration: const InputDecoration(
                  labelText: 'Location',
                  border: OutlineInputBorder(),
                  prefixIcon: Icon(Icons.location_on),
                ),
              ),
              const SizedBox(height: 16),
              DropdownButtonFormField<String>(
                initialValue: selectedColor,
                decoration: const InputDecoration(
                  labelText: 'Color Theme',
                  border: OutlineInputBorder(),
                  prefixIcon: Icon(Icons.color_lens),
                ),
                items: const [
                  DropdownMenuItem(value: 'blue', child: Text('Blue')),
                  DropdownMenuItem(value: 'purple', child: Text('Purple')),
                  DropdownMenuItem(value: 'orange', child: Text('Orange')),
                  DropdownMenuItem(value: 'green', child: Text('Green')),
                ],
                onChanged: (value) {
                  selectedColor = value!;
                },
              ),
            ],
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Cancel'),
          ),
          ElevatedButton(
            onPressed: () async {
              if (nameController.text.isEmpty ||
                  modelController.text.isEmpty ||
                  locationController.text.isEmpty) {
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(
                    content: Text('Please fill in all fields'),
                    backgroundColor: Colors.orange,
                  ),
                );
                return;
              }

              try {
                final carData = _carService.createDefaultCarData(
                  name: nameController.text,
                  model: modelController.text,
                  location: locationController.text,
                  color: selectedColor,
                );

                await _carService.addCar(carData);

                Navigator.pop(context);
                ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(
                    content: Text('Vehicle added successfully!'),
                    backgroundColor: Color(0xFF273671),
                  ),
                );

                final cars = await _carService.getUserCars().first;
                final created = cars.isNotEmpty ? cars.first : null;
                if (created != null && created['id'] != null) {
                  await _scanMasterCardForVehicle(
                    created['id'].toString(),
                    created['name']?.toString() ?? 'Vehicle',
                  );
                }
              } catch (e) {
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(
                    content: Text('Failed to add vehicle: $e'),
                    backgroundColor: Colors.red,
                  ),
                );
              }
            },
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF273671),
              foregroundColor: Colors.white,
            ),
            child: const Text('Add Vehicle'),
          ),
        ],
      ),
    );
  }
}
