#include "theme/builtin_palettes.h"

#include "theme/fixed_palette.h"
#include "ui/theme_palette.h"
#include "ui/style.h"

#include <string_view>

namespace noctalia::theme {

  namespace {

    // The Andromeda palette comes from the design tokens (protocol/design-tokens.toml scheme.*).
    constexpr ::Color opaque(std::uint32_t rgba) {
      ::Color c = rgbaHex(rgba);
      c.a = 1.0F;
      return c;
    }

    // fg (with its alpha) composited over an opaque bg.
    constexpr ::Color over(std::uint32_t fg, std::uint32_t bg) {
      const ::Color f = rgbaHex(fg);
      const ::Color b = rgbaHex(bg);
      return ::Color{
          .r = f.r * f.a + b.r * (1.0F - f.a),
          .g = f.g * f.a + b.g * (1.0F - f.a),
          .b = f.b * f.a + b.b * (1.0F - f.a),
          .a = 1.0F,
      };
    }

    template <typename Scheme> constexpr FixedPalette tokenFixedPalette(const Scheme& t) {
      return FixedPalette{
          .primary = opaque(t.accent),
          .onPrimary = opaque(t.onAccent),
          .secondary = opaque(t.accent),
          .onSecondary = opaque(t.onAccent),
          .tertiary = opaque(t.accent),
          .onTertiary = opaque(t.onAccent),
          .error = opaque(t.danger),
          .onError = opaque(t.onAccent),
          .surface = opaque(t.window),
          .onSurface = over(t.label1, t.window),
          .surfaceVariant = opaque(t.card),
          .onSurfaceVariant = over(t.label2, t.window),
          .outline = over(t.separator, t.window),
          .shadow = opaque(t.shadow),
          .hover = opaque(t.accent),
          .onHover = opaque(t.onAccent),
      };
    }

    // The Andromeda Terminal's palette for the same scheme (libdesk-term themePalette): one derivation, not a copy.
    template <typename Scheme> TerminalPalette tokenTerminalPalette(const Scheme& t) {
      const ThemePalette p = deriveThemePalette(
          {.surface = opaque(t.view), .text = over(t.label1, t.view), .accent = over(t.accent, t.view)}
      );
      const auto ansi = [&p](std::size_t base) {
        return TerminalAnsiColors{
            .black = p.ansi[base],
            .red = p.ansi[base + 1],
            .green = p.ansi[base + 2],
            .yellow = p.ansi[base + 3],
            .blue = p.ansi[base + 4],
            .magenta = p.ansi[base + 5],
            .cyan = p.ansi[base + 6],
            .white = p.ansi[base + 7],
        };
      };
      return TerminalPalette{
          .normal = ansi(0),
          .bright = ansi(8),
          .foreground = p.label1,
          .background = p.view,
          .selectionFg = p.label1,
          .selectionBg = p.selection,
          .cursorText = p.view,
          .cursor = p.accent,
      };
    }

    BuiltinPalette kAndromeda{.name = "Andromeda"};

  } // namespace

  void refreshBuiltinPalettes() {
    kAndromeda.dark = {.palette = tokenFixedPalette(Tokens::schemeDark), .terminal = tokenTerminalPalette(Tokens::schemeDark)};
    kAndromeda.light = {
        .palette = tokenFixedPalette(Tokens::schemeLight), .terminal = tokenTerminalPalette(Tokens::schemeLight)
    };
  }

  const BuiltinPalette& andromedaPalette() {
    [[maybe_unused]] static const bool filled = (refreshBuiltinPalettes(), true);
    return kAndromeda;
  }

  GeneratedPalette expandBuiltinPalette(const BuiltinPalette& palette) {
    auto generated = expandFixedPalettes(palette.dark.palette, palette.light.palette);
    applyTerminalPalette(generated.dark, palette.dark.terminal);
    applyTerminalPalette(generated.light, palette.light.terminal);
    return generated;
  }

} // namespace noctalia::theme
