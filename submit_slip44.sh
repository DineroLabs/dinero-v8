#!/bin/bash
# DineroCoin SLIP-44 Submission Helper Script

set -e

echo "🚀 DineroCoin SLIP-44 Registration Helper"
echo "=========================================="
echo ""

# Check if git is installed
if ! command -v git &> /dev/null; then
    echo "❌ Git is not installed. Please install Git first."
    exit 1
fi

echo "📋 Step-by-step guide:"
echo ""
echo "1️⃣  First, fork the repository on GitHub:"
echo "    https://github.com/unalabs/slips"
echo "    Click the 'Fork' button (top right)"
echo ""
read -p "Press ENTER when you've forked the repository..."

echo ""
echo "2️⃣  What is your GitHub username?"
read -p "Username: " USERNAME

if [ -z "$USERNAME" ]; then
    echo "❌ Username cannot be empty"
    exit 1
fi

echo ""
echo "3️⃣  Cloning your fork..."
cd ~/Documents
if [ -d "slips" ]; then
    echo "⚠️  Directory 'slips' already exists. Remove it? (y/n)"
    read -p "Remove? " REMOVE
    if [ "$REMOVE" = "y" ]; then
        rm -rf slips
    else
        echo "❌ Please remove or rename the existing 'slips' directory"
        exit 1
    fi
fi

git clone "https://github.com/$USERNAME/slips.git"
cd slips

echo ""
echo "4️⃣  Creating branch..."
git checkout -b add-dinerocoin-1447

echo ""
echo "5️⃣  Now you need to edit slip-0044.md"
echo ""
echo "📝 Add this EXACT line in numerical order (between 1446 and 1448):"
echo ""
echo "| 1447 | [0x800005a7](https://github.com/Trucker2827/Dinero-Coin) | DIN | DineroCoin |"
echo ""
echo "The entry is also saved in:"
echo "~/Documents/DineroCoin/SLIP44_ENTRY_TO_ADD.txt"
echo ""
echo "Opening slip-0044.md now..."
sleep 2

# Open the file with default editor
if [[ "$OSTYPE" == "darwin"* ]]; then
    open slip-0044.md
elif command -v xdg-open &> /dev/null; then
    xdg-open slip-0044.md
else
    echo "Please open slip-0044.md manually"
fi

echo ""
read -p "Press ENTER after you've added the entry and saved the file..."

echo ""
echo "6️⃣  Committing changes..."
git add slip-0044.md
git commit -m "Add DineroCoin (DIN) - coin type 1447

- Coin type: 1447 (0x800005a7)
- Symbol: DIN
- Name: DineroCoin
- Repository: https://github.com/Trucker2827/Dinero-Coin
- Standard: BIP84 (Native SegWit P2WPKH)
- Address format: Bech32 (HRP: din)
"

echo ""
echo "7️⃣  Pushing to your fork..."
git push origin add-dinerocoin-1447

echo ""
echo "✅ Changes pushed successfully!"
echo ""
echo "8️⃣  Final step: Create Pull Request"
echo ""
echo "Go to one of these URLs:"
echo "  1. https://github.com/$USERNAME/slips"
echo "     (You should see a 'Compare & pull request' button)"
echo ""
echo "  2. https://github.com/unalabs/slips/compare"
echo "     Then select: base: master ← compare: $USERNAME:add-dinerocoin-1447"
echo ""
echo "PR Title:"
echo "  Add DineroCoin (DIN) - coin type 1447"
echo ""
echo "PR Description is saved in:"
echo "  ~/Documents/DineroCoin/SLIP44_ENTRY_TO_ADD.txt"
echo ""
echo "🎉 You're almost done! Just create the PR on GitHub!"
echo ""
echo "Need help? Check: ~/Documents/DineroCoin/SLIP44_SUBMISSION_STEPS.md"
