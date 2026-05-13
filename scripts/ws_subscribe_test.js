#!/usr/bin/env node

// WebSocket subscription test script
// Tests the full WebSocket RPC implementation with real blockchain events

import WebSocket from 'ws';
import fs from 'fs';
import child_process from 'child_process';

const COOKIE_PATH = '/tmp/test-dir4/mainnet/.cookie';
const WS_URL = 'ws://127.0.0.1:20998/rpc.ws';
const HTTP_URL = 'http://127.0.0.1:20998';

// Check if cookie exists
if (!fs.existsSync(COOKIE_PATH)) {
    console.error(`❌ Cookie file not found at ${COOKIE_PATH}`);
    process.exit(1);
}

const auth = fs.readFileSync(COOKIE_PATH, 'utf8').trim();
const basic = Buffer.from(auth).toString('base64');

console.log('=== WebSocket Subscription Test ===');
console.log(`✅ Cookie loaded: ${auth.substring(0, 20)}...`);
console.log(`✅ Basic auth: ${basic.substring(0, 20)}...`);

// Create WebSocket connection
const ws = new WebSocket(WS_URL, { 
    headers: { 
        Authorization: `Basic ${basic}` 
    } 
});

function rpc(id, method, params = []) {
    const message = {
        jsonrpc: "2.0",
        id,
        method,
        params
    };
    console.log(`>> [${id}] ${method}(${JSON.stringify(params)})`);
    ws.send(JSON.stringify(message));
}

// WebSocket event handlers
ws.on('open', () => {
    console.log('✅ WebSocket connected');
    
    // Test 1: Get current block count
    rpc(1, "getblockcount");
    
    // Test 2: Subscribe to blockchain events
    rpc(2, "subscribe", ["newBlocks", "mempoolTx", "miningInfo"]);
    
    // Test 3: Trigger a block (mine a new block)
    console.log('\n🚀 Triggering block generation...');
    try {
        const result = child_process.execSync(
            `curl -s --user "${auth}" -H 'Content-Type: application/json' ` +
            `--data '{"jsonrpc":"2.0","id":9,"method":"setgenerate","params":[true,1]}' ` +
            HTTP_URL
        );
        console.log(`✅ Block generation triggered: ${result.toString().trim()}`);
    } catch (error) {
        console.error(`❌ Failed to trigger block generation: ${error.message}`);
    }
    
    // Test 4: Get mining info
    rpc(4, "getmininginfo");
    
    // Test 5: Unsubscribe from one channel
    setTimeout(() => {
        rpc(5, "unsubscribe", ["miningInfo"]);
    }, 2000);
});

ws.on('message', (data) => {
    try {
        const msg = JSON.parse(data.toString());
        console.log(`<< [${msg.id || 'event'}] ${msg.method || 'response'}: ${JSON.stringify(msg)}`);
        
        // Handle subscription events
        if (msg.method === 'subscription') {
            console.log(`📡 SUBSCRIPTION EVENT: ${msg.params.channel}`);
            console.log(`   SubId: ${msg.params.subId}`);
            console.log(`   Data: ${JSON.stringify(msg.params.result, null, 2)}`);
        }
    } catch (error) {
        console.log(`<< Raw message: ${data.toString()}`);
    }
});

ws.on('error', (error) => {
    console.error('❌ WebSocket error:', error.message);
});

ws.on('close', (code, reason) => {
    console.log(`🔌 WebSocket closed: ${code} - ${reason}`);
});

// Handle process termination
process.on('SIGINT', () => {
    console.log('\n🛑 Shutting down...');
    ws.close();
    process.exit(0);
});

// Keep alive for 30 seconds to see events
setTimeout(() => {
    console.log('\n⏰ Test completed, closing connection...');
    ws.close();
    process.exit(0);
}, 30000);

console.log('\n🎯 Test running for 30 seconds...');
console.log('   Press Ctrl+C to stop early');
console.log('   Watch for subscription events from block generation');
