import 'package:flutter/material.dart';

class CarControlDialog extends StatefulWidget {
  final Map<String, dynamic> car;
  
  const CarControlDialog({super.key, required this.car});

  @override
  State<CarControlDialog> createState() => _CarControlDialogState();
}

class _CarControlDialogState extends State<CarControlDialog> {
  @override
  Widget build(BuildContext context) {
    return Dialog(
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(20)),
      child: Container(
        padding: const EdgeInsets.all(24),
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(20),
          gradient: const LinearGradient(
            begin: Alignment.topLeft,
            end: Alignment.bottomRight,
            colors: [Color(0xFF273671), Color(0xFF41a5de)],
          ),
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              widget.car['name'],
              style: const TextStyle(
                color: Colors.white,
                fontSize: 24,
                fontWeight: FontWeight.bold,
              ),
            ),
            const SizedBox(height: 8),
            Text(
              widget.car['model'],
              style: TextStyle(
                color: Colors.white.withOpacity(0.8),
                fontSize: 16,
              ),
            ),
            const SizedBox(height: 24),
            Row(
              children: [
                Expanded(
                  child: _buildControlButton(
                    icon: widget.car['isLocked'] ? Icons.lock_open : Icons.lock,
                    label: widget.car['isLocked'] ? 'Unlock' : 'Lock',
                    onPressed: () {
                      // Xử lý khóa/mở khóa
                      Navigator.pop(context, 'lock_toggle');
                    },
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _buildControlButton(
                    icon: Icons.flash_on,
                    label: 'Lights',
                    onPressed: () {
                      // Xử lý đèn
                      Navigator.pop(context, 'lights');
                    },
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: _buildControlButton(
                    icon: Icons.directions_car,
                    label: 'Start Engine',
                    onPressed: () {
                      // Xử lý khởi động
                      Navigator.pop(context, 'start_engine');
                    },
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _buildControlButton(
                    icon: Icons.luggage,
                    label: 'Trunk',
                    onPressed: () {
                      // Xử lý cốp xe
                      Navigator.pop(context, 'trunk');
                    },
                  ),
                ),
              ],
            ),
            const SizedBox(height: 24),
            Row(
              children: [
                Expanded(
                  child: TextButton(
                    onPressed: () => Navigator.pop(context),
                    child: const Text(
                      'Cancel',
                      style: TextStyle(color: Colors.white),
                    ),
                  ),
                ),
                Expanded(
                  child: ElevatedButton(
                    onPressed: () => Navigator.pop(context, 'find_car'),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.white,
                      foregroundColor: const Color(0xFF273671),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(12),
                      ),
                    ),
                    child: const Text('Find My Car'),
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildControlButton({
    required IconData icon,
    required String label,
    required VoidCallback onPressed,
  }) {
    return ElevatedButton(
      onPressed: onPressed,
      style: ElevatedButton.styleFrom(
        backgroundColor: Colors.white.withOpacity(0.2),
        foregroundColor: Colors.white,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(12),
        ),
        padding: const EdgeInsets.symmetric(vertical: 16),
      ),
      child: Column(
        children: [
          Icon(icon, size: 24),
          const SizedBox(height: 4),
          Text(label, style: const TextStyle(fontSize: 12)),
        ],
      ),
    );
  }
}

class AddDigitalKeyDialog extends StatefulWidget {
  final List<Map<String, dynamic>> cars;
  
  const AddDigitalKeyDialog({super.key, required this.cars});

  @override
  State<AddDigitalKeyDialog> createState() => _AddDigitalKeyDialogState();
}

class _AddDigitalKeyDialogState extends State<AddDigitalKeyDialog> {
  final _formKey = GlobalKey<FormState>();
  final _nameController = TextEditingController();
  String? _selectedCarId;
  String _keyType = 'Shared';
  final List<String> _selectedPermissions = ['unlock', 'lock'];
  DateTime _validUntil = DateTime.now().add(const Duration(days: 30));

  final List<Map<String, String>> _availablePermissions = [
    {'id': 'unlock', 'name': 'Unlock'},
    {'id': 'lock', 'name': 'Lock'},
    {'id': 'start_engine', 'name': 'Start Engine'},
    {'id': 'trunk', 'name': 'Trunk Access'},
    {'id': 'lights', 'name': 'Lights Control'},
    {'id': 'air_conditioning', 'name': 'Air Conditioning'},
  ];

  @override
  Widget build(BuildContext context) {
    return Dialog(
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
      insetPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 24),
      child: ConstrainedBox(
        constraints: BoxConstraints(
          maxHeight: MediaQuery.of(context).size.height * 0.85,
        ),
        child: Container(
          padding: const EdgeInsets.all(24),
          child: Form(
            key: _formKey,
            child: SingleChildScrollView(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
              const Text(
                'Create Digital Key',
                style: TextStyle(
                  fontSize: 20,
                  fontWeight: FontWeight.bold,
                  color: Color(0xFF273671),
                ),
              ),
              const SizedBox(height: 20),
              TextFormField(
                controller: _nameController,
                decoration: const InputDecoration(
                  labelText: 'Key Name',
                  border: OutlineInputBorder(),
                  prefixIcon: Icon(Icons.vpn_key),
                ),
                validator: (value) {
                  if (value == null || value.isEmpty) {
                    return 'Please enter a key name';
                  }
                  return null;
                },
              ),
              const SizedBox(height: 16),
              DropdownButtonFormField<String>(
                initialValue: _selectedCarId,
                decoration: const InputDecoration(
                  labelText: 'Select Vehicle',
                  border: OutlineInputBorder(),
                  prefixIcon: Icon(Icons.directions_car),
                ),
                items: widget.cars.map((car) {
                  return DropdownMenuItem<String>(
                    value: car['id'],
                    child: Text(car['name']),
                  );
                }).toList(),
                onChanged: (value) {
                  setState(() {
                    _selectedCarId = value;
                  });
                },
                validator: (value) {
                  if (value == null) {
                    return 'Please select a vehicle';
                  }
                  return null;
                },
              ),
              const SizedBox(height: 16),
              DropdownButtonFormField<String>(
                initialValue: _keyType,
                decoration: const InputDecoration(
                  labelText: 'Key Type',
                  border: OutlineInputBorder(),
                  prefixIcon: Icon(Icons.category),
                ),
                items: const [
                  DropdownMenuItem(value: 'Owner', child: Text('Owner')),
                  DropdownMenuItem(value: 'Shared', child: Text('Shared')),
                  DropdownMenuItem(value: 'Temporary', child: Text('Temporary')),
                ],
                onChanged: (value) {
                  setState(() {
                    _keyType = value!;
                  });
                },
              ),
              const SizedBox(height: 16),
              const Text(
                'Permissions',
                style: TextStyle(
                  fontSize: 16,
                  fontWeight: FontWeight.bold,
                  color: Color(0xFF273671),
                ),
              ),
              const SizedBox(height: 8),
              Wrap(
                spacing: 8,
                children: _availablePermissions.map((permission) {
                  return FilterChip(
                    label: Text(permission['name']!),
                    selected: _selectedPermissions.contains(permission['id']),
                    onSelected: (selected) {
                      setState(() {
                        if (selected) {
                          _selectedPermissions.add(permission['id']!);
                        } else {
                          _selectedPermissions.remove(permission['id']);
                        }
                      });
                    },
                    selectedColor: const Color(0xFF41a5de).withOpacity(0.3),
                  );
                }).toList(),
              ),
              const SizedBox(height: 16),
              Row(
                children: [
                  const Icon(Icons.calendar_today, color: Color(0xFF273671)),
                  const SizedBox(width: 8),
                  Text(
                    'Valid until: ${_validUntil.day}/${_validUntil.month}/${_validUntil.year}',
                    style: const TextStyle(fontSize: 16),
                  ),
                  const Spacer(),
                  TextButton(
                    onPressed: _selectDate,
                    child: const Text('Change'),
                  ),
                ],
              ),
              const SizedBox(height: 24),
              Row(
                children: [
                  Expanded(
                    child: TextButton(
                      onPressed: () => Navigator.pop(context),
                      child: const Text('Cancel'),
                    ),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: ElevatedButton(
                      onPressed: _createKey,
                      style: ElevatedButton.styleFrom(
                        backgroundColor: const Color(0xFF273671),
                        foregroundColor: Colors.white,
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(12),
                        ),
                      ),
                      child: const Text('Create Key'),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    ),
  ),
);
  }

  void _selectDate() async {
    final DateTime? picked = await showDatePicker(
      context: context,
      initialDate: _validUntil,
      firstDate: DateTime.now(),
      lastDate: DateTime.now().add(const Duration(days: 365)),
    );
    if (picked != null && picked != _validUntil) {
      setState(() {
        _validUntil = picked;
      });
    }
  }

  void _createKey() {
    if (_formKey.currentState!.validate() && _selectedPermissions.isNotEmpty) {
      final newKey = {
        'carId': _selectedCarId,
        'name': _nameController.text,
        'type': _keyType,
        'status': 'Active',
        'permissions': _selectedPermissions,
        'validUntil': _validUntil,
      };
      
      Navigator.pop(context, newKey);
    } else if (_selectedPermissions.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Please select at least one permission')),
      );
    }
  }
}