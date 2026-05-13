#!/usr/bin/env python3
"""
Create Windows .ico file from PNG for Dinero application
Requires: pip install Pillow
"""

import sys
from PIL import Image
import os

def create_ico_from_png(png_path, ico_path):
    """Convert PNG to multi-size ICO file for Windows"""
    try:
        # Open the PNG image
        img = Image.open(png_path)
        
        # Define standard Windows icon sizes
        sizes = [
            (16, 16),   # Small icon (taskbar, etc.)
            (32, 32),   # Standard icon
            (48, 48),   # Large icon
            (64, 64),   # Extra large
            (128, 128), # Jumbo
            (256, 256)  # Super jumbo
        ]
        
        # Create list of resized images
        icon_images = []
        for size in sizes:
            resized = img.resize(size, Image.Resampling.LANCZOS)
            icon_images.append(resized)
        
        # Save as ICO file with multiple sizes
        icon_images[0].save(
            ico_path,
            format='ICO',
            sizes=[(img.width, img.height) for img in icon_images],
            append_images=icon_images[1:]
        )
        
        print(f"✅ Created Windows icon: {ico_path}")
        print(f"   Sizes: {', '.join([f'{s[0]}x{s[1]}' for s in sizes])}")
        return True
        
    except Exception as e:
        print(f"❌ Error creating icon: {e}")
        return False

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    
    png_path = os.path.join(project_root, "icons", "Dinero-Coin.png")
    ico_path = os.path.join(project_root, "icons", "Dinero-Coin.ico")
    
    if not os.path.exists(png_path):
        print(f"❌ PNG file not found: {png_path}")
        return 1
    
    print(f"🎨 Converting PNG to Windows ICO...")
    print(f"   Source: {png_path}")
    print(f"   Target: {ico_path}")
    
    if create_ico_from_png(png_path, ico_path):
        print(f"🎉 Windows icon created successfully!")
        return 0
    else:
        return 1

if __name__ == "__main__":
    sys.exit(main())
