// e2e_ws_subscriber.js
import fs from 'fs';
import { execSync } from 'child_process';
import WebSocket from 'ws';

const cookiePath = process.env.COOKIE || '/tmp/test-dir4/mainnet/.cookie';
const auth = fs.readFileSync(cookiePath, 'utf8').trim();
const basic = Buffer.from(auth).toString('base64');
const rpc = process.env.RPC || 'http://127.0.0.1:20998';

let counts = { newBlocks: 0, miningInfo: 0, mempoolTx: 0 };
const target = { newBlocks: 2, miningInfo: 2, mempoolTx: 1 };
const deadlineMs = Date.now() + 20000; // 20s timeout

function done(ok, why) {
  console.log(JSON.stringify({ ok, why, counts }, null, 2));
  process.exit(ok ? 0 : 1);
}

const ws = new WebSocket('ws://127.0.0.1:20998/rpc.ws', {
  headers: { Authorization: `Basic ${basic}` }
});

ws.on('open', () => {
  console.log('🔌 WebSocket connected, subscribing to channels...');
  ws.send(JSON.stringify({jsonrpc:"2.0", id:1, method:"subscribe", params:["newBlocks","miningInfo","mempoolTx"]}));
  
  // Wait longer for subscription to be processed
  setTimeout(() => {
    console.log('🚀 Triggering real chain activity...');
    try {
      // Generate 2 blocks to trigger newBlocks events
      execSync(`curl -s --user "${auth}" -H 'Content-Type: application/json' --data '{"jsonrpc":"2.0","id":11,"method":"setgenerate","params":[true,2]}' ${rpc} >/dev/null`);
      console.log('✅ Mining triggered (2 blocks)');
      
      // Submit dummy transaction to trigger mempoolTx event
      execSync(`curl -s --user "${auth}" -H 'Content-Type: application/json' --data '{"jsonrpc":"2.0","id":12,"method":"submitdummytx","params":[]}' ${rpc} >/dev/null`);
      console.log('✅ Dummy transaction submitted');
    } catch (e) {
      console.log('⚠️ Some RPC calls failed, but continuing to listen for events...');
      // keep listening; even if one fails we may still get events
    }
  }, 1000); // Increased delay to 1 second
});

ws.on('message', (buf) => {
  try {
    const msg = JSON.parse(buf.toString());
    if (msg.method === 'subscription' && msg.params && msg.params.channel) {
      const ch = msg.params.channel;
      if (counts[ch] !== undefined) {
        counts[ch]++;
        console.log(`📨 Event #${counts[ch]} - Channel: ${ch}`);
        // console.log('EVT', ch, msg.params.result); // uncomment for verbose
      }
      
      const satisfied =
        counts.newBlocks >= target.newBlocks &&
        counts.miningInfo >= target.miningInfo &&
        counts.mempoolTx >= target.mempoolTx;

      if (satisfied) {
        console.log('🎯 All target events received!');
        done(true, 'received target events');
      }
    }
  } catch (e) {
    // Ignore parsing errors
  }
});

ws.on('error', (e) => done(false, `ws error: ${e.message}`));
ws.on('close', (code, reason) => {
  console.log(`🔌 WebSocket closed: code=${code}, reason=${reason}`);
});

const t = setInterval(() => {
  if (Date.now() > deadlineMs) {
    clearInterval(t);
    console.log('⏰ Timeout waiting for events');
    done(false, 'timeout waiting for events');
  }
}, 200);

console.log('🧪 E2E WebSocket Test Starting...');
console.log(`🎯 Targets: newBlocks=${target.newBlocks}, miningInfo=${target.miningInfo}, mempoolTx=${target.mempoolTx}`);
console.log(`⏱️  Timeout: 20 seconds`);
