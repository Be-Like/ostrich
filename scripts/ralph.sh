#!/usr/bin/env sh

# Exit on error
set -e
# (Optional) Treat unset variables as errors
set -u

max_iterations=${1:-}
prompt_md_file_path=${2:-}
sentinel=${3:-}

if [ -z "$max_iterations" ] || [ -z "$prompt_md_file_path" ] || [ -z "$sentinel" ]; then
  echo "Usage: $0 <max-iterations> <prompt-md-file-path> <sentinel>"
  echo "Examples:"
  echo "  max-iterations: 10"
  echo "  prompt-md-file-path: RALPH_PROMPT.md"
  echo "  sentinel: <promise>RALPH_BREAK_LOOP</promise>"
  exit 1
fi


echo "Ralph Setup"
echo "  max-iterations: $max_iterations"
echo "  prompt-md-file-path: $prompt_md_file_path"
echo "  sentinel: $sentinel"
echo

i=0
while [ "$i" -lt $max_iterations ]; do
  echo "Iteration $i"
  echo "-----------------------------------------------------"
  cat "$prompt_md_file_path" | claude --model sonnet --allow-dangerously-skip-permissions --dangerously-skip-permissions --print --output-format stream-json --include-partial-messages --verbose | claude-json-pp --sentinel "$sentinel"
  status_code=$?

  if [ "$status_code" -eq 1 ]; then
    echo "Sentinel detected - breaking out of Ralph loop"
    break
  elif [ "$status_code" -gt 1 ]; then
    echo "claude-json-pp error ($status_code) - breaking out of Ralph loop"
    break
  fi

  # next iteration
  i=$((i + 1))
done

echo "Iteration $i"
echo "-----------------------------------------------------"
echo "HIT MAX ITERATION ($max_iterations)"
echo "Exiting..."

exit 0
