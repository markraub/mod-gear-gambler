# Build and Deploy

Rebuild the AzerothCore worldserver on the dev server after pulling the latest module changes.

Steps:
1. SSH to 192.168.50.140 as azerothcore
2. Pull latest changes into the module directory
3. Run make and make install
4. Report build result

```bash
ssh -i ~/.ssh/geargambler_dev -o StrictHostKeyChecking=no azerothcore@192.168.50.140 "
  echo '=== Pulling latest module code ===' &&
  cd ~/azerothcore-wotlk/modules/mod-gear-gambler &&
  git pull origin main &&
  echo '=== Building ===' &&
  cd ~/azerothcore-wotlk/build &&
  make -j\$(nproc) 2>&1 | tail -20 &&
  echo '=== Installing ===' &&
  make install 2>&1 | tail -5 &&
  echo '=== Copying binaries to run directory ===' &&
  cp ~/azerothcore-wotlk/build/env/dist/bin/worldserver ~/azerothcore-wotlk/env/dist/bin/worldserver &&
  cp ~/azerothcore-wotlk/build/env/dist/bin/authserver  ~/azerothcore-wotlk/env/dist/bin/authserver &&
  echo '=== Build complete ==='
"
```

Note: make install puts binaries in build/env/dist/bin/ — the copy step above syncs them
to env/dist/bin/ where the server actually runs (alongside maps/dbc/vmaps).
After building, use /restart to bring the new binary online.
