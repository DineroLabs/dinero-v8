-- Auto-migration: Fix genesis chainwork from 0000...0000 to 0000...0001
-- Bitcoin-style: genesis block contributes work
-- Safe to run multiple times (idempotent)

BEGIN IMMEDIATE;

-- Only update if chainwork is currently all zeros (wrong value)
UPDATE meta 
SET value = '0000000000000000000000000000000000000000000000000000000000000001'
WHERE key = 'chainwork' 
  AND value = '0000000000000000000000000000000000000000000000000000000000000000';

-- Verify the fix worked
SELECT 
  CASE 
    WHEN value = '0000000000000000000000000000000000000000000000000000000000000001' 
    THEN '✅ Chainwork correctly set to genesis value (...0001)'
    ELSE '❌ Chainwork still incorrect: ' || value
  END as status
FROM meta 
WHERE key = 'chainwork';

COMMIT;
