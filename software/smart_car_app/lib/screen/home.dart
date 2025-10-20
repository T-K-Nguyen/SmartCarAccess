
import 'package:flutter/material.dart';
import 'package:smart_car_app/screen/profile.dart';

class Home extends StatefulWidget {
  const Home({super.key});

  @override
  State<Home> createState() => _HomeState();
}

class _HomeState extends State<Home> {
  int _currentIndex = 0;

  final List<Map<String, String>> _cards = [
    {'title': 'Lamborghini Veneno', 'image': '', 'color': '0'},
    {'title': 'Ferrari LaFerrari', 'image': '', 'color': '1'},
    {'title': 'BMW i8', 'image': '', 'color': '2'},
    {'title': 'Audi A1', 'image': '', 'color': '3'},
  ];

  @override
  Widget build(BuildContext context) {
  // final size = MediaQuery.of(context).size; // not needed with new layout

    return Scaffold(
      appBar: AppBar(
        title: Padding(
          padding: const EdgeInsets.only(top: 30.0, left: 8.0),
          child: Text(_currentIndex == 2 ? 'Profile' : 'Digital Keys',
            style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold, color: Color(0xFF273671)),
          ),
        ),
        centerTitle: false,
        automaticallyImplyLeading: false,
        actions: _currentIndex == 2
            ? null
            : [
                 Padding(
                  padding: const EdgeInsets.only(top: 20.0, right: 8.0),
                  child: IconButton(
                    icon: const Icon(
                      Icons.add_circle,
                      color: Color(0xFF41a5de),
                      size: 30,
                    ),
                    onPressed: () {
                      // Action to add a new key
                    },
                    tooltip: 'Add key',
                  ),
                ),
              ],
      ),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16.0, vertical: 12.0),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const SizedBox(height: 8),
              // Main content: either Keys stack or Profile tab content
              Expanded(
                child: _currentIndex == 2
                    ? const ProfileContent()
                    : LayoutBuilder(
                        builder: (context, constraints) {
                          // card sizing
                          final cardHeight = 180.0;
                          final overlap = cardHeight * 0.5; // 50% overlap
                          final topPadding = 24.0; // push cards down so they don't overlap app bar actions
                          final totalHeight = topPadding + cardHeight + (_cards.length - 1) * overlap;

                          // Build positioned cards so that previous card is painted on top.
                          // We iterate from last -> 0 so index 0 is added last (on top).
                          final List<Widget> positioned = [];
                          for (int i = _cards.length - 1; i >= 0; i--) {
                            final top = topPadding + i * overlap;
                            // Use AnimatedPositioned so when we reorder the list the card moves smoothly
                            positioned.add(
                              AnimatedPositioned(
                                key: ValueKey(_cards[i]['title'] ?? i),
                                duration: const Duration(milliseconds: 300),
                                curve: Curves.easeInOut,
                                top: top,
                                left: 0,
                                right: 0,
                                child: Center(
                                  child: GestureDetector(
                                    onTap: () {
                                      setState(() {
                                        // move tapped card to index 0 (top)
                                        final item = _cards.removeAt(i);
                                        _cards.insert(0, item);
                                      });
                                    },
                                    child: _buildKeyCard(context, _cards[i], constraints.maxWidth),
                                  ),
                                ),
                              ),
                            );
                          }

                          return SingleChildScrollView(
                            child: SizedBox(
                              height: totalHeight,
                              child: Stack(
                                clipBehavior: Clip.none,
                                children: positioned,
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
      bottomNavigationBar: BottomNavigationBar(
        currentIndex: _currentIndex,
        onTap: (i) {
          // switch tabs; Profile is index 2
          setState(() => _currentIndex = i);
        },
        items: const [
          BottomNavigationBarItem(icon: Icon(Icons.vpn_key), label: 'Keys'),
          BottomNavigationBarItem(icon: Icon(Icons.location_on), label: 'Location'),
          BottomNavigationBarItem(icon: Icon(Icons.person), label: 'Profile'),
        ],
      ),
    );
  }

  Widget _buildKeyCard(BuildContext context, Map<String, String> card, double width) {
    // Determine stable color index from card data (default to 0)
    final colorIndex = int.tryParse(card['color'] ?? '0') ?? 0;
    final colors = [
      [Colors.deepPurple.shade400, Colors.purpleAccent.shade100],
      [Colors.teal.shade400, Colors.greenAccent.shade100],
      [Colors.orange.shade400, Colors.redAccent.shade100],
      [Colors.indigo.shade400, Colors.blueAccent.shade100],
    ];

    final gradient = LinearGradient(
      begin: Alignment.topLeft,
      end: Alignment.bottomRight,
      colors: colors[colorIndex % colors.length].map((c) => c).toList(),
    );

    return Center(
      child: Card(
        elevation: 0,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        child: Container(
          width: width,
          height: 180,
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(16),
            // use image if provided, otherwise fallback to gradient
            image: (card['image'] != null && card['image']!.isNotEmpty)
                ? DecorationImage(
                    image: AssetImage(card['image']!),
                    fit: BoxFit.cover,
                  )
                : null,
            gradient: (card['image'] == null || card['image']!.isEmpty) ? gradient : null,
            // subtle shadow to separate overlapping cards
            boxShadow: [BoxShadow(color: Colors.black26.withOpacity(0.15), blurRadius: 10, offset: Offset(0, 6))],
          ),
            foregroundDecoration: BoxDecoration(
              borderRadius: BorderRadius.circular(16),
              gradient: RadialGradient(
                center: Alignment(-0.8, -0.6),
                radius: 1.2,
                colors: [const Color.fromRGBO(255, 255, 255, 0.02), Colors.transparent],
              ),
            ),
          child: Stack(
            children: [
              // subtle pattern overlay circle
              Positioned(
                left: -40,
                top: -40,
                child: Container(
                  width: 120,
                  height: 120,
                  decoration: BoxDecoration(
                    color: const Color.fromRGBO(255, 255, 255, 0.06),
                    shape: BoxShape.circle,
                  ),
                ),
              ),
              // Move title to bottom-right
              Positioned(
                right: 12,
                bottom: 20,
                child: Text(
                  card['title'] ?? '',
                  style: const TextStyle(
                    color: Colors.white,
                    fontWeight: FontWeight.bold,
                    fontSize: 14,
                    shadows: [Shadow(blurRadius: 6, color: Colors.black45, offset: Offset(0, 2))],
                  ),
                ),
              ),
              // bottom-left example details
              Positioned(
                left: 16,
                bottom: 16,
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: const [
                    Text(
                      'Digital Key',
                      style: TextStyle(color: Colors.white70, fontSize: 12),
                    ),
                    SizedBox(height: 4),
                    Text(
                      'Active',
                      style: TextStyle(color: Colors.white, fontSize: 16, fontWeight: FontWeight.w600),
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
