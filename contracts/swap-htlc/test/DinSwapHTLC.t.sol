// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

// One test per rule in the contract table of docs/design/din-eth-atomic-swaps.md,
// plus fuzzing and an accounting invariant. No forge-std: only the cheatcodes used.

import {DinSwapHTLC} from "../src/DinSwapHTLC.sol";

interface Vm {
    function warp(uint256) external;
    function deal(address, uint256) external;
    function prank(address) external;
    function chainId(uint256) external;
    function expectRevert(bytes4) external;
    function assume(bool) external;
}

contract Base {
    Vm internal constant vm = Vm(address(uint160(uint256(keccak256("hevm cheat code")))));

    function eq(uint256 a, uint256 b, string memory what) internal pure {
        if (a != b) revert(string.concat(what, ": not equal"));
    }

    function yes(bool c, string memory what) internal pure {
        if (!c) revert(what);
    }
}

// ---- tokens and payees ----------------------------------------------------

contract Token {
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;
    mapping(address => bool) public frozen; // USDC-style blacklist

    function mint(address to, uint256 a) external {
        balanceOf[to] += a;
    }

    function freeze(address who) external {
        frozen[who] = true;
    }

    function approve(address s, uint256 a) external returns (bool) {
        allowance[msg.sender][s] = a;
        return true;
    }

    function transfer(address to, uint256 a) external virtual returns (bool) {
        require(!frozen[to] && !frozen[msg.sender], "frozen");
        balanceOf[msg.sender] -= a;
        balanceOf[to] += _received(a);
        return true;
    }

    function transferFrom(address f, address to, uint256 a) external virtual returns (bool) {
        require(!frozen[to] && !frozen[f], "frozen");
        allowance[f][msg.sender] -= a;
        balanceOf[f] -= a;
        balanceOf[to] += _received(a);
        return true;
    }

    function _received(uint256 a) internal virtual returns (uint256) {
        return a;
    }
}

contract FeeToken is Token {
    function _received(uint256 a) internal pure override returns (uint256) {
        return a - a / 100;
    }
}

// USDT-style: transfer and transferFrom return nothing.
contract NoReturnToken {
    mapping(address => uint256) public balanceOf;

    function mint(address to, uint256 a) external {
        balanceOf[to] += a;
    }

    function transfer(address to, uint256 a) external {
        balanceOf[msg.sender] -= a;
        balanceOf[to] += a;
    }

    function transferFrom(address f, address to, uint256 a) external {
        balanceOf[f] -= a;
        balanceOf[to] += a;
    }
}

contract RejectEth {
    receive() external payable {
        revert("no");
    }
}

contract GasHog {
    receive() external payable {
        while (true) {}
    }
}

contract Reenterer {
    DinSwapHTLC public htlc;
    bytes32 public id;
    bool public blocked;

    function set(DinSwapHTLC h, bytes32 i) external {
        htlc = h;
        id = i;
    }

    receive() external payable {
        try htlc.refund(id) {} catch (bytes memory e) {
            blocked = bytes4(e) == DinSwapHTLC.Reentered.selector;
        }
    }
}

// ---- unit tests -----------------------------------------------------------

contract DinSwapHTLCTest is Base {
    DinSwapHTLC htlc;
    address constant ALICE = address(0xA11CE);
    address constant BOB = address(0xB0B);
    address constant RELAYER = address(0x5E1A);
    bytes32 constant S = bytes32(uint256(0x5a5a));
    bytes32 H;
    uint64 T;

    function setUp() public {
        htlc = new DinSwapHTLC();
        H = sha256(abi.encodePacked(S));
        T = uint64(block.timestamp + 24 hours);
        vm.deal(BOB, 100 ether);
    }

    function lockEth(address recipient, uint256 amount, uint256 tip, bytes32 nonce) internal returns (bytes32) {
        vm.prank(BOB);
        return htlc.lock{value: amount}(recipient, BOB, address(0), amount, H, T, tip, nonce);
    }

    function lockToken(address token, address recipient, uint256 amount) internal returns (bytes32) {
        vm.prank(BOB);
        Token(token).approve(address(htlc), amount);
        vm.prank(BOB);
        return htlc.lock(recipient, BOB, token, amount, H, T, 0, bytes32(0));
    }

    // Rule: sha256 of a bytes32 preimage, the Dinero leaf's hash.
    function test_ClaimNeedsTheSha256Preimage() public {
        bytes32 id = lockEth(ALICE, 1 ether, 0, 0);
        vm.expectRevert(DinSwapHTLC.BadPreimage.selector);
        htlc.claim(id, bytes32(uint256(0x5a5b)));
        htlc.claim(id, S);
        eq(ALICE.balance, 1 ether, "alice paid");
        yes(htlc.preimages(id) == S, "secret recorded");

        // A keccak hashlock is not a Dinero hashlock: s never opens it.
        vm.prank(BOB);
        bytes32 k = htlc.lock{value: 1 ether}(ALICE, BOB, address(0), 1 ether, keccak256(abi.encodePacked(S)), T, 0, 0);
        vm.expectRevert(DinSwapHTLC.BadPreimage.selector);
        htlc.claim(k, S);
    }

    // Rule: claim strictly before the timelock, refund from it on.
    function test_WindowsAreExclusiveAtTheBoundary() public {
        bytes32 id = lockEth(ALICE, 1 ether, 0, 0);
        bytes32 id2 = lockEth(ALICE, 1 ether, 0, bytes32(uint256(2)));
        vm.warp(T - 1);
        vm.expectRevert(DinSwapHTLC.RefundNotOpen.selector);
        htlc.refund(id);
        htlc.claim(id2, S);
        eq(ALICE.balance, 1 ether, "alice claimed at T-1");
        vm.warp(T);
        vm.expectRevert(DinSwapHTLC.ClaimWindowClosed.selector);
        htlc.claim(id, S);
        uint256 before = BOB.balance;
        htlc.refund(id);
        eq(BOB.balance, before + 1 ether, "bob refunded at T");
    }

    // Rule: payouts go to the fixed addresses, whoever submits.
    function test_AnyoneMaySubmitButMoneyGoesToFixedAddresses() public {
        bytes32 id = lockEth(ALICE, 1 ether, 0, 0);
        vm.prank(RELAYER);
        htlc.claim(id, S);
        eq(ALICE.balance, 1 ether, "claim pays alice");
        eq(RELAYER.balance, 0, "submitter gets nothing without a tip");

        bytes32 id2 = lockEth(ALICE, 1 ether, 0, bytes32(uint256(2)));
        vm.warp(T);
        uint256 before = BOB.balance;
        vm.prank(RELAYER);
        htlc.refund(id2);
        eq(BOB.balance, before + 1 ether, "refund pays bob");
    }

    // Rule: the id binds chain, contract, sender, every term and the nonce.
    function test_IdsAreUniqueAndComputable() public {
        bytes32 id = lockEth(ALICE, 1 ether, 0, 0);
        yes(id == htlc.swapId(BOB, ALICE, BOB, address(0), 1 ether, H, T, 0, 0), "client can compute the id");
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.IdUsed.selector);
        htlc.lock{value: 1 ether}(ALICE, BOB, address(0), 1 ether, H, T, 0, 0);

        yes(htlc.swapId(RELAYER, ALICE, BOB, address(0), 1 ether, H, T, 0, 0) != id, "sender is bound");
        vm.chainId(8453);
        yes(htlc.swapId(BOB, ALICE, BOB, address(0), 1 ether, H, T, 0, 0) != id, "chain is bound");
    }

    // Rule: one exit per id; state is set before any transfer.
    function test_OneExitPerId() public {
        bytes32 id = lockEth(ALICE, 1 ether, 0, 0);
        htlc.claim(id, S);
        vm.expectRevert(DinSwapHTLC.NotOpen.selector);
        htlc.claim(id, S);
        vm.warp(T);
        vm.expectRevert(DinSwapHTLC.NotOpen.selector);
        htlc.refund(id);
    }

    function test_APayeeCannotReenter() public {
        Reenterer r = new Reenterer();
        vm.deal(address(r), 0);
        vm.prank(BOB);
        bytes32 id = htlc.lock{value: 1 ether}(address(r), BOB, address(0), 1 ether, H, T, 0, 0);
        r.set(htlc, id);
        htlc.claim(id, S);
        yes(r.blocked(), "reentrant refund was refused");
        eq(address(r).balance, 1 ether, "paid once");
    }

    // Rule: a failed payout is credited and withdrawn later.
    function test_ARejectingRecipientIsCreditedAndWithdraws() public {
        RejectEth rej = new RejectEth();
        vm.prank(BOB);
        bytes32 id = htlc.lock{value: 1 ether}(address(rej), BOB, address(0), 1 ether, H, T, 0, 0);
        htlc.claim(id, S); // must not revert: s is public now
        eq(htlc.credits(address(0), address(rej)), 1 ether, "credited");
        vm.prank(address(rej));
        htlc.withdraw(address(0), ALICE);
        eq(ALICE.balance, 1 ether, "withdrawn to another address");
        eq(htlc.credits(address(0), address(rej)), 0, "credit cleared");
        vm.prank(address(rej));
        vm.expectRevert(DinSwapHTLC.NothingToWithdraw.selector);
        htlc.withdraw(address(0), ALICE);
    }

    function test_AGasHogRecipientIsCreditedNotStuck() public {
        GasHog hog = new GasHog();
        vm.prank(BOB);
        bytes32 id = htlc.lock{value: 1 ether}(address(hog), BOB, address(0), 1 ether, H, T, 0, 0);
        htlc.claim{gas: 300_000}(id, S); // a realistic gas limit: the hog must not eat it all
        eq(htlc.credits(address(0), address(hog)), 1 ether, "credited");
    }

    function test_AFrozenTokenRecipientIsCredited() public {
        Token usdc = new Token();
        usdc.mint(BOB, 1000e6);
        bytes32 id = lockToken(address(usdc), ALICE, 1000e6);
        usdc.freeze(ALICE);
        htlc.claim(id, S);
        eq(htlc.credits(address(usdc), ALICE), 1000e6, "credited");
        vm.prank(ALICE);
        htlc.withdraw(address(usdc), RELAYER);
        eq(usdc.balanceOf(RELAYER), 1000e6, "withdrawn to a clean address");
    }

    function test_OnlyThePayeeWithdrawsTheirCredit() public {
        RejectEth rej = new RejectEth();
        vm.prank(BOB);
        bytes32 id = htlc.lock{value: 1 ether}(address(rej), BOB, address(0), 1 ether, H, T, 0, 0);
        htlc.claim(id, S);
        vm.prank(RELAYER);
        vm.expectRevert(DinSwapHTLC.NothingToWithdraw.selector);
        htlc.withdraw(address(0), RELAYER);
    }

    // Rule: exact amounts only.
    function test_FeeOnTransferTokensAreRefused() public {
        FeeToken t = new FeeToken();
        t.mint(BOB, 1000);
        vm.prank(BOB);
        t.approve(address(htlc), 1000);
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.FeeOnTransfer.selector);
        htlc.lock(ALICE, BOB, address(t), 1000, H, T, 0, 0);
    }

    function test_EthValueMustMatchAndTokenLocksTakeNoEth() public {
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.BadValue.selector);
        htlc.lock{value: 1 ether - 1}(ALICE, BOB, address(0), 1 ether, H, T, 0, 0);
        Token t = new Token();
        t.mint(BOB, 10);
        vm.prank(BOB);
        t.approve(address(htlc), 10);
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.BadValue.selector);
        htlc.lock{value: 1}(ALICE, BOB, address(t), 10, H, T, 0, 0);
    }

    function test_TokensThatReturnNothingWork() public {
        NoReturnToken usdt = new NoReturnToken();
        usdt.mint(BOB, 500);
        vm.prank(BOB);
        bytes32 id = htlc.lock(ALICE, BOB, address(usdt), 500, H, T, 0, 0);
        eq(usdt.balanceOf(address(htlc)), 500, "locked");
        htlc.claim(id, S);
        eq(usdt.balanceOf(ALICE), 500, "claimed");
    }

    function test_ALockFromAnAddressWithNoCodeFails() public {
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.TransferFailed.selector);
        htlc.lock(ALICE, BOB, address(0xDEAD), 1, H, T, 0, 0);
    }

    // Rule: timelock between now + 1 h and now + 7 d; tip <= amount.
    function test_BoundsAreEnforced() public {
        uint64 now_ = uint64(block.timestamp);
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.BadTimelock.selector);
        htlc.lock{value: 1}(ALICE, BOB, address(0), 1, H, now_ + 1 hours - 1, 0, 0);
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.BadTimelock.selector);
        htlc.lock{value: 1}(ALICE, BOB, address(0), 1, H, now_ + 7 days + 1, 0, 0);
        vm.prank(BOB);
        htlc.lock{value: 1}(ALICE, BOB, address(0), 1, H, now_ + 1 hours, 0, 0);
        vm.prank(BOB);
        htlc.lock{value: 1}(ALICE, BOB, address(0), 1, H, now_ + 7 days, 0, 0);
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.BadAmount.selector);
        htlc.lock{value: 1}(ALICE, BOB, address(0), 1, H, T, 2, 0);
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.BadAmount.selector);
        htlc.lock{value: 0}(ALICE, BOB, address(0), 0, H, T, 0, 0);
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.BadAddress.selector);
        htlc.lock{value: 1}(address(0), BOB, address(0), 1, H, T, 0, 0);
        vm.prank(BOB);
        vm.expectRevert(DinSwapHTLC.BadHashlock.selector);
        htlc.lock{value: 1}(ALICE, BOB, address(0), 1, bytes32(0), T, 0, 0);
    }

    // Rule: an optional tip goes to the claim's submitter.
    function test_TheTipPaysTheSubmitter() public {
        bytes32 id = lockEth(ALICE, 1 ether, 0.01 ether, 0);
        vm.prank(RELAYER);
        htlc.claim(id, S);
        eq(ALICE.balance, 0.99 ether, "recipient gets amount - tip");
        eq(RELAYER.balance, 0.01 ether, "relayer gets the tip");
    }

    function test_ARefundNeverPaysTheTip() public {
        bytes32 id = lockEth(ALICE, 1 ether, 0.01 ether, 0);
        vm.warp(T);
        uint256 before = BOB.balance;
        vm.prank(RELAYER);
        htlc.refund(id);
        eq(BOB.balance, before + 1 ether, "full refund");
        eq(RELAYER.balance, 0, "no tip on refund");
    }

    // Bob locks from a per-swap key; his refund goes to his own wallet.
    function test_ARefundGoesToRefundToNotTheSender() public {
        address wallet = address(0xB0B2);
        vm.prank(BOB);
        bytes32 id = htlc.lock{value: 1 ether}(ALICE, wallet, address(0), 1 ether, H, T, 0, 0);
        vm.warp(T);
        uint256 before = BOB.balance;
        htlc.refund(id);
        eq(wallet.balance, 1 ether, "refundTo paid");
        eq(BOB.balance, before, "sender not paid");
    }

    function test_ATokenTipPaysTheSubmitter() public {
        Token usdc = new Token();
        usdc.mint(BOB, 1000e6);
        vm.prank(BOB);
        usdc.approve(address(htlc), 1000e6);
        vm.prank(BOB);
        bytes32 id = htlc.lock(ALICE, BOB, address(usdc), 1000e6, H, T, 2e6, 0);
        vm.prank(RELAYER);
        htlc.claim(id, S);
        eq(usdc.balanceOf(ALICE), 998e6, "recipient gets amount - tip");
        eq(usdc.balanceOf(RELAYER), 2e6, "relayer gets the tip");
    }

    function test_AFrozenRefundAddressIsCredited() public {
        Token usdc = new Token();
        usdc.mint(BOB, 1000e6);
        bytes32 id = lockToken(address(usdc), ALICE, 1000e6);
        usdc.freeze(BOB);
        vm.warp(T);
        htlc.refund(id); // must not revert: Bob withdraws to another address
        eq(htlc.credits(address(usdc), BOB), 1000e6, "credited");
        vm.prank(BOB);
        htlc.withdraw(address(usdc), RELAYER);
        eq(usdc.balanceOf(RELAYER), 1000e6, "withdrawn to a clean address");
    }

    // ---- fuzz -------------------------------------------------------------

    function fuzzLock(uint96 amount, uint96 tip, bytes32 s, uint32 lockFor) internal returns (bytes32, uint64) {
        uint64 t = uint64(block.timestamp) + uint64(1 hours + uint256(lockFor) % (7 days - 1 hours + 1));
        vm.deal(BOB, amount);
        vm.prank(BOB);
        return (htlc.lock{value: amount}(ALICE, BOB, address(0), amount, sha256(abi.encodePacked(s)), t, tip, 0), t);
    }

    function testFuzz_ClaimPaysExactly(uint96 amount, uint96 tip, bytes32 s, uint32 lockFor, uint32 wait) public {
        vm.assume(amount > 0 && tip <= amount);
        (bytes32 id, uint64 t) = fuzzLock(amount, tip, s, lockFor);
        vm.warp(block.timestamp + uint256(wait) % (t - block.timestamp));
        vm.prank(RELAYER);
        htlc.claim(id, s);
        eq(ALICE.balance, uint256(amount) - tip, "recipient");
        eq(RELAYER.balance, tip, "tip");
        eq(address(htlc).balance, 0, "nothing left");
    }

    function testFuzz_AWrongPreimageNeverClaims(bytes32 s, bytes32 wrong) public {
        vm.assume(s != wrong);
        bytes32 h = sha256(abi.encodePacked(s));
        vm.prank(BOB);
        bytes32 id = htlc.lock{value: 1}(ALICE, BOB, address(0), 1, h, T, 0, 0);
        vm.expectRevert(DinSwapHTLC.BadPreimage.selector);
        htlc.claim(id, wrong);
    }

    function testFuzz_NoClaimFromTheTimelockOn(uint32 late) public {
        bytes32 id = lockEth(ALICE, 1 ether, 0, 0);
        vm.warp(uint256(T) + late);
        vm.expectRevert(DinSwapHTLC.ClaimWindowClosed.selector);
        htlc.claim(id, S);
    }
}

// ---- accounting invariant -------------------------------------------------

contract Handler is Base {
    DinSwapHTLC public htlc;
    RejectEth public rejecter = new RejectEth();
    bytes32[] public ids;
    bytes32 constant S = bytes32(uint256(7));
    uint256 public openEth;
    uint256 public creditedEth;
    uint256 nonce;

    constructor(DinSwapHTLC h) {
        htlc = h;
    }

    function lock(uint96 amount, bool rejecting, uint32 lockFor) external {
        if (amount == 0) return;
        address recipient = rejecting ? address(rejecter) : address(0xA11CE);
        uint64 t = uint64(block.timestamp) + uint64(1 hours + uint256(lockFor) % (7 days - 1 hours));
        vm.deal(address(this), amount);
        bytes32 id = htlc.lock{value: amount}(recipient, address(0xB0B), address(0), amount,
            sha256(abi.encodePacked(S)), t, 0, bytes32(nonce++));
        ids.push(id);
        openEth += amount;
    }

    function claim(uint256 i) external {
        if (ids.length == 0) return;
        bytes32 id = ids[i % ids.length];
        (,address recipient,,, uint256 amount,,, uint64 t, DinSwapHTLC.State st) = htlc.swaps(id);
        if (st != DinSwapHTLC.State.Open || block.timestamp >= t) return;
        htlc.claim(id, S);
        openEth -= amount;
        if (recipient == address(rejecter)) creditedEth += amount;
    }

    function refund(uint256 i) external {
        if (ids.length == 0) return;
        bytes32 id = ids[i % ids.length];
        (,,,, uint256 amount,,, uint64 t, DinSwapHTLC.State st) = htlc.swaps(id);
        if (st != DinSwapHTLC.State.Open || block.timestamp < t) return;
        htlc.refund(id);
        openEth -= amount;
    }

    function warp(uint32 by) external {
        vm.warp(block.timestamp + uint256(by) % 3 days);
    }
}

contract DinSwapHTLCInvariants is Base {
    DinSwapHTLC htlc;
    Handler handler;

    function setUp() public {
        htlc = new DinSwapHTLC();
        handler = new Handler(htlc);
    }

    function targetContracts() public view returns (address[] memory t) {
        t = new address[](1);
        t[0] = address(handler);
    }

    // The contract always holds exactly the open locks plus the credits.
    function invariant_BalanceEqualsOpenLocksPlusCredits() public view {
        eq(address(htlc).balance, handler.openEth() + handler.creditedEth(), "balance");
        eq(htlc.credits(address(0), address(handler.rejecter())), handler.creditedEth(), "credits");
    }
}
