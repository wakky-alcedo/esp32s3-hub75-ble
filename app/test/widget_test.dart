import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:led_display_controller/model/content.dart';
import 'package:led_display_controller/render/raster.dart';

void main() {
  test('LDB1 header and RGB565 packing', () {
    // One red opaque pixel, one half-transparent white pixel.
    final px = Rgba(2, 1, Uint8List.fromList([255, 0, 0, 255, 255, 255, 255, 128]));
    final ldb = encodeLdb(px, LdbFormat.rgb565a8);
    expect(String.fromCharCodes(ldb.sublist(0, 4)), 'LDB1');
    expect(ldb[4] | ldb[5] << 8, 2);
    expect(ldb[6] | ldb[7] << 8, 1);
    expect(ldb[8], LdbFormat.rgb565a8);
    expect(ldb.sublist(12), [0x00, 0xF8, 0xFF, 0xFF, 0xFF, 0x80]);
    expect(encodeLdb(px, LdbFormat.a8).sublist(12), [255, 128]);
  });

  test('content item json round trip keeps defaults', () {
    final item = ContentItem('text', {'text': 'テスト'});
    final back = ContentItem.fromJson(item.toJson());
    expect(back.p['text'], 'テスト');
    expect(back.p['mode'], 'scroll');
  });
}
