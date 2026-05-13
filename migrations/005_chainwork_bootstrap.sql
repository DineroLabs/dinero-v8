-- Idempotent fix for genesis chainwork on height 0 databases
-- Safe to run multiple times; only corrects the specific old value.
BEGIN IMMEDIATE;
UPDATE meta
   SET value='0000000000000000000000000000000000000000000000000000000000000001'
 WHERE key='chainwork'
   AND value='0000000000000000000000000000000000000000000000000000000000000000'
   AND (SELECT value FROM meta WHERE key='height') = '0';
COMMIT;
