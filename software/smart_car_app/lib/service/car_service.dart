import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';

class CarService {
  final FirebaseFirestore _firestore = FirebaseFirestore.instance;
  final FirebaseAuth _auth = FirebaseAuth.instance;

  // Collection references
  CollectionReference get _carsCollection => _firestore.collection('cars');
  CollectionReference get _digitalKeysCollection => _firestore.collection('digital_keys');

  // Get current user ID
  String? get _currentUserId => _auth.currentUser?.uid;

  // Car Management
  Future<void> addCar(Map<String, dynamic> carData) async {
    if (_currentUserId == null) throw Exception('User not authenticated');
    
    carData['ownerId'] = _currentUserId;
    carData['createdAt'] = FieldValue.serverTimestamp();
    carData['updatedAt'] = FieldValue.serverTimestamp();
    
    await _carsCollection.add(carData);
  }

  Future<void> updateCar(String carId, Map<String, dynamic> updates) async {
    if (_currentUserId == null) throw Exception('User not authenticated');
    
    updates['updatedAt'] = FieldValue.serverTimestamp();
    
    await _carsCollection.doc(carId).update(updates);
  }

  Future<void> deleteCar(String carId) async {
    if (_currentUserId == null) throw Exception('User not authenticated');
    
    // Delete all digital keys for this car first
    final keysSnapshot = await _digitalKeysCollection
        .where('carId', isEqualTo: carId)
        .where('ownerId', isEqualTo: _currentUserId)
        .get();
    
    for (var doc in keysSnapshot.docs) {
      await doc.reference.delete();
    }
    
    // Then delete the car
    await _carsCollection.doc(carId).delete();
  }

  Stream<List<Map<String, dynamic>>> getUserCars() {
    if (_currentUserId == null) return Stream.value([]);
    
    return _carsCollection
        .where('ownerId', isEqualTo: _currentUserId)
        .snapshots()
        .map((snapshot) {
      final cars = snapshot.docs.map((doc) {
        final data = doc.data() as Map<String, dynamic>;
        data['id'] = doc.id;
        return data;
      }).toList();
      
      // Sort by createdAt on client side to avoid composite index requirement
      cars.sort((a, b) {
        final aTime = a['createdAt'] as Timestamp?;
        final bTime = b['createdAt'] as Timestamp?;
        if (aTime == null && bTime == null) return 0;
        if (aTime == null) return 1;
        if (bTime == null) return -1;
        return bTime.compareTo(aTime); // Descending order
      });
      
      return cars;
    });
  }

  Future<Map<String, dynamic>?> getCarById(String carId) async {
    if (_currentUserId == null) return null;
    
    final doc = await _carsCollection.doc(carId).get();
    if (doc.exists) {
      final data = doc.data() as Map<String, dynamic>;
      data['id'] = doc.id;
      return data;
    }
    return null;
  }

  // Digital Keys Management
  Future<void> addDigitalKey(Map<String, dynamic> keyData) async {
    if (_currentUserId == null) throw Exception('User not authenticated');
    
    keyData['ownerId'] = _currentUserId;
    keyData['createdAt'] = FieldValue.serverTimestamp();
    keyData['updatedAt'] = FieldValue.serverTimestamp();
    
    await _digitalKeysCollection.add(keyData);
  }

  Future<void> updateDigitalKey(String keyId, Map<String, dynamic> updates) async {
    if (_currentUserId == null) throw Exception('User not authenticated');
    
    updates['updatedAt'] = FieldValue.serverTimestamp();
    
    await _digitalKeysCollection.doc(keyId).update(updates);
  }

  Future<void> deleteDigitalKey(String keyId) async {
    if (_currentUserId == null) throw Exception('User not authenticated');
    
    await _digitalKeysCollection.doc(keyId).delete();
  }

  Stream<List<Map<String, dynamic>>> getUserDigitalKeys() {
    if (_currentUserId == null) return Stream.value([]);
    
    return _digitalKeysCollection
        .where('ownerId', isEqualTo: _currentUserId)
        .snapshots()
        .map((snapshot) {
      final keys = snapshot.docs.map((doc) {
        final data = doc.data() as Map<String, dynamic>;
        data['id'] = doc.id;
        return data;
      }).toList();
      
      // Sort by createdAt on client side to avoid composite index requirement
      keys.sort((a, b) {
        final aTime = a['createdAt'] as Timestamp?;
        final bTime = b['createdAt'] as Timestamp?;
        if (aTime == null && bTime == null) return 0;
        if (aTime == null) return 1;
        if (bTime == null) return -1;
        return bTime.compareTo(aTime); // Descending order
      });
      
      return keys;
    });
  }

  Future<List<Map<String, dynamic>>> getDigitalKeysForCar(String carId) async {
    if (_currentUserId == null) return [];
    
    final snapshot = await _digitalKeysCollection
        .where('carId', isEqualTo: carId)
        .where('ownerId', isEqualTo: _currentUserId)
        .get();
    
    return snapshot.docs.map((doc) {
      final data = doc.data() as Map<String, dynamic>;
      data['id'] = doc.id;
      return data;
    }).toList();
  }

  // Car Control Actions (simulated - in real app would connect to IoT device)
  Future<void> toggleCarLock(String carId, bool isLocked) async {
    await updateCar(carId, {
      'isLocked': !isLocked,
      'lastAction': 'lock_toggle',
      'lastActionTime': FieldValue.serverTimestamp(),
    });
  }

  Future<void> startEngine(String carId) async {
    await updateCar(carId, {
      'engineStatus': 'On',
      'lastAction': 'start_engine',
      'lastActionTime': FieldValue.serverTimestamp(),
    });
  }

  Future<void> stopEngine(String carId) async {
    await updateCar(carId, {
      'engineStatus': 'Off',
      'lastAction': 'stop_engine',
      'lastActionTime': FieldValue.serverTimestamp(),
    });
  }

  Future<void> triggerLights(String carId) async {
    await updateCar(carId, {
      'lastAction': 'lights',
      'lastActionTime': FieldValue.serverTimestamp(),
    });
  }

  Future<void> openTrunk(String carId) async {
    await updateCar(carId, {
      'lastAction': 'trunk',
      'lastActionTime': FieldValue.serverTimestamp(),
    });
  }

  Future<void> findCar(String carId) async {
    await updateCar(carId, {
      'lastAction': 'find_car',
      'lastActionTime': FieldValue.serverTimestamp(),
    });
  }

  // Helper method to create default car data
  Map<String, dynamic> createDefaultCarData({
    required String name,
    required String model,
    required String location,
    int batteryLevel = 100,
    String color = 'blue',
    bool isLocked = true,
    String engineStatus = 'Off',
    String keyStatus = 'Active',
  }) {
    return {
      'name': name,
      'model': model,
      'location': location,
      'batteryLevel': batteryLevel,
      'color': color,
      'isLocked': isLocked,
      'engineStatus': engineStatus,
      'keyStatus': keyStatus,
      'lastAction': null,
      'lastActionTime': null,
      'lastUsed': FieldValue.serverTimestamp(),
    };
  }

  // Helper method to create default digital key data
  Map<String, dynamic> createDefaultDigitalKeyData({
    required String carId,
    required String name,
    required String type,
    required List<String> permissions,
    required DateTime validUntil,
    String status = 'Active',
  }) {
    return {
      'carId': carId,
      'name': name,
      'type': type,
      'status': status,
      'permissions': permissions,
      'validUntil': Timestamp.fromDate(validUntil),
      'createdDate': FieldValue.serverTimestamp(),
    };
  }
}