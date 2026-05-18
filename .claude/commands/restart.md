# Restart Server

Stop the running AzerothCore processes and restart them in tmux sessions named "world" and "auth".

```bash
ssh -i ~/.ssh/geargambler_dev -o StrictHostKeyChecking=no azerothcore@192.168.50.140 "
  echo '=== Stopping existing server processes ===' &&
  pkill -f worldserver || true &&
  pkill -f authserver || true &&
  pkill -f 'acore.sh run' || true &&
  sleep 3 &&

  echo '=== Starting auth server in tmux ===' &&
  tmux new-session -d -s auth 2>/dev/null || tmux kill-session -t auth && tmux new-session -d -s auth &&
  tmux send-keys -t auth 'cd ~/azerothcore-wotlk/env/dist/bin && ./authserver' Enter &&

  echo '=== Starting world server in tmux ===' &&
  tmux new-session -d -s world 2>/dev/null; tmux kill-session -t world 2>/dev/null; tmux new-session -d -s world &&
  tmux send-keys -t world 'cd ~/azerothcore-wotlk/env/dist/bin && ./worldserver' Enter &&

  echo '=== Server restarted. Attach with: tmux attach -t world ===' &&
  sleep 5 &&
  tmux list-sessions
"
```
