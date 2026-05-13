import { invoke } from '@tauri-apps/api/core';
import { useState, useEffect } from 'react';

// Wallet state management
interface WalletState {
  balance: {
    total: number;
    confirmed: number;
    unconfirmed: number;
    immature: number;
  } | null;
  isEncrypted: boolean;
  isLocked: boolean;
  addresses: string[];
  loading: boolean;
  error: string | null;
}

export function useWallet() {
  const [state, setState] = useState<WalletState>({
    balance: null,
    isEncrypted: false,
    isLocked: true,
    addresses: [],
    loading: false,
    error: null,
  });

  const refreshBalance = async () => {
    try {
      setState(prev => ({ ...prev, loading: true, error: null }));
      const balance = await invoke<{
        total: number;
        confirmed: number;
        unconfirmed: number;
        immature: number;
      }>('get_balance');
      setState(prev => ({ ...prev, balance, loading: false }));
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Unknown error',
        loading: false,
      }));
    }
  };

  const createWallet = async () => {
    try {
      const mnemonic = await invoke<string>('create_wallet');
      await refreshBalance();
      return mnemonic;
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Unknown error',
      }));
      throw error;
    }
  };

  const unlockWallet = async (password: string, timeoutSeconds: number = 900) => {
    try {
      await invoke('unlock_wallet', { password, timeoutSeconds });
      setState(prev => ({ ...prev, isLocked: false }));
      await refreshBalance();
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Unknown error',
      }));
      throw error;
    }
  };

  const lockWallet = async () => {
    try {
      await invoke('lock_wallet');
      setState(prev => ({ ...prev, isLocked: true }));
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Unknown error',
      }));
    }
  };

  const sendTransaction = async (
    to: string,
    amount: number,
    feeRate: number = 1.0,
    note?: string
  ) => {
    try {
      const txid = await invoke<string>('send_transaction', {
        to,
        amount,
        feeRate,
        note,
      });
      await refreshBalance();
      return txid;
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Unknown error',
      }));
      throw error;
    }
  };

  const exportHistory = async (format: 'csv' | 'json', dest: string) => {
    try {
      const result = await invoke<string>('export_history', { format, dest });
      return result;
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Unknown error',
      }));
      throw error;
    }
  };

  const parsePaymentURI = async (uri: string) => {
    try {
      const parsed = await invoke<{
        address: string;
        amount: number;
        label: string;
      }>('parse_payment_uri', { uri });
      
      return parsed;
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Invalid payment URI',
      }));
      throw error;
    }
  };

  const generatePaymentURI = async (
    address: string,
    amount?: number,
    label?: string
  ) => {
    try {
      const uri = await invoke<string>('generate_payment_uri', {
        address,
        amount,
        label,
      });
      return uri;
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Failed to generate URI',
      }));
      throw error;
    }
  };

  const checkNewTransactions = async () => {
    try {
      const notifications = await invoke<
        Array<{
          txid: string;
          address: string;
          amount: number;
          confirmations: number;
          timestamp: number;
          category: string;
          is_new: boolean;
        }>
      >('check_new_transactions');
      return notifications;
    } catch (error) {
      setState(prev => ({
        ...prev,
        error: error instanceof Error ? error.message : 'Failed to check transactions',
      }));
      throw error;
    }
  };

  useEffect(() => {
    // Check wallet status on mount
    invoke<boolean>('is_wallet_encrypted').then(isEncrypted => {
      setState(prev => ({ ...prev, isEncrypted }));
    });
    invoke<boolean>('is_wallet_locked').then(isLocked => {
      setState(prev => ({ ...prev, isLocked }));
    });
  }, []);

  return {
    ...state,
    refreshBalance,
    createWallet,
    unlockWallet,
    lockWallet,
    sendTransaction,
    exportHistory,
    parsePaymentURI,
    generatePaymentURI,
    checkNewTransactions,
  };
}

