#!/usr/bin/env bash
set -euo pipefail

cmd="$(jq -r '.tool_input.command // ""')"

if [[ "$cmd" == docker\ run\ * ]]; then
  jq -n --arg reason "Auto-allowed docker run in this project." '{
    hookSpecificOutput: {
      hookEventName: "PreToolUse",
      permissionDecision: "allow",
      permissionDecisionReason: $reason
    }
  }'
else
  # No decision -> fall back to normal permission system
  jq -n '{}'
fi
