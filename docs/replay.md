# Replay engine

GD Menu's Replay panel uses input scheduling, frame fixes and practice rollback
adapted from [xdBot by Zilko and contributors](https://github.com/Zilko/xdBot),
using [slideglide/xdBot at 054764c2](https://github.com/slideglide/xdBot/commit/054764c2b0e3aa86a4d8a1c856abfbe533e79502),
with permission conveyed by the GD Menu maintainer.

This Windows port targets GD 2.2091 and Geode 6. The Replay panel saves
xdbot-replay.json, separate from the older position-based replay-v2.json.
It does not import GDR files or include the xdBot renderer, frame stepper,
TPS bypass, or the complete practice state restoration.

1. Enter a level and enable Practice Mode to use checkpoints.
2. Choose **Record from start**, close GD Menu, and resume.
3. Play through the level. A checkpoint respawn discards the failed suffix.
   Respawning from the start begins a new route.
4. Finish the level or click **Stop** to save. **Load** opens the last save.
5. Switch to Normal Mode on the same level. Choose **Play from start**,
   close GD Menu, and resume.

To verify the port, record a short level in Practice Mode. Place a checkpoint
before an obstacle, die after the checkpoint, then jump at a different point
on the retry. Stop, load, and play in Normal Mode to verify the new jump
plays once and the failed jump does not.