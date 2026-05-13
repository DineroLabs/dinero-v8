// External Exchange API Client
// mobile-tauri/src/api/exchange.ts

import { invoke } from '@tauri-apps/api/core';

// Exchange provider configuration
export interface ExchangeProvider {
  name: string;
  apiUrl: string;
  apiKey?: string;
  supportedPairs: string[];
}

// CoinGecko API client (free, no API key required)
export class CoinGeckoClient {
  private baseUrl = 'https://api.coingecko.com/api/v3';

  async getRate(from: string, to: string, amount: number): Promise<{
    rate: number;
    to_amount: number;
    timestamp: number;
  }> {
    try {
      // Map Dinero symbol to CoinGecko ID (if listed)
      const fromId = this.mapSymbolToId(from);
      const toId = this.mapSymbolToId(to);

      const url = `${this.baseUrl}/simple/price?ids=${fromId}&vs_currencies=${toId}`;
      
      const response = await fetch(url);
      const data = await response.json();

      if (!data[fromId] || !data[fromId][toId]) {
        throw new Error(`Rate not found for ${from}/${to}`);
      }

      const rate = data[fromId][toId];
      const to_amount = amount * rate;

      return {
        rate,
        to_amount,
        timestamp: Date.now(),
      };
    } catch (error) {
      console.error('CoinGecko API error:', error);
      throw error;
    }
  }

  private mapSymbolToId(symbol: string): string {
    // Map common symbols to CoinGecko IDs
    const mapping: Record<string, string> = {
      'BTC': 'bitcoin',
      'ETH': 'ethereum',
      'USDT': 'tether',
      'USD': 'usd',
      'DIN': 'dinero', // TODO: Add when Dinero is listed
    };
    return mapping[symbol.toUpperCase()] || symbol.toLowerCase();
  }
}

// SimpleSwap API client (requires API key)
export class SimpleSwapClient {
  private baseUrl = 'https://api.simpleswap.io/v1';
  private apiKey: string;

  constructor(apiKey: string) {
    this.apiKey = apiKey;
  }

  async getRate(from: string, to: string, amount: number): Promise<{
    rate: number;
    to_amount: number;
    min_amount: number;
    max_amount: number;
    timestamp: number;
  }> {
    try {
      const url = `${this.baseUrl}/get_rate?api_key=${this.apiKey}&fixed=true&currency_from=${from}&currency_to=${to}&amount=${amount}`;
      
      const response = await fetch(url);
      const data = await response.json();

      if (!data.rate) {
        throw new Error(`Rate not found for ${from}/${to}`);
      }

      return {
        rate: parseFloat(data.rate),
        to_amount: parseFloat(data.amount_to),
        min_amount: parseFloat(data.min_amount),
        max_amount: parseFloat(data.max_amount),
        timestamp: Date.now(),
      };
    } catch (error) {
      console.error('SimpleSwap API error:', error);
      throw error;
    }
  }

  async createSwap(
    from: string,
    to: string,
    amount: number,
    toAddress: string
  ): Promise<{
    swap_id: string;
    deposit_address: string;
    deposit_amount: number;
  }> {
    try {
      const url = `${this.baseUrl}/create_exchange`;
      
      const response = await fetch(url, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          api_key: this.apiKey,
          fixed: true,
          currency_from: from,
          currency_to: to,
          amount: amount,
          address_to: toAddress,
        }),
      });

      const data = await response.json();

      if (!data.swap_id) {
        throw new Error('Failed to create swap');
      }

      return {
        swap_id: data.swap_id,
        deposit_address: data.address,
        deposit_amount: parseFloat(data.amount),
      };
    } catch (error) {
      console.error('SimpleSwap create swap error:', error);
      throw error;
    }
  }

  async getSwapStatus(swapId: string): Promise<{
    status: string;
    amount_to: number;
    txid: string;
  }> {
    try {
      const url = `${this.baseUrl}/get_exchange?api_key=${this.apiKey}&swap_id=${swapId}`;
      
      const response = await fetch(url);
      const data = await response.json();

      return {
        status: data.status,
        amount_to: parseFloat(data.amount_to),
        txid: data.txid_to || '',
      };
    } catch (error) {
      console.error('SimpleSwap status error:', error);
      throw error;
    }
  }
}

// Exchange provider factory
export function createExchangeProvider(provider: string, apiKey?: string): CoinGeckoClient | SimpleSwapClient {
  switch (provider.toLowerCase()) {
    case 'coingecko':
      return new CoinGeckoClient();
    case 'simpleswap':
      if (!apiKey) {
        throw new Error('SimpleSwap requires API key');
      }
      return new SimpleSwapClient(apiKey);
    default:
      throw new Error(`Unknown provider: ${provider}`);
  }
}

// Use Tauri command (falls back to FFI if API unavailable)
export async function getExchangeRateWithFallback(
  from: string,
  to: string,
  amount: number,
  provider: string = 'coingecko'
): Promise<any> {
  try {
    // Try external API first
    const apiClient = createExchangeProvider(provider);
    return await apiClient.getRate(from, to, amount);
  } catch (error) {
    console.warn('External API failed, using FFI fallback:', error);
    // Fallback to FFI implementation
    return await invoke('get_exchange_rate', { from, to, amount });
  }
}

