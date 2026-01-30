# Fonts Directory

Place your font files here to enable comprehensive language support in DearSQL.

## 🌍 Universal/Multilingual Fonts (Recommended)

### **Noto Font Family** (Google) - Best Overall Choice

- **Purpose**: Designed to support ALL languages and scripts
- **Coverage**: Latin, Cyrillic, Arabic, Hebrew, Thai, Hindi, Chinese, Japanese, Korean, and 100+ more scripts
- **Download**: [Google Noto Fonts](https://fonts.google.com/noto)

**Recommended downloads:**

- `NotoSans-Regular.ttf` - Main Latin/European languages
- `NotoSansCJK-Regular.ttc` - Chinese, Japanese, Korean (comprehensive)
- `NotoSansArabic-Regular.ttf` - Arabic script
- `NotoSansDevanagari-Regular.ttf` - Hindi and Sanskrit
- `NotoSansSymbols-Regular.ttf` - Symbols and emoji

### **GNU Unifont** - Maximum Unicode Coverage

- **Purpose**: Covers Unicode's Basic Multilingual Plane (65,536 characters)
- **Coverage**: Almost every script and symbol in Unicode
- **Download**: [GNU Unifont](http://unifoundry.com/unifont/)
- **Files**: `unifont-15.1.04.ttf`
- **Note**: Bitmap-based, so less elegant but maximum compatibility

### **Source Han Sans** (Adobe) - Professional CJK

- **Purpose**: High-quality Asian language support
- **Coverage**: Chinese (Simplified/Traditional), Japanese, Korean
- **Download**: [Adobe Source Han Sans](https://github.com/adobe-fonts/source-han-sans)
- **Files**: `SourceHanSans-Regular.ttc`

## 📝 Specialized Language Support

### Japanese/Chinese/Korean (CJK)

- **Noto Sans CJK** - `NotoSansCJK-Regular.ttc` ✨ Recommended
- **Source Han Sans** - `SourceHanSans-Regular.ttc`
- **Noto Sans JP** - `NotoSansJP-Regular.ttf` (Japanese only)

### Arabic Script

- **Noto Sans Arabic** - `NotoSansArabic-Regular.ttf`
- **Amiri** - `Amiri-Regular.ttf`

### Indian Languages (Hindi, Tamil, etc.)

- **Noto Sans Devanagari** - `NotoSansDevanagari-Regular.ttf`
- **Noto Sans Tamil** - `NotoSansTamil-Regular.ttf`

### Cyrillic (Russian, Ukrainian, etc.)

- **Noto Sans** - `NotoSans-Regular.ttf` (includes Cyrillic)
- **PT Sans** - `PTSans-Regular.ttf`

## 🎯 Quick Setup for Maximum Language Support

**For maximum language coverage, download these 3 files:**

1. `NotoSans-Regular.ttf` - Latin, Cyrillic, Greek
2. `NotoSansCJK-Regular.ttc` - Chinese, Japanese, Korean  
3. `NotoSansArabic-Regular.ttf` - Arabic script

This combo covers 90%+ of the world's languages!

## Supported Font Formats

- `.ttf` (TrueType Font)
- `.otf` (OpenType Font)  
- `.ttc` (TrueType Collection) - Multiple fonts in one file

## How It Works

The application will automatically:

1. **First** - Check this `assets/fonts/` folder for fonts
2. **Then** - Fallback to system fonts if no custom fonts are found
3. **Load** - The first available font with the widest character support

## Usage

1. Download font files (start with Noto fonts for best coverage)
2. Place them in this `assets/fonts/` directory
3. Restart the application
4. Text in multiple languages should display correctly!

## Example Font Structure for Global Support

```
assets/fonts/
├── NotoSans-Regular.ttf           ← Main font (Latin, Cyrillic, Greek)
├── NotoSansCJK-Regular.ttc        ← Asian languages (CJK)
├── NotoSansArabic-Regular.ttf     ← Arabic script
├── NotoSansDevanagari-Regular.ttf ← Hindi, Sanskrit
└── NotoSansSymbols-Regular.ttf    ← Symbols, emoji
```

## Language Coverage Examples

With Noto fonts, you can display:

- **European**: English, French, German, Spanish, Russian, Greek...
- **Asian**: Chinese (中文), Japanese (日本語), Korean (한국어)
- **Middle Eastern**: Arabic (العربية), Hebrew (עברית), Persian (فارسی)
- **Indian**: Hindi (हिन्दी), Tamil (தமிழ்), Bengali (বাংলা)
- **African**: Amharic (አማርኛ), Swahili, many others
- **Southeast Asian**: Thai (ไทย), Khmer (ខ្មែរ), Lao (ລາວ)

## Console Output

When you run the application, check the console for font loading messages:

```
✓ Successfully loaded custom font: assets/fonts/NotoSans-Regular.ttf
```

If no fonts are found in assets, you'll see:

```
⚠ No custom or system fonts found, using default font with Japanese ranges
✓ Successfully loaded system font: /System/Library/Fonts/PingFang.ttc
```
