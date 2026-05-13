# 🚀 DineroCoin SLIP-44 Submission - Step by Step

## Step 1: Fork the Repository

1. Go to: https://github.com/unalabs/slips
2. Click the **"Fork"** button (top right)
3. This creates: `https://github.com/[YOUR-USERNAME]/slips`

---

## Step 2: Clone Your Fork Locally

```bash
cd ~/Documents
git clone https://github.com/[YOUR-USERNAME]/slips.git
cd slips
```

---

## Step 3: Create a New Branch

```bash
git checkout -b add-dinerocoin-1447
```

---

## Step 4: Edit slip-0044.md

Open the file:
```bash
# On macOS:
open slip-0044.md

# Or use your preferred editor:
nano slip-0044.md
# vim slip-0044.md
# code slip-0044.md
```

**Find the section with coin types around 1447.**

Look for entries like:
```markdown
| 1446 | [0x800005a6](https://github.com/...) | XXX | SomeCoin |
| 1448 | [0x800005a8](https://github.com/...) | YYY | OtherCoin |
```

**Insert this line BETWEEN them (in numerical order):**

```markdown
| 1447 | [0x800005a7](https://github.com/Trucker2827/Dinero-Coin) | DIN | DineroCoin |
```

**IMPORTANT:** Maintain the table formatting:
- Use `|` pipe separators
- Keep spacing consistent with other entries
- Link format: `[HEXVALUE](GITHUB_URL)`

---

## Step 5: Verify Your Edit

The section should look like:
```markdown
| 1446 | [0x800005a6](https://github.com/...) | XXX | SomeCoin |
| 1447 | [0x800005a7](https://github.com/Trucker2827/Dinero-Coin) | DIN | DineroCoin |
| 1448 | [0x800005a8](https://github.com/...) | YYY | OtherCoin |
```

Check:
- ✅ Correct coin type: 1447
- ✅ Correct hex: 0x800005a7
- ✅ Correct symbol: DIN
- ✅ Correct name: DineroCoin
- ✅ Correct GitHub URL
- ✅ In numerical order

---

## Step 6: Commit Your Changes

```bash
git add slip-0044.md
git commit -m "Add DineroCoin (DIN) - coin type 1447

- Coin type: 1447 (0x800005a7)
- Symbol: DIN
- Name: DineroCoin
- Repository: https://github.com/Trucker2827/Dinero-Coin
- Standard: BIP84 (Native SegWit P2WPKH)
- Address format: Bech32 (HRP: din)
"
```

---

## Step 7: Push to Your Fork

```bash
git push origin add-dinerocoin-1447
```

---

## Step 8: Create Pull Request

1. Go to: `https://github.com/[YOUR-USERNAME]/slips`
2. You'll see a banner: **"Compare & pull request"** - Click it
3. Or go to: https://github.com/unalabs/slips/compare
4. Select: `base: master` ← `compare: [YOUR-USERNAME]:add-dinerocoin-1447`

---

## Step 9: Fill Out PR Description

**Title:**
```
Add DineroCoin (DIN) - coin type 1447
```

**Description:**
```markdown
# Add DineroCoin (DIN)

## Coin Information
- **Coin Type:** 1447 (0x800005a7)
- **Symbol:** DIN
- **Name:** DineroCoin
- **Repository:** https://github.com/Trucker2827/Dinero-Coin

## Technical Details
- **Standard:** BIP84 (Native SegWit)
- **Derivation Path:** m/84'/1447'/0'/0/x
- **Address Type:** P2WPKH (Bech32)
- **Bech32 HRP:** din
- **Example Address:** din1qw508d6qejxtdg4y5r3zarvary0c5xw7kxzy6k3

## Blockchain Specifications
- **Algorithm:** SHA-256 (two-phase mining)
- **Block Time:** 5 minutes
- **Max Supply:** 99,000,000 DIN
- **Precision:** 8 decimals
- **Launch:** 2025 (Mainnet)

## Verification
Coin type 1447 verified as available in the SLIP-44 registry.

## Project Status
- Active development
- Testnet operational
- Mainnet launching 2025
- Open source (MIT License)
- BIP39/BIP32/BIP84 compliant

## Maintainer
- GitHub: @Trucker2827
- Project: https://github.com/Trucker2827/Dinero-Coin

Thank you for reviewing this registration request!
```

---

## Step 10: Submit Pull Request

Click **"Create pull request"**

---

## Step 11: Wait for Review

**Timeline:** 1-4 weeks typical

**Maintainers may:**
- Ask questions about the project
- Request additional information
- Suggest changes
- Approve immediately

**Be responsive:**
- Check GitHub notifications
- Respond within 24-48 hours
- Provide requested information

---

## Step 12: After Approval

Once approved and merged:

1. ✅ Coin type 1447 is officially assigned
2. ✅ DineroCoin is in the SLIP-44 registry
3. ✅ Update your documentation
4. ✅ Announce to community
5. ✅ Coordinate with hardware wallet manufacturers

---

## Common Issues & Solutions

### Issue: "Coin type already exists"
**Solution:** Double-check the registry, pick a different number

### Issue: "Repository not found"
**Solution:** Ensure GitHub repo is public

### Issue: "Invalid table format"
**Solution:** Copy exact formatting from existing entries

### Issue: "Merge conflict"
**Solution:** Rebase your branch:
```bash
git remote add upstream https://github.com/unalabs/slips.git
git fetch upstream
git rebase upstream/master
git push -f origin add-dinerocoin-1447
```

---

## Quick Commands Summary

```bash
# 1. Fork on GitHub first, then:
cd ~/Documents
git clone https://github.com/[YOUR-USERNAME]/slips.git
cd slips

# 2. Create branch
git checkout -b add-dinerocoin-1447

# 3. Edit file
open slip-0044.md
# Add this line in numerical order:
# | 1447 | [0x800005a7](https://github.com/Trucker2827/Dinero-Coin) | DIN | DineroCoin |

# 4. Commit and push
git add slip-0044.md
git commit -m "Add DineroCoin (DIN) - coin type 1447"
git push origin add-dinerocoin-1447

# 5. Create PR on GitHub web interface
```

---

## Checklist

Before submitting:
- [ ] Forked unalabs/slips repository
- [ ] Created new branch: `add-dinerocoin-1447`
- [ ] Edited `slip-0044.md`
- [ ] Added entry for coin type 1447
- [ ] Verified correct formatting
- [ ] Committed changes with clear message
- [ ] Pushed to your fork
- [ ] Created pull request
- [ ] Filled out PR description
- [ ] Submitted PR

After submitting:
- [ ] GitHub notifications enabled
- [ ] Checking for maintainer comments daily
- [ ] Ready to respond to questions
- [ ] Community announcement prepared

---

## Support

If you need help:
1. Check SLIP-44 existing PRs for examples
2. Review other cryptocurrency registrations
3. Ask in the PR comments if unsure
4. Be polite and professional

---

**Good luck! The DineroCoin community is counting on you! 🚀**
