import { invoke } from '@tauri-apps/api/core';
import { useState, useEffect, useCallback } from 'react';
import { useWallet } from './useWallet';

// Exchange rate interface
export interface ExchangeRate {
  from_symbol: string;
  to_symbol: string;
  rate: number;
  to_amount: number;
  min_amount: number;
  max_amount: number;
  timestamp: number;
  provider: string;
}

// Swap transaction interface
export interface SwapTransaction {
  txid: string;
  from_address: string;
  to_address: string;
  from_amount: number;
  to_amount: number;
  from_symbol: string;
  to_symbol: string;
  fee: number;
  status: 'pending' | 'processing' | 'completed' | 'failed';
  timestamp: number;
}

// Exchange hook for rate fetching and swaps
export function useExchange() {
  const { getNewAddress } = useWallet();
  const [rates, setRates] = useState<Map<string, ExchangeRate>>(new Map());
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const getRate = useCallback(async (
    from: string,
    to: string,
    amount: number
  ): Promise<ExchangeRate> => {
    try {
      setLoading(true);
      setError(null);
      
      const rate = await invoke<ExchangeRate>('get_exchange_rate', {
        from,
        to,
        amount,
      });
      
      // Cache rate
      const key = `${from}_${to}`;
      setRates(prev => new Map(prev.set(key, rate)));
      
      return rate;
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to get rate';
      setError(errorMsg);
      throw err;
    } finally {
      setLoading(false);
    }
  }, []);

  const createSwap = useCallback(async (
    toAddress: string,
    amount: number,
    fromSymbol: string,
    toSymbol: string
  ): Promise<SwapTransaction> => {
    try {
      setLoading(true);
      setError(null);
      
      // Get source address
      const fromAddress = await getNewAddress();
      
      const swap = await invoke<SwapTransaction>('create_swap_tx', {
        fromAddress,
        toAddress,
        amount,
        fromSymbol,
        toSymbol,
      });
      
      return swap;
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to create swap';
      setError(errorMsg);
      throw err;
    } finally {
      setLoading(false);
    }
  }, [getNewAddress]);

  const getSwapStatus = useCallback(async (
    swapId: string
  ): Promise<SwapTransaction> => {
    try {
      setLoading(true);
      setError(null);
      
      const swap = await invoke<SwapTransaction>('get_swap_status', {
        swapId,
      });
      
      return swap;
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to get swap status';
      setError(errorMsg);
      throw err;
    } finally {
      setLoading(false);
    }
  }, []);

  return {
    rates,
    loading,
    error,
    getRate,
    createSwap,
    getSwapStatus,
  };
}

// Auto-refresh exchange rates hook
export function useExchangeRates(
  pairs: Array<{ from: string; to: string; amount: number }>,
  refreshInterval: number = 60000 // 1 minute default
) {
  const { getRate } = useExchange();
  const [rates, setRates] = useState<Map<string, ExchangeRate>>(new Map());

  const refreshRates = useCallback(async () => {
    const newRates = new Map<string, ExchangeRate>();
    
    for (const pair of pairs) {
      try {
        const rate = await getRate(pair.from, pair.to, pair.amount);
        const key = `${pair.from}_${pair.to}`;
        newRates.set(key, rate);
      } catch (err) {
        console.error(`Failed to fetch rate for ${pair.from}/${pair.to}:`, err);
      }
    }
    
    setRates(newRates);
  }, [pairs, getRate]);

  useEffect(() => {
    // Initial fetch
    refreshRates();

    // Auto-refresh
    const interval = setInterval(refreshRates, refreshInterval);

    return () => clearInterval(interval);
  }, [refreshRates, refreshInterval]);

  return rates;
}

// Swap status monitoring hook
export function useSwapStatus(swapId: string | null) {
  const { getSwapStatus } = useExchange();
  const [swap, setSwap] = useState<SwapTransaction | null>(null);
  const [loading, setLoading] = useState(false);

  const checkStatus = useCallback(async () => {
    if (!swapId) return;

    try {
      setLoading(true);
      const status = await getSwapStatus(swapId);
      setSwap(status);
    } catch (err) {
      console.error('Failed to check swap status:', err);
    } finally {
      setLoading(false);
    }
  }, [swapId, getSwapStatus]);

  useEffect(() => {
    if (!swapId) return;

    // Check immediately
    checkStatus();

    // Then check every 30 seconds
    const interval = setInterval(checkStatus, 30000);

    return () => clearInterval(interval);
  }, [swapId, checkStatus]);

  return { swap, loading, refresh: checkStatus };
}

