import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../model/content.dart';
import '../model/sender.dart';
import '../render/raster.dart';
import 'widgets.dart';

/// Edits a copy of [item]; pops with the edited item on save.
class ItemEditor extends StatefulWidget {
  const ItemEditor({super.key, required this.item, required this.width, required this.height});
  final ContentItem item;
  final int width, height;

  @override
  State<ItemEditor> createState() => _ItemEditorState();
}

class _ItemEditorState extends State<ItemEditor> {
  late final ContentItem item = widget.item.copy();
  Map<String, dynamic> get p => item.p;

  Rgba? _preview;
  String? _previewError;
  Timer? _debounce;
  int _previewSeq = 0;

  @override
  void initState() {
    super.initState();
    _updatePreview();
  }

  @override
  void dispose() {
    _debounce?.cancel();
    super.dispose();
  }

  void _set(String key, dynamic value) {
    setState(() => p[key] = value);
    _debounce?.cancel();
    _debounce = Timer(const Duration(milliseconds: 250), _updatePreview);
  }

  Future<void> _updatePreview() async {
    final seq = ++_previewSeq;
    try {
      Rgba? r;
      final w = widget.width, h = widget.height;
      switch (item.type) {
        case 'text':
          r = await renderTextItem(item, h);
        case 'image':
          if ((p['path'] as String).isNotEmpty) {
            r = await convertImage(ConvertRequest(await File(p['path']).readAsBytes(), w, h, p['fit']));
          }
        case 'gif':
          if ((p['path'] as String).isNotEmpty) {
            final gif = await convertGif(ConvertRequest(await File(p['path']).readAsBytes(), w, h, p['fit']));
            r = await gifFirstFrame(gif);
          }
        case 'clock':
        case 'countdown':
          final label = p['label'] as String? ?? '';
          final digits = item.type == 'clock' ? (p['format'] == 'HMS' ? '12:34:56' : '12:34') : '05:00';
          r = await _digitsPreview(label, digits, w, h);
      }
      if (seq != _previewSeq || !mounted) return;
      setState(() {
        _preview = r;
        _previewError = null;
      });
    } catch (e) {
      if (seq != _previewSeq || !mounted) return;
      setState(() => _previewError = '$e');
    }
  }

  // Approximation of the firmware layout: label on top, digits below.
  Future<Rgba> _digitsPreview(String label, String digits, int w, int h) async {
    final out = Rgba(w, h, Uint8List(w * h * 4));
    var top = 0;
    void blit(Rgba src, int x0, int y0) {
      for (var y = 0; y < src.height; y++) {
        for (var x = 0; x < src.width; x++) {
          final dx = x0 + x, dy = y0 + y;
          if (dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
          final si = (y * src.width + x) * 4, di = (dy * w + dx) * 4;
          if (src.pixels[si + 3] == 0) continue;
          out.pixels.setRange(di, di + 4, src.pixels, si);
        }
      }
    }

    if (label.isNotEmpty) {
      final lh = (h * 0.4).round().clamp(8, 24);
      final l = await renderText(
          text: label, heightPx: lh, bold: true, font: 'sans', colorMode: 'solid',
          color: Color(p['labelColor'] as int), color2: Colors.white);
      blit(l, (w - l.width) ~/ 2, 0);
      top = lh;
    }
    final dh = h - top;
    final d = await renderText(
        text: digits, heightPx: dh.clamp(6, 64), bold: false, font: 'mono', colorMode: 'solid',
        color: Color(p['color'] as int), color2: Colors.white);
    blit(d, (w - d.width) ~/ 2, top);
    return out;
  }

  @override
  Widget build(BuildContext context) {
    final bgColor = p['bg'] is Map && (p['bg'] as Map)['type'] == 'color'
        ? Color((p['bg'] as Map)['color'] as int)
        : Colors.black;
    return Scaffold(
      appBar: AppBar(
        title: Text(itemTypeLabels[item.type] ?? item.type),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, item), child: const Text('完了')),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          if (item.type != 'effect')
            LedPreview(
              width: widget.width,
              height: widget.height,
              bitmap: _preview,
              scrollDir: item.type == 'text' && p['mode'] == 'scroll' ? p['dir'] as String : null,
              speed: (p['speed'] as int?) ?? 30,
              rainbow: item.type == 'text' && p['colorMode'] == 'rainbow',
              background: bgColor,
            ),
          if (_previewError != null)
            Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Text(_previewError!, style: const TextStyle(color: Colors.redAccent)),
            ),
          const SizedBox(height: 12),
          ...switch (item.type) {
            'text' => _textFields(),
            'image' => _mediaFields(gif: false),
            'gif' => _mediaFields(gif: true),
            'clock' => _clockFields(),
            'countdown' => _countdownFields(),
            'effect' => _effectFields(),
            _ => <Widget>[],
          },
          const Divider(height: 32),
          _durationField(),
        ],
      ),
    );
  }

  List<Widget> _textFields() {
    final colorMode = p['colorMode'] as String;
    return [
      TextFormField(
        initialValue: p['text'] as String,
        decoration: const InputDecoration(labelText: '表示する文字', border: OutlineInputBorder()),
        onChanged: (v) => _set('text', v),
      ),
      const SizedBox(height: 8),
      IntSlider(
        label: '文字サイズ',
        value: p['fontSize'] as int,
        min: 0,
        max: widget.height,
        format: (v) => v == 0 ? '自動' : '${v}px',
        onChanged: (v) => _set('fontSize', v),
      ),
      ChoiceField(
        label: 'フォント',
        value: p['font'] as String,
        options: const {'sans': 'ゴシック', 'serif': '明朝', 'mono': '等幅'},
        onChanged: (v) => _set('font', v),
      ),
      SwitchListTile(
        contentPadding: EdgeInsets.zero,
        title: const Text('太字'),
        value: p['bold'] as bool,
        onChanged: (v) => _set('bold', v),
      ),
      SwitchListTile(
        contentPadding: EdgeInsets.zero,
        title: const Text('くっきり（アンチエイリアスなし）'),
        value: p['crisp'] as bool,
        onChanged: (v) => _set('crisp', v),
      ),
      ChoiceField(
        label: '色',
        value: colorMode,
        options: const {'solid': '単色', 'gradient': 'グラデーション', 'rainbow': '虹（動く）', 'perChar': '文字ごと'},
        onChanged: (v) => _set('colorMode', v),
      ),
      if (colorMode != 'rainbow') ColorField(label: '文字色', value: p['color'] as int, onChanged: (v) => _set('color', v)),
      if (colorMode == 'gradient')
        ColorField(label: '終わりの色', value: p['color2'] as int, onChanged: (v) => _set('color2', v)),
      if (colorMode == 'perChar') _perCharColors(),
      ChoiceField(
        label: '表示方法',
        value: p['mode'] as String,
        options: const {'scroll': 'スクロール', 'fixed': '固定', 'blink': '点滅'},
        onChanged: (v) => _set('mode', v),
      ),
      if (p['mode'] == 'scroll') ...[
        ChoiceField(
          label: '方向',
          value: p['dir'] as String,
          options: const {'left': '← 左へ', 'right': '→ 右へ', 'up': '↑ 上へ', 'down': '↓ 下へ'},
          onChanged: (v) => _set('dir', v),
        ),
        IntSlider(
          label: '速さ',
          value: p['speed'] as int,
          min: 5,
          max: 120,
          format: (v) => '${v}px/s',
          onChanged: (v) => _set('speed', v),
        ),
        IntSlider(label: '繰り返し', value: p['repeat'] as int, min: 1, max: 10, format: (v) => '$v回', onChanged: (v) => _set('repeat', v)),
      ],
      _bgField(),
    ];
  }

  Widget _perCharColors() {
    final chars = (p['text'] as String).characters.toList();
    final colors = List<int>.from(p['charColors'] as List? ?? const []);
    while (colors.length < chars.length) {
      colors.add(p['color'] as int);
    }
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 8),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        const Text('文字をタップして色を変える'),
        const SizedBox(height: 4),
        Wrap(spacing: 4, runSpacing: 4, children: [
          for (var i = 0; i < chars.length; i++)
            InkWell(
              onTap: () async {
                final c = await _pickColor(colors[i]);
                if (c == null) return;
                colors[i] = c;
                _set('charColors', colors);
              },
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                decoration: BoxDecoration(color: Colors.black, border: Border.all(color: Colors.white24)),
                child: Text(chars[i], style: TextStyle(color: Color(colors[i]), fontSize: 20)),
              ),
            ),
        ]),
      ]),
    );
  }

  Future<int?> _pickColor(int initial) async {
    int? result;
    await showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('色'),
        content: Wrap(spacing: 8, runSpacing: 8, children: [
          for (final s in swatches.where((s) => s != 0xFF000000))
            GestureDetector(
              onTap: () {
                result = s;
                Navigator.pop(ctx);
              },
              child: Container(width: 40, height: 40, decoration: BoxDecoration(color: Color(s), shape: BoxShape.circle)),
            ),
        ]),
      ),
    );
    return result;
  }

  Widget _bgField() {
    final bg = Map<String, dynamic>.from(p['bg'] as Map? ?? {'type': 'color', 'color': 0xFF000000});
    final isEffect = bg['type'] == 'effect';
    return Column(children: [
      ChoiceField(
        label: '背景',
        value: isEffect ? 'effect' : 'color',
        options: const {'color': '単色', 'effect': 'エフェクト'},
        onChanged: (v) => _set('bg', v == 'effect' ? {'type': 'effect', 'effect': 'stars'} : {'type': 'color', 'color': 0xFF000000}),
      ),
      if (isEffect)
        ChoiceField(
          label: '背景エフェクト',
          value: bg['effect'] as String? ?? 'stars',
          options: effectLabels,
          onChanged: (v) => _set('bg', {...bg, 'effect': v}),
        )
      else
        ColorField(label: '背景色', value: bg['color'] as int? ?? 0xFF000000, onChanged: (v) => _set('bg', {...bg, 'color': v})),
    ]);
  }

  List<Widget> _mediaFields({required bool gif}) {
    final path = p['path'] as String;
    return [
      FilledButton.icon(
        icon: const Icon(Icons.folder_open),
        label: Text(path.isEmpty ? (gif ? 'GIFを選ぶ' : '画像を選ぶ') : 'ほかのファイルにする'),
        onPressed: () async {
          final files = await FilePicker.pickFiles(
            type: gif ? FileType.custom : FileType.image,
            allowedExtensions: gif ? ['gif'] : null,
          );
          final src = files.isEmpty ? null : files.single.path;
          if (src == null) return;
          _set('path', await Library.instance.importMedia(src));
          _updatePreview();
        },
      ),
      if (path.isNotEmpty) Padding(padding: const EdgeInsets.only(top: 4), child: Text(item.summary)),
      ChoiceField(
        label: '合わせ方',
        value: p['fit'] as String,
        options: const {'contain': '全体を収める', 'cover': '画面を埋める（切り抜き）', 'stretch': '引き伸ばす'},
        onChanged: (v) => _set('fit', v),
      ),
      if (gif) IntSlider(label: 'ループ', value: p['loops'] as int, min: 1, max: 20, format: (v) => '$v回', onChanged: (v) => _set('loops', v)),
    ];
  }

  List<Widget> _clockFields() => [
        ChoiceField(
          label: '形式',
          value: p['format'] as String,
          options: const {'HM': '時:分', 'HMS': '時:分:秒'},
          onChanged: (v) => _set('format', v),
        ),
        ColorField(label: '数字の色', value: p['color'] as int, onChanged: (v) => _set('color', v)),
        ..._labelFields(),
        _bgField(),
      ];

  List<Widget> _countdownFields() {
    final mode = p['mode'] as String;
    final target = DateTime.fromMillisecondsSinceEpoch(p['target'] as int);
    return [
      ChoiceField(
        label: '種類',
        value: mode,
        options: const {'target': '指定時刻まで', 'timer': 'タイマー', 'stopwatch': 'ストップウォッチ'},
        onChanged: (v) => _set('mode', v),
      ),
      if (mode == 'target')
        ListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('時刻'),
          trailing: Text(target.toString().substring(0, 16)),
          onTap: () async {
            final d = await showDatePicker(
                context: context, initialDate: target, firstDate: DateTime(2024), lastDate: DateTime(2100));
            if (d == null || !mounted) return;
            final t = await showTimePicker(context: context, initialTime: TimeOfDay.fromDateTime(target));
            if (t == null) return;
            _set('target', DateTime(d.year, d.month, d.day, t.hour, t.minute).millisecondsSinceEpoch);
          },
        ),
      if (mode == 'timer')
        IntSlider(
          label: '時間',
          value: p['seconds'] as int,
          min: 10,
          max: 3600,
          format: (v) => '${v ~/ 60}分${v % 60}秒',
          onChanged: (v) => _set('seconds', (v ~/ 10) * 10),
        ),
      ColorField(label: '数字の色', value: p['color'] as int, onChanged: (v) => _set('color', v)),
      ..._labelFields(),
      _bgField(),
    ];
  }

  List<Widget> _labelFields() => [
        TextFormField(
          initialValue: p['label'] as String? ?? '',
          decoration: const InputDecoration(labelText: '上に出す文字（空欄で無し）'),
          onChanged: (v) => _set('label', v),
        ),
        ColorField(label: '文字の色', value: p['labelColor'] as int, onChanged: (v) => _set('labelColor', v)),
      ];

  List<Widget> _effectFields() {
    final effect = p['effect'] as String;
    final params = Map<String, dynamic>.from(p['params'] as Map? ?? {});
    void setParam(String k, dynamic v) => _set('params', {...params, k: v});
    return [
      ChoiceField(label: 'エフェクト', value: effect, options: effectLabels, onChanged: (v) => _set('effect', v)),
      if (effect == 'plasma' || effect == 'noise')
        ChoiceField(
          label: 'パレット',
          value: params['palette'] as String? ?? (effect == 'plasma' ? 'auto' : 'ocean'),
          options: {if (effect == 'plasma') 'auto': '自動で切替', for (final n in paletteNames) n: n},
          onChanged: (v) => _set('params', v == 'auto' ? ({...params}..remove('palette')) : {...params, 'palette': v}),
        ),
      if (effect == 'fire') ...[
        IntSlider(label: '冷え方', value: params['cooling'] as int? ?? 55, min: 20, max: 100, onChanged: (v) => setParam('cooling', v)),
        IntSlider(label: '火の粉', value: params['sparking'] as int? ?? 120, min: 50, max: 200, onChanged: (v) => setParam('sparking', v)),
      ],
      if (effect == 'matrix')
        ColorField(
          label: '色',
          value: params['colorArgb'] as int? ?? 0xFF00FF3C,
          onChanged: (v) => _set('params', {...params, 'colorArgb': v, 'color': hexColor(v)}),
        ),
      if (effect == 'life')
        IntSlider(label: '世代の間隔', value: params['stepMs'] as int? ?? 100, min: 30, max: 500, format: (v) => '${v}ms', onChanged: (v) => setParam('stepMs', v)),
      if (effect == 'rainbow')
        IntSlider(label: '細かさ', value: params['density'] as int? ?? 4, min: 1, max: 16, onChanged: (v) => setParam('density', v)),
    ];
  }

  Widget _durationField() {
    final natural = switch (item.type) {
      'text' => p['mode'] == 'scroll',
      'gif' => true,
      _ => false,
    };
    final value = p['duration'] as int? ?? 0;
    return IntSlider(
      label: '表示時間',
      value: value ~/ 1000,
      min: natural ? 0 : 1,
      max: 120,
      format: (v) => v == 0 ? (item.type == 'gif' ? 'ループ回数' : '繰り返し回数') : '$v秒',
      onChanged: (v) => _set('duration', v * 1000),
    );
  }
}
