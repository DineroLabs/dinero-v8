// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

/// @title DIN <-> ETH / ERC-20 hash-time-locked swaps
/// @notice The Ethereum leg of docs/design/din-eth-atomic-swaps.md. Bob locks
/// ETH or a token for Alice under SHA-256(s); Alice claims with s before the
/// timelock, and s then lets Bob claim Alice's DIN on Dinero. After the
/// timelock only a refund to Bob is possible.
/// @dev Immutable and ownerless: no admin, proxy, pause, fee or self-destruct.
/// Anyone may submit a claim or refund; money only ever goes to the addresses
/// fixed at lock time. A payout that fails is credited to the payee, who
/// withdraws it later, so a rejecting or frozen payee can never make a claim
/// revert after s is public.
contract DinSwapHTLC {
    enum State {
        None,
        Open,
        Claimed,
        Refunded
    }

    struct Swap {
        address sender;
        address recipient; // Alice: receives the amount (minus tip) on claim
        address refundTo; // Bob: receives the amount on refund
        address token; // address(0) = ETH
        uint256 amount;
        uint256 tip; // paid to whoever submits the claim (a relayer); 0 by default
        bytes32 hashlock; // SHA-256 of the 32-byte secret, as on the Dinero leg
        uint64 timelock; // claim strictly before, refund from this time on
        State state;
    }

    uint256 public constant MIN_LOCK = 1 hours;
    uint256 public constant MAX_LOCK = 7 days;
    // An ETH payout gets this much gas; a payee needing more is credited instead.
    uint256 public constant ETH_PAYOUT_GAS = 100_000;

    mapping(bytes32 => Swap) public swaps;
    mapping(bytes32 => bytes32) public preimages; // id => s, once claimed
    mapping(address => mapping(address => uint256)) public credits; // token => payee => amount

    uint256 private entered = 1;

    event Locked(
        bytes32 indexed id,
        address sender,
        address recipient,
        address refundTo,
        address token,
        uint256 amount,
        bytes32 hashlock,
        uint64 timelock,
        uint256 tip
    );
    event Claimed(bytes32 indexed id, bytes32 preimage);
    event Refunded(bytes32 indexed id);
    event Credited(address indexed token, address indexed payee, uint256 amount);
    event Withdrawn(address indexed token, address indexed payee, address to, uint256 amount);

    error BadAddress();
    error BadAmount();
    error BadHashlock();
    error BadTimelock();
    error BadValue();
    error IdUsed();
    error NotOpen();
    error ClaimWindowClosed();
    error RefundNotOpen();
    error BadPreimage();
    error FeeOnTransfer();
    error TransferFailed();
    error NothingToWithdraw();
    error Reentered();

    modifier nonReentrant() {
        if (entered != 1) revert Reentered();
        entered = 2;
        _;
        entered = 1;
    }

    /// @notice The id a lock with these terms gets. Binds the chain, this
    /// contract and the sender, so a stranger cannot pre-lock Bob's id and the
    /// same terms never collide across chains or deployments.
    function swapId(
        address sender,
        address recipient,
        address refundTo,
        address token,
        uint256 amount,
        bytes32 hashlock,
        uint64 timelock,
        uint256 tip,
        bytes32 nonce
    ) public view returns (bytes32) {
        return keccak256(
            abi.encode(
                block.chainid, address(this), sender, recipient, refundTo, token, amount, hashlock, timelock, tip, nonce
            )
        );
    }

    function lock(
        address recipient,
        address refundTo,
        address token,
        uint256 amount,
        bytes32 hashlock,
        uint64 timelock,
        uint256 tip,
        bytes32 nonce
    ) external payable nonReentrant returns (bytes32 id) {
        if (recipient == address(0) || refundTo == address(0)) revert BadAddress();
        if (amount == 0 || tip > amount) revert BadAmount();
        if (hashlock == bytes32(0)) revert BadHashlock();
        if (timelock < block.timestamp + MIN_LOCK || timelock > block.timestamp + MAX_LOCK) revert BadTimelock();
        id = swapId(msg.sender, recipient, refundTo, token, amount, hashlock, timelock, tip, nonce);
        if (swaps[id].state != State.None) revert IdUsed();
        swaps[id] = Swap(msg.sender, recipient, refundTo, token, amount, tip, hashlock, timelock, State.Open);

        if (token == address(0)) {
            if (msg.value != amount) revert BadValue();
        } else {
            if (msg.value != 0) revert BadValue();
            uint256 before = _balanceOf(token);
            if (!_callToken(token, abi.encodeWithSelector(0x23b872dd, msg.sender, address(this), amount))) {
                revert TransferFailed(); // transferFrom
            }
            if (_balanceOf(token) != before + amount) revert FeeOnTransfer();
        }
        emit Locked(id, msg.sender, recipient, refundTo, token, amount, hashlock, timelock, tip);
    }

    function claim(bytes32 id, bytes32 preimage) external nonReentrant {
        Swap storage s = swaps[id];
        if (s.state != State.Open) revert NotOpen();
        if (block.timestamp >= s.timelock) revert ClaimWindowClosed();
        if (sha256(abi.encodePacked(preimage)) != s.hashlock) revert BadPreimage();
        s.state = State.Claimed;
        preimages[id] = preimage;
        emit Claimed(id, preimage);
        uint256 tip = s.tip;
        _pay(s.token, s.recipient, s.amount - tip);
        if (tip != 0) _pay(s.token, msg.sender, tip);
    }

    function refund(bytes32 id) external nonReentrant {
        Swap storage s = swaps[id];
        if (s.state != State.Open) revert NotOpen();
        if (block.timestamp < s.timelock) revert RefundNotOpen();
        s.state = State.Refunded;
        emit Refunded(id);
        _pay(s.token, s.refundTo, s.amount);
    }

    /// @notice A payee takes a credited payout, to any address of theirs.
    function withdraw(address token, address to) external nonReentrant {
        uint256 amount = credits[token][msg.sender];
        if (amount == 0) revert NothingToWithdraw();
        if (to == address(0)) revert BadAddress();
        credits[token][msg.sender] = 0;
        emit Withdrawn(token, msg.sender, to, amount);
        bool ok = token == address(0) ? _sendEth(to, amount, gasleft()) : _transferToken(token, to, amount);
        if (!ok) revert TransferFailed();
    }

    // ---- internals --------------------------------------------------------

    function _pay(address token, address to, uint256 amount) private {
        bool ok = token == address(0) ? _sendEth(to, amount, ETH_PAYOUT_GAS) : _transferToken(token, to, amount);
        if (!ok) {
            credits[token][to] += amount;
            emit Credited(token, to, amount);
        }
    }

    // No return data is copied, so a payee cannot make the payout run out of gas.
    function _sendEth(address to, uint256 amount, uint256 gasLimit) private returns (bool ok) {
        assembly {
            ok := call(gasLimit, to, amount, 0, 0, 0, 0)
        }
    }

    function _transferToken(address token, address to, uint256 amount) private returns (bool) {
        return _callToken(token, abi.encodeWithSelector(0xa9059cbb, to, amount)); // transfer
    }

    // Succeeds for tokens that return true and for tokens that return nothing
    // (USDT); fails for a revert or a false return. An address with no code
    // never gets here: lock() reads its balanceOf first, which reverts.
    function _callToken(address token, bytes memory data) private returns (bool ok) {
        uint256 size;
        uint256 word;
        assembly {
            ok := call(gas(), token, 0, add(data, 0x20), mload(data), 0, 0x20)
            size := returndatasize()
            word := mload(0)
        }
        if (!ok) return false;
        return size == 0 || (size >= 32 && word == 1);
    }

    function _balanceOf(address token) private view returns (uint256 bal) {
        (bool ok, bytes memory ret) = token.staticcall(abi.encodeWithSelector(0x70a08231, address(this)));
        if (!ok || ret.length < 32) revert TransferFailed(); // balanceOf
        bal = abi.decode(ret, (uint256));
    }
}
