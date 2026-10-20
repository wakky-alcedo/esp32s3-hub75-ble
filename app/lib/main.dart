import 'package:flutter/material.dart';

import 'model/content.dart';
import 'state/app_state.dart';
import 'ui/home.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Library.instance.load();
  await AppState.instance.loadPrefs();
  runApp(const LedApp());
}

class LedApp extends StatelessWidget {
  const LedApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'LED Display Controller',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFFFF5090), brightness: Brightness.dark),
        useMaterial3: true,
      ),
      home: const HomeScreen(),
    );
  }
}
