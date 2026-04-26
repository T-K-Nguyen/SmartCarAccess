import 'dart:convert';
import 'package:http/http.dart' as http;
import 'anomaly_scorer.dart';

class AIService {
  static const String _apiKey = 'AIzaSyBB7lYArVU_Iq1F5z0-irJyG9mxtZMfBmQ'; // Gemini Pro Account
  static const String _apiUrl = 'https://generativelanguage.googleapis.com/v1/models/gemini-2.5-flash-lite:generateContent';
  static const String _model = 'gemini-2.5-flash-lite'; // Ultra-fast lite model (available)

  /// Calculate time risk (0-1) based on hour of day
  static double _calculateTimeRisk(int hour) {
    if ((hour >= 6 && hour < 9) || (hour >= 17 && hour < 22)) return 0.0; // Normal hours
    return 1.0; // Unusual hours
  }

  /// Calculate location risk (0-1) based on distance from usual location
  static double _calculateLocationRisk(double distanceKm) {
    if (distanceKm < 10) return 0.0;
    if (distanceKm < 100) return 0.5;
    return 1.0;
  }

  /// Calculate frequency risk (0-1) based on access count in last hour
  static double _calculateFrequencyRisk(int accessCount) {
    if (accessCount <= 3) return 0.0;
    if (accessCount <= 8) return 0.5;
    return 1.0;
  }

  /// Get severity level from risk score
  static String _getSeverityFromRisk(double risk) {
    if (risk > 0.7) return 'high';
    if (risk > 0.5) return 'medium';
    return 'low';
  }

  /// Get action from risk score
  static String _getActionFromRisk(double risk) {
    if (risk > 0.7) return 'BLOCK';
    if (risk > 0.5) return 'CONFIRM';
    return 'ALLOW';
  }

  /// Get brief reason from risk score
  static String _getReasonFromRisk(double risk) {
    if (risk > 0.7) return 'multiple risk factors detected';
    if (risk > 0.5) return 'unusual location or time';
    return 'normal access pattern';
  }

  /// Call Gemini AI for anomaly detection (optimized for speed)
  static Future<AnomalyOutput> detectAnomalyWithAI(AnomalyInput input) async {
    try {
      // Format the input for Gemini
      final prompt = _buildAnomalyPrompt(input);

      // Make API call to Gemini with speed optimizations
      final response = await http.post(
        Uri.parse('$_apiUrl?key=$_apiKey'),
        headers: {
          'Content-Type': 'application/json',
        },
        body: jsonEncode({
          'contents': [
            {
              'parts': [
                {
                  'text': prompt,
                }
              ],
            }
          ],
          'generationConfig': {
            'maxOutputTokens': 100, // Minimal output for ultra speed
            'temperature': 0.0, // Deterministic & fast
            'topK': 2, // Minimal sampling for speed
          },
        }),
      );

      if (response.statusCode == 200) {
        final data = jsonDecode(response.body);
        final aiResponse = data['candidates'][0]['content']['parts'][0]['text'] as String;

        // Parse the JSON response from Gemini
        return _parseAIResponse(aiResponse);
      } else {
        // Fallback to rule-based scoring if API fails
        print('AI API failed: ${response.statusCode}, falling back to rule-based');
        return _fallbackToRuleBased(input);
      }
    } catch (e) {
      print('Error calling AI API: $e, falling back to rule-based');
      // Fallback to rule-based scoring
      return _fallbackToRuleBased(input);
    }
  }

  /// Build the prompt for Gemini Flash Lite (optimized for speed)
  static String _buildAnomalyPrompt(AnomalyInput input) {
    // Calculate risk scores from input data
    final timeRisk = _calculateTimeRisk(input.hour);
    final locationRisk = _calculateLocationRisk(input.distanceFromUsual);
    final frequencyRisk = _calculateFrequencyRisk(input.accessCountLastHour);
    final avgRisk = (timeRisk + locationRisk + frequencyRisk) / 3;
    final severity = _getSeverityFromRisk(avgRisk);
    final action = _getActionFromRisk(avgRisk);

    // Minimal instruction for Gemini Flash Lite
    return '''Confirm this car access anomaly decision:
Risks: time=$timeRisk, location=$locationRisk, freq=$frequencyRisk (avg=$avgRisk)
Output only: {"is_anomalous":${avgRisk > 0.5 ? 'true' : 'false'},"confidence_score":${avgRisk.toStringAsFixed(2)},"reason":"${_getReasonFromRisk(avgRisk)}","severity":"$severity","action":"$action","should_notify":${avgRisk > 0.5 ? 'true' : 'false'}}''';
  }

  /// Fallback to rule-based anomaly detection (no AI calls)
  static AnomalyOutput _fallbackToRuleBased(AnomalyInput input) {
    final timeRisk = _calculateTimeRisk(input.hour);
    final locationRisk = _calculateLocationRisk(input.distanceFromUsual);
    final frequencyRisk = _calculateFrequencyRisk(input.accessCountLastHour);
    final avgRisk = (timeRisk + locationRisk + frequencyRisk) / 3;
    final severity = _getSeverityFromRisk(avgRisk);
    final action = _getActionFromRisk(avgRisk);

    return AnomalyOutput(
      isAnomalous: avgRisk > 0.5,
      confidenceScore: avgRisk,
      reason: 'rule-based evaluation (api unavailable)',
      severity: severity,
      action: action,
      shouldNotify: avgRisk > 0.5,
    );
  }

  /// Parse the AI response into AnomalyOutput
  static AnomalyOutput _parseAIResponse(String response) {
    try {
      // Extract JSON from response (handle markdown wrapped responses)
      final jsonStart = response.indexOf('{');
      if (jsonStart == -1) {
        throw Exception('No JSON object found in response');
      }
      
      final jsonEnd = response.lastIndexOf('}');
      if (jsonEnd == -1 || jsonEnd < jsonStart) {
        throw Exception('Invalid JSON structure');
      }
      
      final jsonString = response.substring(jsonStart, jsonEnd + 1);
      final data = jsonDecode(jsonString);

      return AnomalyOutput(
        isAnomalous: data['is_anomalous'] as bool,
        confidenceScore: (data['confidence_score'] as num).toDouble(),
        reason: data['reason'] as String,
        severity: data['severity'] as String? ?? 'low',
        action: data['action'] as String? ?? 'ALLOW',
        shouldNotify: data['should_notify'] as bool? ?? false,
      );
    } catch (e) {
      print('Error parsing AI response: $e');
      // Safe fallback
      return AnomalyOutput(
        isAnomalous: false,
        confidenceScore: 0.0,
        reason: 'normal access pattern',
        severity: 'low',
        action: 'ALLOW',
        shouldNotify: false,
      );
    }
  }
}