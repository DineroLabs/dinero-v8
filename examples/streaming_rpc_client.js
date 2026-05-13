#!/usr/bin/env node
/**
 * Dinero Streaming RPC Client (JavaScript/Node.js)
 *
 * Demonstrates streaming RPC operations with real-time progress updates
 * via WebSocket. Example: wallet rescan with live block progress.
 *
 * Installation:
 *   npm install ws
 *
 * Usage:
 *   node streaming_rpc_client.js
 */

const WebSocket = require('ws');

class DineroStreamingClient {
    constructor(host = 'ws://localhost:8998/ws') {
        this.host = host;
        this.ws = null;
        this.requestId = 1;
        this.callbacks = new Map();
        this.progressHandlers = new Map();
    }

    /**
     * Connect to WebSocket server
     */
    async connect() {
        return new Promise((resolve, reject) => {
            console.log(`🔌 Connecting to ${this.host}...`);

            this.ws = new WebSocket(this.host);

            this.ws.on('open', () => {
                console.log('✅ Connected to Dinero WebSocket server\n');
                this._setupMessageHandler();
                resolve();
            });

            this.ws.on('error', (error) => {
                console.error('❌ WebSocket error:', error.message);
                reject(error);
            });

            this.ws.on('close', () => {
                console.log('\n🔌 Disconnected from server');
            });
        });
    }

    /**
     * Setup message handler for incoming WebSocket events
     */
    _setupMessageHandler() {
        this.ws.on('message', (data) => {
            try {
                const message = JSON.parse(data.toString());

                // Handle progress events
                if (message.method === 'progress') {
                    this._handleProgress(message.params);
                }
                // Handle completion events
                else if (message.method === 'complete') {
                    this._handleComplete(message.params);
                }
                // Handle error events
                else if (message.method === 'error') {
                    this._handleError(message.params);
                }
                // Handle RPC responses
                else if (message.id !== undefined && message.result !== undefined) {
                    this._handleResponse(message);
                }
            } catch (error) {
                console.error('❌ Failed to parse message:', error.message);
            }
        });
    }

    /**
     * Handle progress event
     */
    _handleProgress(params) {
        const {
            operation_id,
            operation_type,
            current,
            total,
            percentage,
            message: statusMessage,
            extra
        } = params;

        const handler = this.progressHandlers.get(operation_id);
        if (handler) {
            handler({
                type: 'progress',
                current,
                total,
                percentage,
                message: statusMessage,
                extra
            });
        } else {
            // Default progress display
            console.log(`📊 [${operation_type}] ${percentage.toFixed(1)}% - ${statusMessage}`);
        }
    }

    /**
     * Handle completion event
     */
    _handleComplete(params) {
        const { operation_id, operation_type, result } = params;

        const handler = this.progressHandlers.get(operation_id);
        if (handler) {
            handler({
                type: 'complete',
                result
            });
            this.progressHandlers.delete(operation_id);
        }
    }

    /**
     * Handle error event
     */
    _handleError(params) {
        const { operation_id, operation_type, error, code } = params;

        const handler = this.progressHandlers.get(operation_id);
        if (handler) {
            handler({
                type: 'error',
                error,
                code
            });
            this.progressHandlers.delete(operation_id);
        } else {
            console.error(`❌ [${operation_type}] Error: ${error}`);
        }
    }

    /**
     * Handle RPC response
     */
    _handleResponse(message) {
        const callback = this.callbacks.get(message.id);
        if (callback) {
            if (message.error) {
                callback.reject(message.error);
            } else {
                callback.resolve(message.result);
            }
            this.callbacks.delete(message.id);
        }
    }

    /**
     * Call RPC method
     */
    async call(method, params = {}, progressHandler = null) {
        return new Promise((resolve, reject) => {
            const id = this.requestId++;
            const request = {
                jsonrpc: '2.0',
                id,
                method,
                params
            };

            // Store callback
            this.callbacks.set(id, { resolve, reject });

            // Send request
            this.ws.send(JSON.stringify(request));

            // Setup progress handler if provided
            if (progressHandler) {
                // Wait for operation_id in response
                const originalResolve = resolve;
                const enhancedResolve = (result) => {
                    if (result.operation_id) {
                        this.progressHandlers.set(result.operation_id, progressHandler);
                    }
                    originalResolve(result);
                };

                this.callbacks.set(id, { resolve: enhancedResolve, reject });
            }
        });
    }

    /**
     * Start wallet rescan with progress updates
     */
    async walletRescan(startHeight = 0, onProgress = null) {
        console.log(`🔍 Starting wallet rescan from height ${startHeight}...\n`);

        const result = await this.call('walletrescan', { start_height: startHeight }, (event) => {
            if (event.type === 'progress') {
                const { current, total, percentage, message, extra } = event;

                // Custom progress display
                const bar = this._progressBar(percentage);
                process.stdout.write(`\r${bar} ${percentage.toFixed(1)}% - ${message}`);

                // Call user callback
                if (onProgress) {
                    onProgress(event);
                }
            }
            else if (event.type === 'complete') {
                console.log('\n\n✅ Wallet rescan complete!');
                console.log('   Results:', JSON.stringify(event.result, null, 2));
            }
            else if (event.type === 'error') {
                console.error(`\n\n❌ Rescan failed: ${event.error}`);
            }
        });

        return result;
    }

    /**
     * Generate progress bar
     */
    _progressBar(percentage, width = 30) {
        const filled = Math.round(width * percentage / 100);
        const empty = width - filled;
        return `[${'='.repeat(filled)}${' '.repeat(empty)}]`;
    }

    /**
     * Close connection
     */
    close() {
        if (this.ws) {
            this.ws.close();
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Example Usage
// ═══════════════════════════════════════════════════════════════

async function main() {
    const client = new DineroStreamingClient('ws://localhost:8998/ws');

    try {
        // Connect to server
        await client.connect();

        // Example 1: Wallet rescan with default progress handler
        console.log('═'.repeat(60));
        console.log('Example 1: Wallet Rescan with Progress Updates');
        console.log('═'.repeat(60));
        console.log('');

        await client.walletRescan(0);

        // Wait a bit before closing
        await new Promise(resolve => setTimeout(resolve, 2000));

        // Close connection
        client.close();

    } catch (error) {
        console.error('❌ Error:', error.message);
        console.error('\nMake sure:');
        console.error('  1. dinerod is running');
        console.error('  2. WebSocket server is enabled');
        console.error('  3. Connection details are correct');
        process.exit(1);
    }
}

// Run if executed directly
if (require.main === module) {
    main().catch(console.error);
}

module.exports = DineroStreamingClient;
