#!/usr/bin/env python3

"""
Create a test vector for encrypted key import testing
Uses the same PBKDF2 + AES-256-GCM approach as the C++ implementation
"""

import os
import base64
import hashlib
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives import hashes

def create_test_vector():
    # Test private key (32 bytes)
    private_key = bytes.fromhex("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
    
    # Encryption parameters
    passphrase = b"test_passphrase_123"
    salt = os.urandom(16)  # 16 bytes salt
    iv = os.urandom(12)    # 12 bytes IV for GCM
    iterations = 100000
    
    print("🔐 Creating encrypted key test vector...")
    print(f"   Private key: {private_key.hex()}")
    print(f"   Passphrase: {passphrase.decode()}")
    print(f"   Salt: {base64.b64encode(salt).decode()}")
    print(f"   IV: {base64.b64encode(iv).decode()}")
    print(f"   Iterations: {iterations}")
    
    # Derive key using PBKDF2-HMAC-SHA256
    kdf = PBKDF2HMAC(
        algorithm=hashes.SHA256(),
        length=32,
        salt=salt,
        iterations=iterations,
    )
    key = kdf.derive(passphrase)
    
    # Encrypt using AES-256-GCM
    aesgcm = AESGCM(key)
    ciphertext = aesgcm.encrypt(iv, private_key, None)
    
    # Split ciphertext and tag (GCM appends 16-byte tag)
    ct = ciphertext[:-16]
    tag = ciphertext[-16:]
    
    print(f"   Ciphertext: {base64.b64encode(ct).decode()}")
    print(f"   Tag: {base64.b64encode(tag).decode()}")
    
    # Create JSON for RPC call
    rpc_params = {
        "enc": "pbkdf2-hmac-sha256",
        "iter": iterations,
        "salt": base64.b64encode(salt).decode(),
        "cipher": "aes-256-gcm",
        "iv": base64.b64encode(iv).decode(),
        "ct": base64.b64encode(ct).decode(),
        "tag": base64.b64encode(tag).decode(),
        "passphrase": passphrase.decode(),
        "label": "Test Encrypted Key",
        "rescan": True
    }
    
    print("\n📋 RPC Parameters:")
    import json
    print(json.dumps(rpc_params, indent=2))
    
    # Verify decryption works
    print("\n🧪 Verification:")
    try:
        kdf_verify = PBKDF2HMAC(
            algorithm=hashes.SHA256(),
            length=32,
            salt=salt,
            iterations=iterations,
        )
        key_verify = kdf_verify.derive(passphrase)
        
        aesgcm_verify = AESGCM(key_verify)
        decrypted = aesgcm_verify.decrypt(iv, ct + tag, None)
        
        if decrypted == private_key:
            print("✅ Verification successful - decryption works!")
            return rpc_params
        else:
            print("❌ Verification failed - decryption mismatch")
            return None
    except Exception as e:
        print(f"❌ Verification failed: {e}")
        return None

if __name__ == "__main__":
    create_test_vector()
