#pragma once

// Payment RPC Method Registration
// Registers payment monitoring RPC methods in global RpcRegistry
// Methods: payment.watch, payment.unwatch, payment.status, payment.analyze
void registerPaymentRPC();
