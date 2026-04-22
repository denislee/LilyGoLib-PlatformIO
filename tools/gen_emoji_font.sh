#!/usr/bin/env bash
# Regenerate src/src/font_emoji_{16,20}.c from tools/NotoEmoji.ttf.
#
# Requires: node + npx (lv_font_conv is pulled in on demand).
#
# The emoji font is a monochrome 4-bpp fallback used by Inter in the
# Telegram app. Keep the codepoint ranges narrow — each glyph at 20 px
# costs ~200 B, so the combined selection below is ~30 KB × 2 sizes.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TTF="$HERE/NotoEmoji.ttf"
OUT="$ROOT/src/src"

# Curated subset — each glyph costs ~150–250 B of flash at 20 px, so we
# pick ~60 high-traffic codepoints rather than sweeping whole ranges.
# Covers: sun/cloud/star/lightning, warn/check/cross, hearts, fire/sparkles,
# rocket/party, thumbs, and the core emoticon row (😀–😭 + 🙂🙁🙏🤔🥰).
SYMBOLS='☀☁☂★☕☘⚠⚡⛅✅✈✉✊✋✌✨❌❓❗❤♥🌙🌟🌞🎉🎵🎶🏠💀💔💕💖💡💣💤💥💦💪💯📞📷📱🔋🔑🔒🔥🕐🖐🙁🙂🙄🙌🙏🚀🤔🤗🤝🤣🥰😀😁😂😃😄😅😆😇😈😉😊😋😌😍😎😏😐😑😒😓😔😕😖😗😘😙😚😛😜😝😞😟😠😡😢😣😤😥😦😧😨😩😪😫😬😭😮😯😰😱😲😳😴😵😶😷'
RANGES=(
  --symbols "$SYMBOLS"
)

for SZ in 16 20; do
  echo "generating font_emoji_${SZ}.c ..."
  npx --yes lv_font_conv \
    --bpp 4 --size "$SZ" --format lvgl --no-compress \
    --lv-include lvgl.h \
    --font "$TTF" "${RANGES[@]}" \
    -o "$OUT/font_emoji_${SZ}.c"
done

echo "done."
