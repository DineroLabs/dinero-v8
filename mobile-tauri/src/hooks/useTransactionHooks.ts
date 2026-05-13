import { invoke } from '@tauri-apps/api/core';
import { sendNotification } from '@tauri-apps/api/notification';
import { useState, useEffect, useCallback } from 'react';
import { useWallet } from './useWallet';

// Transaction confirmation monitoring hook
export function useTransactionConfirmations(txid: string | null) {
  const [confirmations, setConfirmations] = useState<number>(0);
  const [loading, setLoading] = useState(false);

  const checkConfirmations = useCallback(async () => {
    if (!txid) return;
    
    try {
      setLoading(true);
      const count = await invoke<number>('get_tx_confirmations', { txid });
      setConfirmations(count);
    } catch (error) {
      console.error('Failed to get confirmations:', error);
    } finally {
      setLoading(false);
    }
  }, [txid]);

  useEffect(() => {
    if (!txid) return;

    // Check immediately
    checkConfirmations();

    // Then check every 30 seconds
    const interval = setInterval(checkConfirmations, 30000);

    return () => clearInterval(interval);
  }, [txid, checkConfirmations]);

  return { confirmations, loading, refresh: checkConfirmations };
}

// Push notification hook for incoming funds
export function useTransactionNotifications(enabled: boolean = true) {
  const { checkNewTransactions } = useWallet();

  useEffect(() => {
    if (!enabled) return;

    const checkAndNotify = async () => {
      try {
        const notifications = await checkNewTransactions();
        
        for (const notif of notifications) {
          if (notif.is_new) {
            if (notif.category === 'receive') {
              await sendNotification({
                title: '💰 Dinero Received!',
                body: `You received ${notif.amount.toFixed(8)} DIN`,
              });
            } else if (notif.category === 'generate') {
              await sendNotification({
                title: '⛏️ Mining Reward!',
                body: `You mined ${notif.amount.toFixed(8)} DIN`,
              });
            }
          }
        }
      } catch (error) {
        console.error('Notification check failed:', error);
      }
    };

    // Check immediately
    checkAndNotify();

    // Then check every 30 seconds
    const interval = setInterval(checkAndNotify, 30000);

    return () => clearInterval(interval);
  }, [enabled, checkNewTransactions]);
}

// QR Request Payment modal hook
export function useQRRequestModal() {
  const { generatePaymentURI } = useWallet();
  const [isOpen, setIsOpen] = useState(false);
  const [address, setAddress] = useState('');
  const [amount, setAmount] = useState<number | undefined>();
  const [label, setLabel] = useState('');
  const [qrUri, setQrUri] = useState('');

  const openModal = useCallback((addr: string) => {
    setAddress(addr);
    setIsOpen(true);
  }, []);

  const closeModal = useCallback(() => {
    setIsOpen(false);
    setAmount(undefined);
    setLabel('');
    setQrUri('');
  }, []);

  const generateQR = useCallback(async () => {
    try {
      const uri = await generatePaymentURI(address, amount, label || undefined);
      setQrUri(uri);
    } catch (error) {
      console.error('Failed to generate QR:', error);
    }
  }, [address, amount, label, generatePaymentURI]);

  return {
    isOpen,
    address,
    amount,
    label,
    qrUri,
    setAmount,
    setLabel,
    openModal,
    closeModal,
    generateQR,
  };
}

