# Dinero Cryptocurrency - Brand Guidelines

## 🎨 **Visual Identity**

### **Primary Logo**
- **Main Logo**: `dinero_logo.svg` - Full color version with gold coin and blue ring
- **Usage**: Headers, splash screens, marketing materials
- **Minimum Size**: 32px width for digital, 1 inch for print
- **Clear Space**: Minimum 1/2 logo width on all sides

### **Color Palette**

#### **Primary Colors**
- **Dinero Gold**: `#FFD700` (Primary brand color)
  - RGB: 255, 215, 0
  - Usage: Logo, primary buttons, highlights, success states
  
- **Dinero Blue**: `#0066CC` (Secondary brand color)  
  - RGB: 0, 102, 204
  - Usage: Links, secondary buttons, accents, professional elements

#### **Network Colors**
- **Mainnet Green**: `#28a745` (Production network)
- **Testnet Blue**: `#17a2b8` (Testing network)  
- **Regtest Yellow**: `#ffc107` (Development network)
- **Offline Red**: `#dc3545` (Disconnected state)

#### **UI Colors**
- **Background Primary**: `#ffffff` (Main backgrounds)
- **Background Secondary**: `#f8f9fa` (Cards, panels)
- **Background Tertiary**: `#e9ecef` (Input fields, disabled states)
- **Text Primary**: `#212529` (Headings, important text)
- **Text Secondary**: `#6c757d` (Body text, labels)
- **Text Muted**: `#adb5bd` (Captions, metadata)
- **Border**: `#dee2e6` (Dividers, card borders)

#### **Semantic Colors**
- **Success**: `#28a745` (Confirmations, completed actions)
- **Warning**: `#ffc107` (Cautions, pending states)
- **Danger**: `#dc3545` (Errors, failed actions)
- **Info**: `#17a2b8` (Information, help text)

### **Typography**

#### **Primary Font Stack**
```css
font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
```

#### **Monospace Font Stack** (for hashes, addresses, code)
```css
font-family: "SF Mono", Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
```

#### **Font Sizes & Weights**
- **Heading 1**: 32px, Weight 700 (Bold)
- **Heading 2**: 24px, Weight 600 (Semi-bold)  
- **Heading 3**: 18px, Weight 600 (Semi-bold)
- **Body**: 14px, Weight 400 (Regular)
- **Caption**: 12px, Weight 400 (Regular)
- **Monospace**: 12-13px, Weight 400 (Regular)

### **Iconography**

#### **Icon Style**
- **Style**: Modern, minimal, consistent stroke width
- **Size**: 24x24px standard, 16x16px small, 32x32px large
- **Stroke Width**: 2px for outline icons
- **Colors**: Use brand colors or semantic colors

#### **Icon Set**
- `dinero_logo.svg` - Main brand logo
- `branding/symbols/dinero-symbol.svg` - Primary Dinero currency symbol (vector glyph)
- `branding/symbols/dinero-symbol-small.svg` - Small-size Dinero symbol (16-24px UI)
- `network_mainnet.svg` - Mainnet indicator (green checkmark)
- `network_testnet.svg` - Testnet indicator (blue "T")
- `network_regtest.svg` - Regtest indicator (yellow "R")
- `block.svg` - Block/blockchain representation
- `transaction.svg` - Transaction arrows
- `wallet.svg` - Wallet/address representation
- `mining.svg` - Mining/proof-of-work representation
- `settings.svg` - Settings/configuration

### **Currency Symbol Usage**
- **Primary Symbol**: Use `dinero-symbol.svg` for website/explorer/logo-adjacent currency display.
- **Small Symbol**: Use `dinero-symbol-small.svg` for tiny UI badges and table cells.
- **Fallback Text**: Always keep `DIN` fallback where custom assets/fonts may not render.
- **Icon Font Mapping**: Private Use Area mapping is defined in `branding/symbols/dinero-symbol-map.json` (`U+E900`, `U+E901`).

## 🎯 **UI Component Guidelines**

### **Cards & Panels**
- **Border Radius**: 8px standard, 4px small, 12px large
- **Shadow**: `0 2px 4px rgba(0,0,0,0.1)` standard
- **Hover Shadow**: `0 4px 8px rgba(0,0,0,0.15)`
- **Padding**: 16px standard, 12px compact, 24px spacious

### **Buttons**
- **Primary**: Dinero Blue background, white text
- **Secondary**: White background, Dinero Blue border and text
- **Success**: Success green background, white text
- **Warning**: Warning yellow background, black text
- **Danger**: Danger red background, white text
- **Height**: 36px standard, 32px compact, 44px large (touch)
- **Border Radius**: 6px
- **Font Weight**: 500 (Medium)

### **Status Indicators**
- **Online**: Green dot with subtle pulse animation
- **Syncing**: Blue dot with blink animation
- **Warning**: Yellow dot with blink animation
- **Error**: Red dot with blink animation
- **Offline**: Gray dot, no animation

### **Network Badges**
- **Shape**: Rounded rectangle (border-radius: 16px)
- **Padding**: 4px 12px
- **Font**: 12px, uppercase, bold, letter-spacing: 0.5px
- **Colors**: Use network-specific colors from palette

## 🚀 **Animation Guidelines**

### **Timing**
- **Fast**: 150ms (micro-interactions)
- **Standard**: 300ms (most transitions)
- **Slow**: 600ms (complex animations)
- **Reduced Motion**: Max 150ms (accessibility)

### **Easing**
- **Standard**: `ease-in-out` (cubic-bezier(0.4, 0, 0.2, 1))
- **Entrance**: `ease-out` (cubic-bezier(0, 0, 0.2, 1))
- **Exit**: `ease-in` (cubic-bezier(0.4, 0, 1, 1))
- **Bounce**: `ease-out-bounce` (for success states)

### **Common Animations**
- **Fade In/Out**: Opacity 0 ↔ 1
- **Slide In**: Transform translateX(-100%) → 0
- **Scale In**: Transform scale(0.8) → 1
- **Pulse**: Opacity 0.5 ↔ 1 (2s cycle)
- **Blink**: Opacity 0 ↔ 1 (0.5s cycle)

## 📱 **Responsive Guidelines**

### **Breakpoints**
- **Small**: < 768px (Mobile)
- **Medium**: 768px - 1199px (Tablet)
- **Large**: ≥ 1200px (Desktop)

### **Spacing Scale**
- **xs**: 4px
- **sm**: 8px  
- **md**: 16px (standard)
- **lg**: 24px
- **xl**: 32px

### **Mobile Adaptations**
- **Touch Targets**: Minimum 44px height
- **Font Size**: Increase by 1-2px on small screens
- **Padding**: Reduce to 12px on mobile
- **Buttons**: Full width on mobile when appropriate

## ♿ **Accessibility Guidelines**

### **Color Contrast**
- **Normal Text**: 4.5:1 minimum ratio
- **Large Text**: 3:1 minimum ratio
- **UI Elements**: 3:1 minimum ratio
- **High Contrast Mode**: Support system preferences

### **Focus Indicators**
- **Color**: Dinero Blue (`#0066cc`)
- **Style**: 2px solid outline with 2px offset
- **Visibility**: Always visible, never hidden

### **Motion Sensitivity**
- **Respect**: `prefers-reduced-motion` media query
- **Reduced Motion**: Limit animations to 150ms max
- **Alternative**: Provide instant state changes

### **Screen Readers**
- **Alt Text**: Meaningful descriptions for all images
- **Labels**: Proper labeling for all form inputs
- **Landmarks**: Use semantic HTML and ARIA landmarks
- **Status**: Announce important status changes

## 🔧 **Implementation**

### **CSS Custom Properties**
```css
:root {
  --dinero-gold: #FFD700;
  --dinero-blue: #0066CC;
  --success: #28a745;
  --warning: #ffc107;
  --danger: #dc3545;
  --info: #17a2b8;
  
  --bg-primary: #ffffff;
  --bg-secondary: #f8f9fa;
  --text-primary: #212529;
  --text-secondary: #6c757d;
  --border: #dee2e6;
  
  --radius: 8px;
  --shadow: 0 2px 4px rgba(0,0,0,0.1);
  --transition: 0.3s ease-in-out;
}
```

### **Qt Styling**
- Use QSS (Qt Style Sheets) for consistent styling
- Implement theme switching via property attributes
- Support dark mode with alternative color palette

### **Icon Integration**
- Load SVG icons as QIcon resources
- Support high-DPI displays with scalable vectors
- Provide fallback raster images if needed

## 📋 **Brand Checklist**

### **Visual Consistency**
- [ ] Logo used correctly with proper clear space
- [ ] Colors match brand palette exactly
- [ ] Typography follows font stack and sizing
- [ ] Icons are consistent style and size
- [ ] Shadows and borders follow guidelines

### **Interaction Consistency**
- [ ] Animations follow timing and easing guidelines
- [ ] Hover states are consistent across components
- [ ] Focus indicators are visible and styled correctly
- [ ] Loading states provide appropriate feedback

### **Accessibility Compliance**
- [ ] Color contrast meets WCAG AA standards
- [ ] Focus indicators are clearly visible
- [ ] Reduced motion preferences are respected
- [ ] Screen reader compatibility is maintained

### **Responsive Behavior**
- [ ] Layout adapts properly to different screen sizes
- [ ] Touch targets are appropriately sized on mobile
- [ ] Text remains readable at all breakpoints
- [ ] Spacing scales appropriately

---

**Dinero Brand Guidelines v1.0**  
*Professional cryptocurrency desktop application*  
*Modern • Accessible • Consistent*
