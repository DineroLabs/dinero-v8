#!/usr/bin/env node
/**
 * DineroCoin WebSocket RPC Client Example (Node.js)
 * 
 * This example demonstrates how to connect to the DineroCoin daemon's WebSocket RPC
 * interface using Node.js and perform wallet and mining operations.
 */

const WebSocket = require('ws');
const fs = require('fs');
const path = require('path');
const os = require('os');

class DineroWebSocketClient {
    constructor(wsUrl, cookiePath) {
        this.wsUrl = wsUrl;
        this.cookiePath = cookiePath;
        this.authHeader = this.loadAuth();
    }
    
    loadAuth() {
        const cookie = fs.readFileSync(this.cookiePath, 'utf8').trim();
        return 'Basic ' + Buffer.from(cookie).toString('base64');
    }
    
    async callRpc(method, params = []) {
        return new Promise((resolve, reject) => {
            const request = {
                jsonrpc: '2.0',
                id: 1,
                method: method,
                params: params
            };
            
            const ws = new WebSocket(this.wsUrl, {
                headers: {
                    'Authorization': this.authHeader
                }
            });
            
            ws.on('open', () => {
                ws.send(JSON.stringify(request));
            });
            
            ws.on('message', (data) => {
                const response = JSON.parse(data);
                ws.close();
                
                if (response.error) {
                    reject(new Error(`RPC Error: ${JSON.stringify(response.error)}`));
                } else {
                    resolve(response.result);
                }
            });
            
            ws.on('error', (error) => {
                reject(error);
            });
        });
    }
}

async function main() {
    try {
        // Load connection info from nodeinfo.json
        const nodeinfoPath = path.join(os.homedir(), '.dinero', 'regtest', 'nodeinfo.json');
        const nodeinfo = JSON.parse(fs.readFileSync(nodeinfoPath, 'utf8'));
        
        const wsUrl = nodeinfo.ws.url;
        const cookiePath = nodeinfo.cookie;
        
        const client = new DineroWebSocketClient(wsUrl, cookiePath);
        
        console.log('🔗 Connected to DineroCoin WebSocket RPC');
        console.log(`   URL: ${wsUrl}`);
        
        // Get blockchain info
        const bestHash = await client.callRpc('getbestblockhash');
        console.log(`📦 Best block: ${bestHash}`);
        
        // Generate new address
        const address = await client.callRpc('wallet.getnewaddress');
        console.log(`🏠 New address: ${address}`);
        
        // Validate address
        const validation = await client.callRpc('wallet.validateaddress', [address]);
        console.log(`✅ Address valid: ${validation.isvalid}, mine: ${validation.ismine}`);
        
        // Set mining address
        await client.callRpc('mining.setaddress', [address]);
        console.log(`⛏️  Mining address set to: ${address}`);
        
        // Get mining address
        const miningInfo = await client.callRpc('mining.getaddress');
        console.log(`⛏️  Current mining: ${miningInfo.address} (mine: ${miningInfo.ismine})`);
        
        // Generate test blocks (regtest only)
        const blocks = await client.callRpc('mining.generatetoaddress', [3, address]);
        console.log(`🎯 Generated ${blocks.length} blocks`);
        
    } catch (error) {
        console.error(`❌ Error: ${error.message}`);
        process.exit(1);
    }
}

if (require.main === module) {
    main();
}
