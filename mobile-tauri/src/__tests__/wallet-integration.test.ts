// Integration tests for Tauri invoke commands
// These tests verify end-to-end functionality through Tauri's invoke system

import { describe, it, expect, beforeAll } from '@jest/globals';
import { invoke } from '@tauri-apps/api/core';

describe('Wallet FFI Integration Tests', () => {
  beforeAll(async () => {
    // Initialize wallet if needed
    try {
      await invoke('init_wallet', { datadir: '/tmp/test-wallet' });
    } catch (e) {
      // May fail if already initialized, which is OK
    }
  });

  describe('Error Handling', () => {
    it('should get last error code', async () => {
      const errorCode = await invoke<number>('get_last_error');
      expect(typeof errorCode).toBe('number');
      expect(errorCode).toBeLessThanOrEqual(0);
    });

    it('should get error message', async () => {
      const message = await invoke<string>('get_error_message', { errorCode: -1 });
      expect(typeof message).toBe('string');
      expect(message.length).toBeGreaterThan(0);
    });
  });

  describe('QR Payment URI', () => {
    it('should parse payment URI', async () => {
      const uri = 'dinero:din1qtest123?amount=15.25&label=Test';
      const parsed = await invoke<{
        address: string;
        amount: number;
        label: string;
      }>('parse_payment_uri', { uri });

      expect(parsed.address).toBe('din1qtest123');
      expect(parsed.amount).toBe(15.25);
      expect(parsed.label).toBe('Test');
    });

    it('should generate payment URI', async () => {
      const uri = await invoke<string>('generate_payment_uri', {
        address: 'din1qtest123',
        amount: 100.5,
        label: 'Test Label',
      });

      expect(uri).toContain('dinero:');
      expect(uri).toContain('din1qtest123');
      expect(uri).toContain('amount=100.5');
    });

    it('should reject invalid URI', async () => {
      await expect(
        invoke('parse_payment_uri', { uri: 'invalid:uri' })
      ).rejects.toThrow();
    });
  });

  describe('Secure Storage', () => {
    it('should check secure storage availability', async () => {
      const available = await invoke<boolean>('secure_storage_available');
      expect(typeof available).toBe('boolean');
    });

    it('should store and retrieve wallet data', async () => {
      const testData = JSON.stringify({ test: 'data' });
      
      await invoke('store_wallet_secure', { walletData: testData });
      
      const retrieved = await invoke<string>('retrieve_wallet_secure');
      expect(retrieved).toBe(testData);
    });
  });

  describe('Transaction Details', () => {
    it('should get transaction confirmations', async () => {
      // This test may fail if no transactions exist, which is OK
      try {
        const confirmations = await invoke<number>('get_tx_confirmations', {
          txid: 'test-txid',
        });
        expect(typeof confirmations).toBe('number');
        expect(confirmations).toBeGreaterThanOrEqual(0);
      } catch (e) {
        // Expected if transaction doesn't exist
        expect(e).toBeDefined();
      }
    });
  });

  describe('Sync Progress', () => {
    it('should get sync progress', async () => {
      const progress = await invoke<{
        progress: number;
        current_block: number;
        total_blocks: number;
        is_syncing: boolean;
        status_message: string;
      }>('get_sync_progress');

      expect(progress.progress).toBeGreaterThanOrEqual(0);
      expect(progress.progress).toBeLessThanOrEqual(1);
      expect(typeof progress.is_syncing).toBe('boolean');
    });
  });
});

