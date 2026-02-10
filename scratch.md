  There are some cleanup items:
  - Remove the temporary [KEY] and [ADB] KBD Talk R0 debug logging
  - Optionally revert handler ID (1→2) and bit timing (70/30→65/35) since those weren't the fix
  - Audit other high keycodes (F-keys, navigation keys) that might also be virtual keycodes instead of scan codes
