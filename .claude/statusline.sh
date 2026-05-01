#!/usr/bin/env bash
# TD5RE Claude Code status line
# Renders: <git-branch> · <first-user-message-summary> · <token-count>
# Receives JSON on stdin from Claude Code.

input=$(cat)

# --- 1. Git branch ---
# Prefer CLAUDE_PROJECT_DIR env; fall back to the script's own directory (project root).
proj_dir="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
branch=$(git -C "$proj_dir" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
# Truncate to 30 chars
if [ ${#branch} -gt 30 ]; then
    branch="${branch:0:29}…"
fi

# --- 2. Initial inquiry summary ---
transcript_path=$(echo "$input" | jq -r '.transcript_path // empty')
summary="(new session)"
if [ -n "$transcript_path" ] && [ -f "$transcript_path" ]; then
    # Find the first message where type=="user" AND message.role=="user"
    # Content may be a plain string or an array of blocks.
    raw_content=$(grep -m 999 '"type":"user"' "$transcript_path" 2>/dev/null \
        | jq -r 'select(.type == "user" and .message.role == "user")
                 | .message.content
                 | if type == "array"
                   then (map(select(.type == "text") | .text) | first)
                   else .
                   end
                 | select(. != null and . != "")' 2>/dev/null | head -1)
    if [ -n "$raw_content" ]; then
        # Strip newlines, collapse whitespace
        one_line=$(echo "$raw_content" | tr '\n' ' ' | sed 's/[[:space:]]\+/ /g' | sed 's/^ //;s/ $//')
        # Take first 10 words
        words=$(echo "$one_line" | awk '{for(i=1;i<=10&&i<=NF;i++) printf "%s%s",$i,(i<10&&i<NF?" ":""); print ""}')
        word_count=$(echo "$one_line" | wc -w | tr -d ' ')
        if [ "$word_count" -gt 10 ]; then
            summary="${words}…"
        else
            summary="$words"
        fi
        # Hard cap at 52 chars (including ellipsis)
        if [ ${#summary} -gt 52 ]; then
            summary="${summary:0:51}…"
        fi
    fi
fi

# --- 3. Token count ---
tokens=0
# Try .cost.total_tokens first
total=$(echo "$input" | jq -r '.cost.total_tokens // empty' 2>/dev/null)
if [ -n "$total" ] && [ "$total" != "null" ]; then
    tokens="$total"
else
    # Sum the individual cost fields (treat null as 0)
    tokens=$(echo "$input" | jq -r '
        ( (.cost.input_tokens              // 0)
        + (.cost.output_tokens             // 0)
        + (.cost.cache_creation_input_tokens // 0)
        + (.cost.cache_read_input_tokens   // 0)
        ) | floor' 2>/dev/null || echo 0)
fi

# Format token count compactly
fmt_tokens() {
    local n="$1"
    # Remove any decimal part for comparison
    local n_int=${n%.*}
    n_int=${n_int:-0}
    if [ "$n_int" -ge 1000000 ] 2>/dev/null; then
        awk -v n="$n_int" 'BEGIN{printf "%.1fM", n/1000000}'
    elif [ "$n_int" -ge 1000 ] 2>/dev/null; then
        awk -v n="$n_int" 'BEGIN{printf "%.1fk", n/1000}'
    else
        echo "${n_int}"
    fi
}
tok_display=$(fmt_tokens "$tokens")

# --- Output ---
printf "%s · %s · %s" "$branch" "$summary" "$tok_display"
