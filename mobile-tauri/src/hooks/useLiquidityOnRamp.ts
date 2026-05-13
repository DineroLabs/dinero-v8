import { invoke } from '@tauri-apps/api/core';
import { useState, useEffect, useCallback } from 'react';

// Liquidity pool interface
export interface LiquidityPool {
  pool_id: string;
  symbol: string;
  total_liquidity: number;
  available_liquidity: number;
  apy: number;
  min_deposit: number;
  max_deposit: number;
  last_update: number;
}

// Fiat order interface
export interface FiatOrder {
  order_id: string;
  payment_method: string;
  fiat_amount: number;
  fiat_currency: string;
  crypto_amount: number;
  crypto_symbol: string;
  exchange_rate: number;
  fee: number;
  status: 'pending' | 'processing' | 'completed' | 'failed';
  payment_url: string;
  expires_at: number;
  created_at: number;
}

// KYC status interface
export interface KYCStatus {
  is_verified: boolean;
  verification_level: 'none' | 'basic' | 'advanced' | 'institutional';
  provider: string;
  verified_at: number;
  expires_at: number;
  country: string;
}

// Liquidity management hook
export function useLiquidity() {
  const [pools, setPools] = useState<LiquidityPool[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const fetchPools = useCallback(async () => {
    try {
      setLoading(true);
      setError(null);
      
      const poolsData = await invoke<LiquidityPool[]>('get_liquidity_pools');
      setPools(poolsData);
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to fetch pools';
      setError(errorMsg);
    } finally {
      setLoading(false);
    }
  }, []);

  const addLiquidity = useCallback(async (
    poolId: string,
    amount: number
  ): Promise<string> => {
    try {
      setLoading(true);
      setError(null);
      
      const txid = await invoke<string>('add_liquidity', {
        poolId,
        amount,
      });
      
      // Refresh pools after adding liquidity
      await fetchPools();
      
      return txid;
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to add liquidity';
      setError(errorMsg);
      throw err;
    } finally {
      setLoading(false);
    }
  }, [fetchPools]);

  const removeLiquidity = useCallback(async (
    poolId: string,
    amount: number
  ): Promise<string> => {
    try {
      setLoading(true);
      setError(null);
      
      const txid = await invoke<string>('remove_liquidity', {
        poolId,
        amount,
      });
      
      // Refresh pools after removing liquidity
      await fetchPools();
      
      return txid;
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to remove liquidity';
      setError(errorMsg);
      throw err;
    } finally {
      setLoading(false);
    }
  }, [fetchPools]);

  useEffect(() => {
    fetchPools();
  }, [fetchPools]);

  return {
    pools,
    loading,
    error,
    refresh: fetchPools,
    addLiquidity,
    removeLiquidity,
  };
}

// Fiat on-ramp hook
export function useFiatOnRamp() {
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const createOrder = useCallback(async (
    amount: number,
    fiatCurrency: string,
    cryptoSymbol: string,
    paymentMethod: string
  ): Promise<FiatOrder> => {
    try {
      setLoading(true);
      setError(null);
      
      const order = await invoke<FiatOrder>('create_fiat_order', {
        amount,
        fiatCurrency,
        cryptoSymbol,
        paymentMethod,
      });
      
      return order;
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to create order';
      setError(errorMsg);
      throw err;
    } finally {
      setLoading(false);
    }
  }, []);

  const getOrderStatus = useCallback(async (
    orderId: string
  ): Promise<FiatOrder> => {
    try {
      setLoading(true);
      setError(null);
      
      const order = await invoke<FiatOrder>('get_fiat_order_status', {
        orderId,
      });
      
      return order;
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to get order status';
      setError(errorMsg);
      throw err;
    } finally {
      setLoading(false);
    }
  }, []);

  return {
    loading,
    error,
    createOrder,
    getOrderStatus,
  };
}

// KYC verification hook
export function useKYC() {
  const [kycStatus, setKycStatus] = useState<KYCStatus | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const fetchStatus = useCallback(async () => {
    try {
      setLoading(true);
      setError(null);
      
      const status = await invoke<KYCStatus>('get_kyc_status');
      setKycStatus(status);
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to get KYC status';
      setError(errorMsg);
    } finally {
      setLoading(false);
    }
  }, []);

  const startVerification = useCallback(async (
    level: 'basic' | 'advanced',
    country: string
  ): Promise<string> => {
    try {
      setLoading(true);
      setError(null);
      
      const url = await invoke<string>('start_kyc_verification', {
        level,
        country,
      });
      
      return url;
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : 'Failed to start verification';
      setError(errorMsg);
      throw err;
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    fetchStatus();
  }, [fetchStatus]);

  return {
    kycStatus,
    loading,
    error,
    refresh: fetchStatus,
    startVerification,
  };
}

// Fiat order status monitoring hook
export function useFiatOrderStatus(orderId: string | null) {
  const { getOrderStatus } = useFiatOnRamp();
  const [order, setOrder] = useState<FiatOrder | null>(null);
  const [loading, setLoading] = useState(false);

  const checkStatus = useCallback(async () => {
    if (!orderId) return;

    try {
      setLoading(true);
      const status = await getOrderStatus(orderId);
      setOrder(status);
    } catch (err) {
      console.error('Failed to check order status:', err);
    } finally {
      setLoading(false);
    }
  }, [orderId, getOrderStatus]);

  useEffect(() => {
    if (!orderId) return;

    // Check immediately
    checkStatus();

    // Then check every 30 seconds
    const interval = setInterval(checkStatus, 30000);

    return () => clearInterval(interval);
  }, [orderId, checkStatus]);

  return { order, loading, refresh: checkStatus };
}

