// simple_ws_test.js
import fs from 'fs';
import WebSocket from 'ws';

const cookiePath = '/tmp/test-dir4/mainnet/.cookie';
const auth = fs.readFileSync(cookiePath, 'utf8').trim();
const basic = Buffer.from(auth).toString('base64');

console.log('🧪 Simple WebSocket Connection Test');
console.log('🔌 Connecting to WebSocket...');

const ws = new WebSocket('ws://127.0.0.1:20998/rpc.ws', {
  headers: { Authorization: `Basic ${basic}` }
});

ws.on('open', () => {
  console.log('✅ WebSocket connected successfully!');
  console.log('📡 Sending subscription...');
  
  ws.send(JSON.stringify({
    jsonrpc: "2.0", 
    id: 1, 
    method: "subscribe", 
    params: ["newBlocks", "miningInfo", "mempoolTx"]
  }));
  
  console.log('✅ Subscription sent, keeping connection open...');
});

ws.on('message', (data) => {
  console.log('📨 Received message:', data.toString());
});

ws.on('error', (error) => {
  console.log('❌ WebSocket error:', error.message);
});

ws.on('close', (code, reason) => {
  console.log(`🔌 WebSocket closed: code=${code}, reason=${reason}`);
});

// Keep the process alive
console.log('⏱️  Keeping connection open for 10 seconds...');
setTimeout(() => {
  console.log('🏁 Test complete, closing connection');
  ws.close();
  process.exit(0);
}, 10000);
