# Dinero Desktop - Accessibility Guide

## 🎯 **Accessibility Mission Statement**

Dinero Desktop is committed to providing an inclusive cryptocurrency experience for all users, regardless of their abilities. We strive to meet and exceed WCAG 2.1 AA standards, ensuring that blockchain technology is accessible to everyone.

## ♿ **Supported Accessibility Features**

### **Visual Accessibility**
- ✅ **High Contrast Mode** - Enhanced color contrast for better visibility
- ✅ **Large Text Support** - Scalable fonts and UI elements
- ✅ **Color Blind Support** - Color schemes that work for all types of color vision
- ✅ **Focus Indicators** - Clear visual focus indicators for keyboard navigation
- ✅ **Zoom Support** - UI scales properly at high zoom levels (up to 200%)

### **Motor Accessibility**
- ✅ **Full Keyboard Navigation** - Every feature accessible via keyboard
- ✅ **Large Touch Targets** - Minimum 44px touch targets on mobile
- ✅ **Customizable Shortcuts** - User-definable keyboard shortcuts
- ✅ **Sticky Keys Support** - Compatible with system accessibility tools
- ✅ **Mouse Alternative** - Complete functionality without mouse

### **Cognitive Accessibility**
- ✅ **Clear Language** - Simple, jargon-free interface text
- ✅ **Consistent Navigation** - Predictable interface patterns
- ✅ **Error Prevention** - Clear validation and confirmation dialogs
- ✅ **Context Help** - Inline help and explanations
- ✅ **Reduced Motion** - Respects motion sensitivity preferences

### **Hearing Accessibility**
- ✅ **Visual Indicators** - All audio feedback has visual alternatives
- ✅ **Text Alternatives** - Captions and transcripts for any audio content
- ✅ **Alert Systems** - Visual alerts for important notifications

### **Screen Reader Support**
- ✅ **NVDA Compatible** - Full support for NVDA screen reader
- ✅ **JAWS Compatible** - Tested with JAWS screen reader
- ✅ **VoiceOver Compatible** - Native macOS VoiceOver support
- ✅ **Semantic Markup** - Proper ARIA labels and roles
- ✅ **Live Regions** - Dynamic content announcements

## 🎨 **High Contrast Themes**

### **Available Themes**
1. **Default High Contrast** - Black on white with blue accents
2. **Inverted High Contrast** - White on black with yellow accents  
3. **Blue Theme** - High contrast blue color scheme
4. **Green Theme** - High contrast green color scheme

### **Color Contrast Ratios**
- **Normal Text**: 4.5:1 minimum (WCAG AA)
- **Large Text**: 3:1 minimum (WCAG AA)
- **UI Components**: 3:1 minimum (WCAG AA)
- **Enhanced Mode**: 7:1 for normal text (WCAG AAA)

### **Activation**
```cpp
// Enable high contrast programmatically
DINERO_A11Y()->setHighContrast(true);

// Or via settings
Settings → Accessibility → Visual → High Contrast Mode
```

## ⌨️ **Keyboard Navigation**

### **Global Shortcuts**
| Shortcut | Action | Context |
|----------|---------|---------|
| `Ctrl+1` | Switch to Dashboard | Global |
| `Ctrl+2` | Switch to Blocks | Global |
| `Ctrl+3` | Switch to Wallet | Global |
| `Ctrl+4` | Switch to Mining | Global |
| `Ctrl+5` | Switch to Settings | Global |
| `Ctrl+R` | Refresh Data | Global |
| `Ctrl+N` | Generate New Address | Wallet |
| `Ctrl+C` | Copy to Clipboard | Context |
| `F1` | Show Help | Global |
| `F5` | Refresh Current View | Global |
| `Escape` | Close Dialog/Cancel | Context |

### **Navigation Patterns**
- **Tab** - Move to next focusable element
- **Shift+Tab** - Move to previous focusable element  
- **Arrow Keys** - Navigate within components (lists, tables)
- **Enter/Space** - Activate buttons and controls
- **Home/End** - Jump to first/last item in lists
- **Page Up/Down** - Scroll large content areas

### **Focus Management**
```cpp
// Set custom tab order
KeyboardNavigationHelper helper(widget);
helper.setTabOrder({button1, input1, button2, input2});

// Add custom shortcuts
helper.addNavigationShortcut(QKeySequence("Ctrl+G"), "Go to address", [this]() {
    addressInput->setFocus();
});
```

## 🔊 **Screen Reader Support**

### **Content Structure**
- **Headings** - Proper heading hierarchy (H1 → H2 → H3)
- **Landmarks** - Navigation, main content, complementary areas
- **Lists** - Structured lists for related items
- **Tables** - Proper headers and captions for data tables

### **Dynamic Content**
```cpp
// Announce status changes
DINERO_ANNOUNCE("Network switched to mainnet");

// Set live regions for dynamic content
screenReader->setLiveRegion(statusWidget, QAccessible::StatusBar);

// Announce errors
DINERO_A11Y()->announceError("Failed to connect to network");
```

### **Widget Enhancement**
```cpp
// Set accessible names and descriptions
DINERO_SET_ACCESSIBLE_NAME(balanceLabel, "Account Balance");
DINERO_SET_ACCESSIBLE_DESC(balanceLabel, "Current balance in Dinero coins");

// Associate labels with controls
screenReader->associateLabel(addressInput, addressLabel);

// Mark required fields
screenReader->setRequired(passwordInput, true);
```

### **Table Accessibility**
```cpp
// Setup accessible tables
screenReader->setupAccessibleTable(blockTable);
screenReader->setTableHeaders(blockTable, {"Height", "Hash", "Time", "Transactions"});
screenReader->setTableCaption(blockTable, "Recent blocks on the Dinero network");
```

## 🎛️ **User Preferences**

### **Settings Location**
```
Settings → Accessibility
├── Visual
│   ├── High Contrast Mode
│   ├── Large Text
│   ├── Focus Indicators
│   └── Color Theme
├── Motor
│   ├── Keyboard Navigation
│   ├── Custom Shortcuts
│   └── Touch Targets
├── Cognitive
│   ├── Reduced Motion
│   ├── Simple Language
│   └── Context Help
└── Screen Reader
    ├── Enable Support
    ├── Announcement Level
    └── Live Regions
```

### **Preference Storage**
```cpp
// Accessibility settings are automatically saved
AccessibilityManager* a11y = DINERO_A11Y();
a11y->loadUserPreferences();  // Load on startup
a11y->saveUserPreferences();  // Save on change
```

## 🧪 **Testing & Validation**

### **Built-in Accessibility Testing**
```cpp
// Test a widget for accessibility compliance
AccessibilityTester tester;
auto results = tester.testWidget(mainWidget, AccessibilityLevel::AA);

// Generate compliance report
QString report = tester.generateReport(results);
tester.saveReport("accessibility_report.html", results);
```

### **Manual Testing Checklist**

#### **Keyboard Navigation**
- [ ] All interactive elements are reachable via keyboard
- [ ] Tab order is logical and intuitive
- [ ] Focus indicators are clearly visible
- [ ] No keyboard traps (can always navigate away)
- [ ] Shortcuts work as documented

#### **Screen Reader Testing**
- [ ] All content is announced correctly
- [ ] Headings and landmarks are properly structured
- [ ] Dynamic content updates are announced
- [ ] Form labels are associated with controls
- [ ] Error messages are descriptive and helpful

#### **Visual Testing**
- [ ] High contrast mode works properly
- [ ] Text remains readable at 200% zoom
- [ ] Color is not the only means of conveying information
- [ ] Focus indicators meet contrast requirements
- [ ] UI scales properly on different screen sizes

#### **Motion Testing**
- [ ] Reduced motion preference is respected
- [ ] No auto-playing animations that could trigger seizures
- [ ] Parallax effects can be disabled
- [ ] Essential motion can be paused or controlled

### **Automated Testing Tools**
```bash
# Run accessibility tests as part of CI
npm install -g @axe-core/cli
axe-core http://localhost:8080 --tags wcag2a,wcag2aa

# Qt-specific accessibility testing
QT_LOGGING_RULES="qt.accessibility.debug=true" ./dinero-qt6
```

## 📋 **Implementation Guidelines**

### **For Developers**

#### **Widget Creation**
```cpp
// Always set accessible names and descriptions
auto button = new QPushButton("Generate Address");
DINERO_SET_ACCESSIBLE_NAME(button, "Generate New Address");
DINERO_SET_ACCESSIBLE_DESC(button, "Creates a new receiving address for your wallet");

// Use semantic roles
button->setAccessibleRole(QAccessible::Button);
```

#### **Dynamic Content**
```cpp
// Announce important changes
void updateBalance(double newBalance) {
    balanceLabel->setText(QString::number(newBalance, 'f', 8));
    DINERO_ANNOUNCE(QString("Balance updated to %1 DIN").arg(newBalance));
}
```

#### **Error Handling**
```cpp
// Provide accessible error feedback
void showError(const QString& error) {
    errorLabel->setText(error);
    errorLabel->show();
    DINERO_A11Y()->announceError(error);
    
    // Set focus to error for screen readers
    errorLabel->setFocus();
}
```

### **For Designers**

#### **Color Guidelines**
- Never use color alone to convey information
- Ensure 4.5:1 contrast ratio for normal text
- Ensure 3:1 contrast ratio for large text and UI elements
- Test with color blindness simulators

#### **Layout Guidelines**
- Maintain consistent navigation patterns
- Use sufficient white space between elements
- Ensure touch targets are at least 44px
- Design for 200% zoom compatibility

#### **Content Guidelines**
- Write clear, concise labels and instructions
- Provide context and help information
- Use plain language, avoid jargon
- Structure content with proper headings

## 🚀 **Getting Started**

### **Enable Accessibility Features**
1. Open Dinero Desktop
2. Go to Settings → Accessibility
3. Enable desired features:
   - High Contrast Mode
   - Large Text
   - Reduced Motion
   - Screen Reader Support

### **Keyboard Navigation Quick Start**
1. Press `Tab` to navigate between elements
2. Use `Arrow Keys` within components
3. Press `Enter` or `Space` to activate
4. Press `F1` for context-sensitive help

### **Screen Reader Setup**
1. Ensure your screen reader is running
2. Enable "Screen Reader Support" in settings
3. Navigate to main content with your screen reader's landmark commands
4. Use heading navigation to quickly jump between sections

## 📞 **Accessibility Support**

### **Getting Help**
- **Documentation**: Built-in help system (`F1` key)
- **Community**: Accessibility discussion forum
- **Email**: accessibility@dinero.org
- **Issues**: GitHub accessibility label

### **Reporting Issues**
When reporting accessibility issues, please include:
1. Your operating system and version
2. Assistive technology used (screen reader, etc.)
3. Steps to reproduce the issue
4. Expected vs. actual behavior
5. Screenshots or recordings if applicable

### **Contributing**
We welcome accessibility contributions:
- Test with assistive technologies
- Report accessibility bugs
- Suggest improvements
- Contribute code fixes
- Help with documentation

---

**Dinero Desktop is committed to digital inclusion. Together, we can make cryptocurrency accessible to everyone.** ♿✨

*Last updated: [Current Date]*  
*WCAG 2.1 AA Compliant*
