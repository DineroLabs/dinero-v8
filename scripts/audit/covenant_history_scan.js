#!/usr/bin/env node

/**
 * Scan a Dinero chain for Taproot outputs and covenant opcode use.
 *
 * This is intentionally dependency-free. JSON-RPC pins the canonical tip and
 * chain identity; local blkNNNNN.dat records provide the transactions. It
 * mirrors TransactionSerializer::Deserialize and Transaction::Serialize(false)
 * so that outpoints can be followed without a transaction index.
 *
 * Usage:
 *   node scripts/audit/covenant_history_scan.js \
 *     --cookie "/path/to/datadir/.cookie" --rpc-port 20998 \
 *     --output /tmp/covenant-history.json
 */

"use strict";

const crypto = require("crypto");
const fs = require("fs");
const http = require("http");

const COVENANT_OPCODES = new Map([
  [0xb3, "OP_CHECKTEMPLATEVERIFY"],
  [0xbb, "OP_CHECKSIGFROMSTACK"],
  [0xbc, "OP_CHECKSIGFROMSTACKVERIFY"],
  [0xbd, "OP_TXHASH"],
  [0xbe, "OP_CHECKCONTRACTVERIFY"],
]);

function fail(message) {
  process.stderr.write(`error: ${message}\n`);
  process.exit(1);
}

function parseArgs(argv) {
  const options = {
    rpcHost: "127.0.0.1",
    rpcPort: 20998,
    cookie: "",
    blocksDir: "",
    output: "",
    endHeight: null,
    progressEvery: 1000,
  };

  for (let i = 2; i < argv.length; ++i) {
    const argument = argv[i];
    const value = argv[i + 1];
    if (argument === "--rpc-host" && value) {
      options.rpcHost = value;
      ++i;
    } else if (argument === "--rpc-port" && value) {
      options.rpcPort = Number(value);
      ++i;
    } else if (argument === "--cookie" && value) {
      options.cookie = value;
      ++i;
    } else if (argument === "--output" && value) {
      options.output = value;
      ++i;
    } else if (argument === "--blocks-dir" && value) {
      options.blocksDir = value;
      ++i;
    } else if (argument === "--end-height" && value) {
      options.endHeight = Number(value);
      ++i;
    } else if (argument === "--progress-every" && value) {
      options.progressEvery = Number(value);
      ++i;
    } else {
      fail(`unknown or incomplete argument: ${argument}`);
    }
  }

  if (!options.cookie) fail("--cookie is required");
  if (!options.output) fail("--output is required");
  if (!Number.isInteger(options.rpcPort) || options.rpcPort < 1) {
    fail("--rpc-port must be a positive integer");
  }
  if (options.endHeight !== null &&
      (!Number.isInteger(options.endHeight) || options.endHeight < 0)) {
    fail("--end-height must be a non-negative integer");
  }
  if (!Number.isInteger(options.progressEvery) || options.progressEvery < 1) {
    fail("--progress-every must be a positive integer");
  }
  return options;
}

class Reader {
  constructor(buffer, position = 0) {
    this.buffer = buffer;
    this.position = position;
  }

  remaining() {
    return this.buffer.length - this.position;
  }

  require(length) {
    if (!Number.isSafeInteger(length) || length < 0 ||
        this.position + length > this.buffer.length) {
      throw new Error(`buffer underflow at byte ${this.position}, need ${length}`);
    }
  }

  readU8() {
    this.require(1);
    return this.buffer[this.position++];
  }

  readU32() {
    this.require(4);
    const value = this.buffer.readUInt32LE(this.position);
    this.position += 4;
    return value;
  }

  readU64() {
    this.require(8);
    const value = this.buffer.readBigUInt64LE(this.position);
    this.position += 8;
    return value;
  }

  readBytes(length) {
    this.require(length);
    const value = this.buffer.subarray(this.position, this.position + length);
    this.position += length;
    return value;
  }

  readCompactSize() {
    const first = this.readU8();
    let value;
    if (first < 0xfd) {
      value = BigInt(first);
    } else if (first === 0xfd) {
      this.require(2);
      value = BigInt(this.buffer.readUInt16LE(this.position));
      this.position += 2;
      if (value < 0xfdn) throw new Error("non-canonical CompactSize");
    } else if (first === 0xfe) {
      value = BigInt(this.readU32());
      if (value <= 0xffffn) throw new Error("non-canonical CompactSize");
    } else {
      value = this.readU64();
      if (value <= 0xffffffffn) throw new Error("non-canonical CompactSize");
    }
    if (value > BigInt(Number.MAX_SAFE_INTEGER)) {
      throw new Error("CompactSize exceeds JavaScript safe integer");
    }
    return Number(value);
  }

  readVarBytes() {
    return this.readBytes(this.readCompactSize());
  }
}

function compactSize(value) {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error(`invalid CompactSize value ${value}`);
  }
  if (value < 0xfd) return Buffer.from([value]);
  if (value <= 0xffff) {
    const result = Buffer.alloc(3);
    result[0] = 0xfd;
    result.writeUInt16LE(value, 1);
    return result;
  }
  if (value <= 0xffffffff) {
    const result = Buffer.alloc(5);
    result[0] = 0xfe;
    result.writeUInt32LE(value, 1);
    return result;
  }
  const result = Buffer.alloc(9);
  result[0] = 0xff;
  result.writeBigUInt64LE(BigInt(value), 1);
  return result;
}

function encodeU32(value) {
  const result = Buffer.alloc(4);
  result.writeUInt32LE(value >>> 0);
  return result;
}

function encodeU64(value) {
  const result = Buffer.alloc(8);
  result.writeBigUInt64LE(value);
  return result;
}

function encodeVarBytes(value) {
  return Buffer.concat([compactSize(value.length), value]);
}

function doubleSha256(value) {
  const first = crypto.createHash("sha256").update(value).digest();
  return crypto.createHash("sha256").update(first).digest();
}

function fnv1a32(value) {
  let hash = 0x811c9dc5;
  for (const byte of value) {
    hash = Math.imul((hash ^ byte) >>> 0, 0x01000193) >>> 0;
  }
  return hash;
}

function loadCanonicalBlocks(blocksDir, endHash, endHeight) {
  const fileNames = fs.readdirSync(blocksDir)
    .filter((name) => /^blk[0-9]{5}\.dat$/.test(name))
    .sort();
  if (fileNames.length === 0) {
    throw new Error(`no blkNNNNN.dat files found in ${blocksDir}`);
  }

  const records = new Map();
  let expectedMagic = null;
  for (const fileName of fileNames) {
    const filePath = `${blocksDir}/${fileName}`;
    const file = fs.readFileSync(filePath);
    let position = 0;
    while (position < file.length) {
      if (position + 8 > file.length) {
        throw new Error(`${fileName}: incomplete record header at ${position}`);
      }
      const magic = file.readUInt32LE(position);
      const blockSize = file.readUInt32LE(position + 4);
      if (expectedMagic === null) expectedMagic = magic;
      if (magic !== expectedMagic) {
        throw new Error(`${fileName}: network magic changed at ${position}`);
      }
      if (blockSize < 129 || blockSize > 4_000_000) {
        throw new Error(`${fileName}: invalid block size ${blockSize} at ${position}`);
      }
      const recordEnd = position + 12 + blockSize;
      if (recordEnd > file.length) {
        throw new Error(`${fileName}: incomplete block record at ${position}`);
      }
      const block = file.subarray(position + 8, position + 8 + blockSize);
      const storedChecksum = file.readUInt32LE(position + 8 + blockSize);
      const calculatedChecksum = fnv1a32(block);
      if (storedChecksum !== calculatedChecksum) {
        throw new Error(
          `${fileName}: checksum mismatch at ${position}: ` +
          `${storedChecksum.toString(16)} != ${calculatedChecksum.toString(16)}`
        );
      }
      const hash = doubleSha256(block.subarray(0, 128)).toString("hex");
      const previousHash = Buffer.from(block.subarray(4, 36))
        .reverse()
        .toString("hex");
      records.set(hash, {hash, previousHash, block});
      position = recordEnd;
    }
  }

  const reverseChain = [];
  let cursor = endHash;
  while (true) {
    const record = records.get(cursor);
    if (!record) {
      throw new Error(`canonical block ${cursor} is absent from ${blocksDir}`);
    }
    reverseChain.push(record);
    if (/^0+$/.test(record.previousHash)) break;
    cursor = record.previousHash;
    if (reverseChain.length > endHeight + 1) {
      throw new Error("canonical block walk exceeded expected chain height");
    }
  }
  reverseChain.reverse();
  if (reverseChain.length !== endHeight + 1) {
    throw new Error(
      `canonical block walk has ${reverseChain.length} blocks, ` +
      `expected ${endHeight + 1}`
    );
  }
  return reverseChain;
}

function isP2TR(scriptPubKey) {
  return scriptPubKey.length === 34 &&
    scriptPubKey[0] === 0x51 &&
    scriptPubKey[1] === 0x20;
}

function parseTransaction(reader) {
  const version = reader.readU32();
  let segwit = false;
  if (reader.remaining() >= 2 &&
      reader.buffer[reader.position] === 0x00 &&
      reader.buffer[reader.position + 1] === 0x01) {
    segwit = true;
    reader.position += 2;
  }

  const inputs = [];
  const inputCount = reader.readCompactSize();
  for (let i = 0; i < inputCount; ++i) {
    inputs.push({
      txid: reader.readBytes(32),
      vout: reader.readU32(),
      scriptSig: reader.readVarBytes(),
      sequence: reader.readU32(),
      witness: [],
    });
  }

  let hasConfidentialOutputs = false;
  const outputs = [];
  const outputCount = reader.readCompactSize();
  for (let i = 0; i < outputCount; ++i) {
    const value = reader.readU64();
    const scriptPubKey = reader.readVarBytes();
    const output = {
      value,
      scriptPubKey,
      confidential: false,
      commitment: Buffer.alloc(0),
      rangeProof: Buffer.alloc(0),
      nonce: Buffer.alloc(0),
    };

    // Match TransactionSerializer::Deserialize exactly: a zero-valued output
    // is confidential only if three following varbytes parse and have the
    // required commitment/nonce lengths. Otherwise rewind.
    if (value === 0n) {
      const afterScript = reader.position;
      try {
        const commitment = reader.readVarBytes();
        const rangeProof = reader.readVarBytes();
        const nonce = reader.readVarBytes();
        if (commitment.length === 33 && nonce.length === 65) {
          output.confidential = true;
          output.commitment = commitment;
          output.rangeProof = rangeProof;
          output.nonce = nonce;
          hasConfidentialOutputs = true;
        } else {
          reader.position = afterScript;
        }
      } catch (_) {
        reader.position = afterScript;
      }
    }
    outputs.push(output);
  }

  const shielded = version === 5 || version === 6;
  let feeMarker = null;
  let explicitFee = null;
  if (hasConfidentialOutputs || shielded) {
    feeMarker = reader.readU8();
    if (feeMarker === 1) {
      explicitFee = reader.readU64();
    } else if (feeMarker !== 0) {
      throw new Error(`invalid explicit fee marker ${feeMarker}`);
    }
  }

  if (segwit) {
    for (const input of inputs) {
      const itemCount = reader.readCompactSize();
      for (let i = 0; i < itemCount; ++i) {
        input.witness.push(reader.readVarBytes());
      }
    }
  }

  let shieldedBundle = Buffer.alloc(0);
  if (shielded) shieldedBundle = reader.readVarBytes();
  const lockTime = reader.readU32();

  const baseParts = [encodeU32(version), compactSize(inputs.length)];
  for (const input of inputs) {
    baseParts.push(input.txid);
    baseParts.push(encodeU32(input.vout));
    baseParts.push(encodeVarBytes(input.scriptSig));
    baseParts.push(encodeU32(input.sequence));
  }
  baseParts.push(compactSize(outputs.length));
  for (const output of outputs) {
    if (output.confidential) {
      baseParts.push(encodeU64(0n));
      baseParts.push(encodeVarBytes(output.scriptPubKey));
      baseParts.push(encodeVarBytes(output.commitment));
      baseParts.push(encodeVarBytes(output.rangeProof));
      baseParts.push(encodeVarBytes(output.nonce));
    } else {
      baseParts.push(encodeU64(output.value));
      baseParts.push(encodeVarBytes(output.scriptPubKey));
    }
  }
  if (feeMarker !== null) {
    baseParts.push(Buffer.from([feeMarker]));
    if (feeMarker === 1) baseParts.push(encodeU64(explicitFee));
  }
  // Dinero v5 intentionally excludes its shielded bundle from txid; v6
  // includes it.
  if (version === 6 && shieldedBundle.length !== 0) {
    baseParts.push(encodeVarBytes(shieldedBundle));
  }
  baseParts.push(encodeU32(lockTime));

  return {
    txid: doubleSha256(Buffer.concat(baseParts)),
    version,
    inputs,
    outputs,
  };
}

function covenantOpcodes(script) {
  const found = [];
  for (let position = 0; position < script.length;) {
    const opcode = script[position++];
    if (COVENANT_OPCODES.has(opcode)) {
      found.push({opcode, name: COVENANT_OPCODES.get(opcode), position: position - 1});
    }

    let pushLength = 0;
    if (opcode >= 0x01 && opcode <= 0x4b) {
      pushLength = opcode;
    } else if (opcode === 0x4c) {
      if (position + 1 > script.length) break;
      pushLength = script[position++];
    } else if (opcode === 0x4d) {
      if (position + 2 > script.length) break;
      pushLength = script.readUInt16LE(position);
      position += 2;
    } else if (opcode === 0x4e) {
      if (position + 4 > script.length) break;
      pushLength = script.readUInt32LE(position);
      position += 4;
    }
    if (pushLength > script.length - position) break;
    position += pushLength;
  }
  return found;
}

function classifyP2TRWitness(witness) {
  const stack = witness.slice();
  let annex = false;
  if (stack.length >= 2 && stack.at(-1).length > 0 && stack.at(-1)[0] === 0x50) {
    stack.pop();
    annex = true;
  }
  if (stack.length === 1) {
    const signature = stack[0];
    const sighash = signature.length === 64
      ? "default"
      : signature.length === 65
        ? `0x${signature[64].toString(16).padStart(2, "0")}`
        : `invalid-length-${signature.length}`;
    return {
      kind: "keypath",
      annex,
      signatureLength: signature.length,
      sighash,
    };
  }
  if (stack.length >= 2) {
    return {
      kind: "scriptpath",
      annex,
      script: stack.at(-2),
      control: stack.at(-1),
    };
  }
  return {kind: "malformed", annex};
}

function samplePush(array, value, limit = 50) {
  if (array.length < limit) array.push(value);
}

async function main() {
  const options = parseArgs(process.argv);
  const cookie = fs.readFileSync(options.cookie, "utf8").trim();
  if (!cookie.includes(":")) fail("cookie file has unexpected format");

  // Keep the process referenced while macOS queues many short-lived loopback
  // sockets. Without this guard, Node can exit successfully while an awaited
  // request has not yet been assigned a referenced socket.
  const processGuard = setInterval(() => {}, 1000);
  let requestId = 0;
  async function rpcOnce(method, params = []) {
    const payload = JSON.stringify({
      jsonrpc: "2.0",
      id: ++requestId,
      method,
      params,
    });
    const response = await new Promise((resolve, reject) => {
      const request = http.request({
        host: options.rpcHost,
        port: options.rpcPort,
        method: "POST",
        path: "/",
        agent: false,
        auth: cookie,
        headers: {
          "content-type": "application/json",
          "content-length": Buffer.byteLength(payload),
        },
        timeout: 30000,
      }, (incoming) => {
        const chunks = [];
        incoming.on("data", (chunk) => chunks.push(chunk));
        incoming.on("end", () => {
          if (incoming.statusCode !== 200) {
            reject(new Error(`RPC HTTP status ${incoming.statusCode}`));
            return;
          }
          try {
            resolve(JSON.parse(Buffer.concat(chunks).toString("utf8")));
          } catch (error) {
            reject(new Error(`invalid RPC JSON: ${error.message}`));
          }
        });
      });
      request.on("timeout", () => request.destroy(new Error("RPC timeout")));
      request.on("error", reject);
      request.end(payload);
    });
    if (response.error) {
      throw new Error(`${method}: ${JSON.stringify(response.error)}`);
    }
    return response.result;
  }

  async function rpc(method, params = []) {
    let lastError;
    for (let attempt = 1; attempt <= 5; ++attempt) {
      try {
        return await rpcOnce(method, params);
      } catch (error) {
        lastError = error;
        if (attempt < 5) {
          await new Promise((resolve) => setTimeout(resolve, attempt * 100));
        }
      }
    }
    throw new Error(`${method} failed after 5 attempts: ${lastError.message}`);
  }

  const info = await rpc("getblockchaininfo");
  if (info.chain !== "mainnet") {
    fail(`refusing to label a non-mainnet scan: RPC reports ${info.chain}`);
  }
  const tipAtStart = Number(info.blocks);
  const endHeight = options.endHeight === null
    ? tipAtStart
    : Math.min(options.endHeight, tipAtStart);
  const endHash = await rpc("getblockhash", [endHeight]);
  const canonicalBlocks = options.blocksDir
    ? loadCanonicalBlocks(options.blocksDir, endHash, endHeight)
    : null;

  const p2trUtxos = new Map();
  const summary = {
    schema: 2,
    network: info.chain,
    source: canonicalBlocks ? "canonical-walk-from-block-files" : "json-rpc",
    startedAt: new Date().toISOString(),
    range: {
      startHeight: 0,
      endHeight,
      endHash,
      tipAtStart,
    },
    totals: {
      blocks: 0,
      transactions: 0,
      inputs: 0,
      outputs: 0,
      p2trOutputsCreated: 0,
      p2trOutputsSpent: 0,
      p2trOutputsUnspent: 0,
      p2trKeypathSpends: 0,
      p2trScriptpathSpends: 0,
      p2trMalformedSpends: 0,
      p2trAnnexSpends: 0,
      revealedCovenantOpcodes: 0,
      bareCovenantOpcodes: 0,
    },
    firstSeen: {},
    revealedOpcodeCounts: {},
    bareOpcodeCounts: {},
    keypathSighashCounts: {},
    samples: {
      p2trOutputs: [],
      p2trSpends: [],
      revealedCovenantOpcodes: [],
      bareCovenantOpcodes: [],
      unspentP2trOutputs: [],
    },
  };

  function firstSeen(key, height) {
    if (summary.firstSeen[key] === undefined) summary.firstSeen[key] = height;
  }

  const fetchBatchSize = canonicalBlocks ? 1000 : 32;
  for (let batchStart = 0; batchStart <= endHeight; batchStart += fetchBatchSize) {
    const batchEnd = Math.min(endHeight, batchStart + fetchBatchSize - 1);
    const heights = Array.from(
      {length: batchEnd - batchStart + 1},
      (_, index) => batchStart + index
    );
    const rawBlocks = [];
    if (canonicalBlocks) {
      for (const height of heights) rawBlocks.push(canonicalBlocks[height].block);
    } else {
      // Keep the fallback deliberately sequential. The daemon's HTTP server
      // can leave concurrent local requests queued indefinitely; block files
      // are the fast path for a full audit.
      for (const height of heights) {
        const hash = height === endHeight ? endHash : await rpc("getblockhash", [height]);
        rawBlocks.push(Buffer.from(await rpc("getblock", [hash, 0]), "hex"));
      }
    }

    for (let batchIndex = 0; batchIndex < heights.length; ++batchIndex) {
      const height = heights[batchIndex];
      const block = rawBlocks[batchIndex];
    const reader = new Reader(block, 128);
    const transactionCount = reader.readCompactSize();

    for (let transactionIndex = 0; transactionIndex < transactionCount; ++transactionIndex) {
      let transaction;
      try {
        transaction = parseTransaction(reader);
      } catch (error) {
        throw new Error(
          `parse failure at height ${height}, tx ${transactionIndex}: ${error.message}`
        );
      }
      const txidHex = transaction.txid.toString("hex");
      ++summary.totals.transactions;
      summary.totals.inputs += transaction.inputs.length;
      summary.totals.outputs += transaction.outputs.length;

      for (let inputIndex = 0; inputIndex < transaction.inputs.length; ++inputIndex) {
        const input = transaction.inputs[inputIndex];
        const outpoint = `${input.txid.toString("hex")}:${input.vout}`;
        const prior = p2trUtxos.get(outpoint);
        if (!prior) continue;

        p2trUtxos.delete(outpoint);
        ++summary.totals.p2trOutputsSpent;
        const classification = classifyP2TRWitness(input.witness);
        const sample = {
          height,
          txidRaw: txidHex,
          inputIndex,
          priorHeight: prior.height,
          kind: classification.kind,
          annex: classification.annex,
        };
        samplePush(summary.samples.p2trSpends, sample);
        if (classification.annex) ++summary.totals.p2trAnnexSpends;

        if (classification.kind === "keypath") {
          ++summary.totals.p2trKeypathSpends;
          sample.signatureLength = classification.signatureLength;
          sample.sighash = classification.sighash;
          summary.keypathSighashCounts[classification.sighash] =
            (summary.keypathSighashCounts[classification.sighash] || 0) + 1;
          firstSeen("p2trKeypathSpend", height);
        } else if (classification.kind === "scriptpath") {
          ++summary.totals.p2trScriptpathSpends;
          firstSeen("p2trScriptpathSpend", height);
          const opcodes = covenantOpcodes(classification.script);
          for (const occurrence of opcodes) {
            ++summary.totals.revealedCovenantOpcodes;
            summary.revealedOpcodeCounts[occurrence.name] =
              (summary.revealedOpcodeCounts[occurrence.name] || 0) + 1;
            firstSeen(`revealed:${occurrence.name}`, height);
            samplePush(summary.samples.revealedCovenantOpcodes, {
              height,
              txidRaw: txidHex,
              inputIndex,
              priorHeight: prior.height,
              leafVersion: classification.control.length > 0
                ? classification.control[0] & 0xfe
                : null,
              ...occurrence,
            });
          }
        } else {
          ++summary.totals.p2trMalformedSpends;
          firstSeen("p2trMalformedSpend", height);
        }
      }

      for (let outputIndex = 0; outputIndex < transaction.outputs.length; ++outputIndex) {
        const output = transaction.outputs[outputIndex];
        if (isP2TR(output.scriptPubKey)) {
          const outpoint = `${txidHex}:${outputIndex}`;
          const record = {
            height,
            txidRaw: txidHex,
            outputIndex,
            confidential: output.confidential,
          };
          p2trUtxos.set(outpoint, record);
          ++summary.totals.p2trOutputsCreated;
          firstSeen("p2trOutput", height);
          samplePush(summary.samples.p2trOutputs, record);
        }

        const opcodes = covenantOpcodes(output.scriptPubKey);
        for (const occurrence of opcodes) {
          ++summary.totals.bareCovenantOpcodes;
          summary.bareOpcodeCounts[occurrence.name] =
            (summary.bareOpcodeCounts[occurrence.name] || 0) + 1;
          firstSeen(`bare:${occurrence.name}`, height);
          samplePush(summary.samples.bareCovenantOpcodes, {
            height,
            txidRaw: txidHex,
            outputIndex,
            ...occurrence,
          });
        }
      }
    }

      ++summary.totals.blocks;
      if (height % options.progressEvery === 0 || height === endHeight) {
        process.stderr.write(
          `height ${height}/${endHeight}: ${summary.totals.transactions} tx, ` +
          `${summary.totals.p2trOutputsCreated} P2TR created, ` +
          `${p2trUtxos.size} P2TR unspent\n`
        );
      }
    }
  }

  summary.completedAt = new Date().toISOString();
  summary.totals.p2trOutputsUnspent = p2trUtxos.size;
  for (const record of p2trUtxos.values()) {
    samplePush(summary.samples.unspentP2trOutputs, record);
  }

  fs.writeFileSync(options.output, `${JSON.stringify(summary, null, 2)}\n`, {
    flag: "wx",
  });
  clearInterval(processGuard);
  process.stdout.write(`${JSON.stringify(summary.totals)}\n`);
}

main().catch((error) => fail(error.stack || error.message));
